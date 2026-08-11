#include "gn-concord-community-service.h"

#include <json-glib/json-glib.h>
#include <nip_concord.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <nostr/nip19/nip19.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* One Chat Plane page. The Community's own relays bound the fetch; this is the
 * per-Channel ceiling on a backfill, not a protocol constant. */
#define CONCORD_CHAT_PAGE 200
#define CONCORD_STORAGE_KEY "memberships"

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrEvent, nostr_event_free)

typedef struct {
  gchar *community_id;
  gchar *owner;
  gchar *owner_salt;
  gchar *community_root; /* secret, hex */
  guint64 root_epoch;
  gchar *control_pk;     /* NULL on a legacy, pre-split Community */
  gchar *name;
  GnConcordCommunityItem *item;
  GHashTable *messages;  /* channel id -> GListStore(GnConcordMessageItem) */
  GHashTable *seen;      /* rumor id -> GINT_TO_POINTER(1) */
  GHashTable *subscriptions; /* channel id -> subscription id in a guint64 box */
} CommunityState;

typedef struct {
  GnConcordCommunityService *service; /* borrowed; the service owns the sub */
  gchar *community_id;
  gchar *channel_id;
} StreamBinding;

struct _GnConcordCommunityService {
  GObject parent_instance;
  GnostrPluginContext *context; /* host-owned */
  gchar *offline_user_pubkey;
  GListStore *communities;
  GHashTable *states; /* community id -> CommunityState */
  gboolean shutting_down;
};

enum { COMMUNITY_UPDATED, ERROR_REPORTED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(GnConcordCommunityService, gn_concord_community_service,
              G_TYPE_OBJECT)

static void save_memberships(GnConcordCommunityService *self);

/* ------------------------------------------------------------------ *
 * small helpers
 * ------------------------------------------------------------------ */

static void emit_error(GnConcordCommunityService *self, const char *message) {
  g_warning("Concord: %s", message);
  g_signal_emit(self, signals[ERROR_REPORTED], 0, message);
}

static void emit_update(GnConcordCommunityService *self,
                        const char *community_id,
                        GnConcordUpdateFlags flags) {
  g_signal_emit(self, signals[COMMUNITY_UPDATED], 0, community_id,
                (guint)flags);
}

/* Wipes a secret before returning it to the allocator. A `community_root` in
 * memory is a membership credential (NIP-CAS-0008 Security §1). */
static void clear_secret(gchar **secret) {
  if (!secret || !*secret) return;
  memset(*secret, 0, strlen(*secret));
  g_clear_pointer(secret, g_free);
}

static void community_state_free(gpointer data) {
  CommunityState *state = data;
  if (!state) return;
  g_free(state->community_id);
  g_free(state->owner);
  g_free(state->owner_salt);
  clear_secret(&state->community_root);
  g_free(state->control_pk);
  g_free(state->name);
  g_clear_object(&state->item);
  g_clear_pointer(&state->messages, g_hash_table_unref);
  g_clear_pointer(&state->seen, g_hash_table_unref);
  g_clear_pointer(&state->subscriptions, g_hash_table_unref);
  g_free(state);
}

static CommunityState *community_state_new(void) {
  CommunityState *state = g_new0(CommunityState, 1);
  state->messages = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          g_object_unref);
  state->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  state->subscriptions =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  return state;
}

static CommunityState *find_state(GnConcordCommunityService *self,
                                  const char *community_id) {
  return community_id ? g_hash_table_lookup(self->states, community_id) : NULL;
}

static const char *event_tag_value(const NostrEvent *event, const char *key) {
  NostrTags *tags = nostr_event_get_tags(event);
  if (!tags) return NULL;
  for (gsize i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (tag && nostr_tag_size(tag) >= 2 &&
        g_strcmp0(nostr_tag_get(tag, 0), key) == 0)
      return nostr_tag_get(tag, 1);
  }
  return NULL;
}

static gchar *build_stream_filter(const char *stream_pubkey, guint limit) {
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "kinds");
  json_builder_begin_array(builder);
  json_builder_add_int_value(builder, CONCORD_STREAM_WRAP);
  json_builder_end_array(builder);
  json_builder_set_member_name(builder, "authors");
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, stream_pubkey);
  json_builder_end_array(builder);
  if (limit) {
    json_builder_set_member_name(builder, "limit");
    json_builder_add_int_value(builder, limit);
  }
  json_builder_end_object(builder);
  g_autoptr(JsonGenerator) generator = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  gchar *json = json_generator_to_data(generator, NULL);
  json_node_free(root);
  return json;
}

/* Parses and fully verifies a signed event: the id must recompute and the
 * signature must check out. */
static NostrEvent *parse_verified_event(const char *event_json) {
  if (!event_json) return NULL;
  NostrEvent *event = nostr_event_new();
  if (!event || !nostr_event_deserialize_compact(event, event_json, NULL) ||
      nostr_event_validate(event, NULL) != NOSTR_EVENT_VALIDATION_OK) {
    nostr_event_free(event);
    return NULL;
  }
  return event;
}

/* Parses an *unsigned* rumor. A rumor carries no signature by construction
 * (CORD-01): the enclosing seal's signature is what proves its author, so the
 * only check here is that the JSON is a well-formed event. */
static NostrEvent *parse_rumor(const char *rumor_json) {
  if (!rumor_json) return NULL;
  NostrEvent *event = nostr_event_new();
  if (!event || !nostr_event_deserialize_compact(event, rumor_json, NULL)) {
    nostr_event_free(event);
    return NULL;
  }
  return event;
}

static gboolean parse_epoch(const char *value, guint64 *out) {
  if (!value || !*value) return FALSE;
  /* Tag values are decimal with no leading zeros (CORD-01 "Encoding"). */
  if (value[0] == '0' && value[1] != '\0') return FALSE;
  return g_ascii_string_to_unsigned(value, 10, 0, G_MAXUINT64, out, NULL);
}

/* ------------------------------------------------------------------ *
 * derivations
 * ------------------------------------------------------------------ */

/* CORD-03 §1: a Channel's plane derives from its own key when private, and
 * from the community_root when public — one label, two secrets. */
static gboolean derive_channel_key(CommunityState *state,
                                   GnConcordChannelItem *channel,
                                   nostr_concord_group_key_t *out) {
  const char *key_hex = gn_concord_channel_item_get_key(channel);
  if (!key_hex || !*key_hex) key_hex = state->community_root;

  uint8_t secret[32], channel_id[32];
  if (!nostr_concord_hex_decode_32(key_hex, secret) ||
      !nostr_concord_hex_decode_32(
        gn_concord_channel_item_get_id(channel), channel_id))
    return FALSE;

  nostr_concord_status_t status = nostr_concord_channel_key(
    secret, channel_id, gn_concord_channel_item_get_epoch(channel), out);
  memset(secret, 0, sizeof(secret));
  return status == NOSTR_CONCORD_OK;
}

static GListStore *channel_messages(CommunityState *state,
                                    const char *channel_id) {
  GListStore *store = g_hash_table_lookup(state->messages, channel_id);
  if (!store) {
    store = g_list_store_new(GN_TYPE_CONCORD_MESSAGE_ITEM);
    g_hash_table_insert(state->messages, g_strdup(channel_id), store);
  }
  return store;
}

/* ------------------------------------------------------------------ *
 * the read path: wrap -> seal -> rumor
 * ------------------------------------------------------------------ */

static void insert_message_ordered(GListStore *store,
                                   GnConcordMessageItem *item) {
  guint n = g_list_model_get_n_items(G_LIST_MODEL(store));
  guint position = n;
  for (guint i = 0; i < n; i++) {
    g_autoptr(GnConcordMessageItem) current =
      g_list_model_get_item(G_LIST_MODEL(store), i);
    gint64 a = gn_concord_message_item_get_order_key(item);
    gint64 b = gn_concord_message_item_get_order_key(current);
    /* Entries tying on time break by the lower rumor id — the inner event's,
     * never the outer wrap's, which differs per re-wrap (CORD-02 §5). */
    if (a < b || (a == b &&
        g_strcmp0(gn_concord_message_item_get_rumor_id(item),
                  gn_concord_message_item_get_rumor_id(current)) < 0)) {
      position = i;
      break;
    }
  }
  g_list_store_insert(store, position, item);
}

gboolean gn_concord_community_service_ingest_wrap(
    GnConcordCommunityService *self, const char *community_id,
    const char *channel_id, const char *wrap_json) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), FALSE);
  if (self->shutting_down) return FALSE;

  CommunityState *state = find_state(self, community_id);
  if (!state || !state->item) return FALSE;
  g_autoptr(GnConcordChannelItem) channel =
    gn_concord_community_item_find_channel(state->item, channel_id);
  if (!channel) return FALSE;

  /* Every cleanup-attributed local is declared before the first `goto`: a
   * jump over such a declaration leaves its cleanup handler facing
   * uninitialized memory. */
  nostr_concord_group_key_t key;
  gboolean accepted = FALSE;
  char *seal_json = NULL;
  char *rumor_json = NULL;
  g_autoptr(NostrEvent) wrap = NULL;
  g_autoptr(NostrEvent) seal = NULL;
  g_autoptr(NostrEvent) rumor = NULL;
  g_autofree gchar *rumor_id = NULL;
  g_autoptr(GnConcordMessageItem) item = NULL;
  char stream_pk_hex[65];
  const char *ms_tag = NULL;
  guint64 rumor_epoch = 0;
  gint64 created_at = 0;
  gint64 order_key = 0;
  int ms = 0;
  int kind = 0;

  if (!derive_channel_key(state, channel, &key)) return FALSE;

  /* The wrap is signed by the shared stream key, so a valid signature proves
   * only that *a* keyholder published here (CORD-01 "Binding"). */
  wrap = parse_verified_event(wrap_json);
  if (!wrap) goto done;
  if (nostr_event_get_kind(wrap) != CONCORD_STREAM_WRAP) goto done;

  /* The wrap must sit at this Channel's derived address. The subscription
   * filter already says so, but ingest is reachable without one. */
  nostr_concord_hex_encode_32(key.pk, stream_pk_hex);
  if (g_strcmp0(nostr_event_get_pubkey(wrap), stream_pk_hex) != 0) goto done;

  if (nostr_concord_stream_open(key.conv_key, nostr_event_get_content(wrap),
                                &seal_json) != NOSTR_CONCORD_OK)
    goto done;

  /* CORD-02 §5: the Chat Plane's seals MUST be encrypted (kind 20013). A
   * plaintext seal here would leave the rumor liftable as a public artifact. */
  seal = parse_verified_event(seal_json);
  if (!seal || nostr_event_get_kind(seal) != CONCORD_SEAL_ENCRYPTED) goto done;

  if (nostr_concord_stream_open(key.conv_key, nostr_event_get_content(seal),
                                &rumor_json) != NOSTR_CONCORD_OK)
    goto done;

  rumor = parse_rumor(rumor_json);
  if (!rumor) goto done;

  /* The seal's signature is the author proof; a rumor claiming a different
   * author than the seal that carried it is a forgery attempt. */
  if (g_strcmp0(nostr_event_get_pubkey(rumor),
                nostr_event_get_pubkey(seal)) != 0)
    goto done;

  kind = nostr_event_get_kind(rumor);
  if (kind != CONCORD_KIND_MESSAGE && kind != CONCORD_KIND_THREADED_REPLY)
    goto done;

  /* NIP-CAS-0008 (CORD-03): every chat rumor MUST commit its channel and
   * epoch inside the signed rumor, checked against the key that opened the
   * wrap — no cross-channel or cross-epoch replay. */
  if (g_strcmp0(event_tag_value(rumor, "channel"), channel_id) != 0 ||
      !parse_epoch(event_tag_value(rumor, "epoch"), &rumor_epoch) ||
      rumor_epoch != gn_concord_channel_item_get_epoch(channel))
    goto done;

  /* Sub-second ordering rides the tag, never a mutated created_at. An ms
   * outside 0..999 is malformed and its entry is dropped, not interpreted. */
  ms_tag = event_tag_value(rumor, "ms");
  if (ms_tag && !nostr_concord_parse_ms(ms_tag, &ms)) goto done;

  created_at = nostr_event_get_created_at(rumor);
  if (!nostr_concord_order_key(created_at, ms, &order_key)) goto done;

  rumor_id = nostr_event_get_id(rumor);
  if (!rumor_id || g_hash_table_contains(state->seen, rumor_id)) goto done;

  g_hash_table_add(state->seen, g_strdup(rumor_id));
  item = gn_concord_message_item_new(
    rumor_id, nostr_event_get_pubkey(rumor), nostr_event_get_content(rumor),
    created_at, ms, order_key, kind);
  insert_message_ordered(channel_messages(state, channel_id), item);
  accepted = TRUE;

done:
  if (seal_json) {
    memset(seal_json, 0, strlen(seal_json));
    free(seal_json);
  }
  if (rumor_json) {
    memset(rumor_json, 0, strlen(rumor_json));
    free(rumor_json);
  }
  nostr_concord_group_key_clear(&key);
  if (accepted) emit_update(self, community_id, GN_CONCORD_UPDATE_MESSAGES);
  return accepted;
}

static void on_stream_event(const char *event_json, gpointer user_data) {
  StreamBinding *binding = user_data;
  if (!binding || !binding->service ||
      !GN_IS_CONCORD_COMMUNITY_SERVICE(binding->service))
    return;
  gn_concord_community_service_ingest_wrap(
    binding->service, binding->community_id, binding->channel_id, event_json);
}

static void stream_binding_free(gpointer data) {
  StreamBinding *binding = data;
  if (!binding) return;
  g_free(binding->community_id);
  g_free(binding->channel_id);
  g_free(binding);
}

/* ------------------------------------------------------------------ *
 * subscriptions and backfill
 * ------------------------------------------------------------------ */

static void subscribe_channel(GnConcordCommunityService *self,
                              CommunityState *state,
                              GnConcordChannelItem *channel) {
  if (!self->context || self->shutting_down) return;
  const char *channel_id = gn_concord_channel_item_get_id(channel);
  if (g_hash_table_contains(state->subscriptions, channel_id)) return;
  const char *stream_pk = gn_concord_channel_item_get_stream_pubkey(channel);
  if (!stream_pk) return;

  StreamBinding *binding = g_new0(StreamBinding, 1);
  binding->service = self;
  binding->community_id = g_strdup(state->community_id);
  binding->channel_id = g_strdup(channel_id);

  g_autofree gchar *filter = build_stream_filter(stream_pk, 0);
  guint64 id = gnostr_plugin_context_subscribe_events(
    self->context, filter, G_CALLBACK(on_stream_event), binding,
    stream_binding_free);
  if (!id) {
    stream_binding_free(binding);
    return;
  }
  guint64 *boxed = g_new(guint64, 1);
  *boxed = id;
  g_hash_table_insert(state->subscriptions, g_strdup(channel_id), boxed);
}

void gn_concord_community_service_refresh_channel(
    GnConcordCommunityService *self, const char *community_id,
    const char *channel_id) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self));
  if (!self->context || self->shutting_down) return;
  CommunityState *state = find_state(self, community_id);
  if (!state || !state->item) return;
  g_autoptr(GnConcordChannelItem) channel =
    gn_concord_community_item_find_channel(state->item, channel_id);
  if (!channel) return;
  const char *stream_pk = gn_concord_channel_item_get_stream_pubkey(channel);
  if (!stream_pk) return;

  subscribe_channel(self, state, channel);

  g_autoptr(GError) error = NULL;
  g_autofree gchar *filter = build_stream_filter(stream_pk, CONCORD_CHAT_PAGE);
  g_autoptr(GPtrArray) events =
    gnostr_plugin_context_query_events(self->context, filter, &error);
  if (!events) {
    if (error) emit_error(self, error->message);
    return;
  }
  for (guint i = 0; i < events->len; i++)
    gn_concord_community_service_ingest_wrap(
      self, community_id, channel_id, g_ptr_array_index(events, i));
}

void gn_concord_community_service_refresh(GnConcordCommunityService *self) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self));
  if (!self->context || self->shutting_down) return;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->states);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    CommunityState *state = value;
    if (!state->item) continue;
    GListModel *channels = gn_concord_community_item_get_channels(state->item);
    guint n = g_list_model_get_n_items(channels);
    for (guint i = 0; i < n; i++) {
      g_autoptr(GnConcordChannelItem) channel =
        g_list_model_get_item(channels, i);
      gn_concord_community_service_refresh_channel(
        self, state->community_id, gn_concord_channel_item_get_id(channel));
    }
  }
}

/* ------------------------------------------------------------------ *
 * bundles (CORD-05 §1)
 * ------------------------------------------------------------------ */

static const char *object_string(JsonObject *object, const char *member) {
  if (!json_object_has_member(object, member)) return NULL;
  JsonNode *node = json_object_get_member(object, member);
  if (!node || json_node_get_value_type(node) != G_TYPE_STRING) return NULL;
  return json_node_get_string(node);
}

static gint64 object_int(JsonObject *object, const char *member,
                         gint64 fallback) {
  if (!json_object_has_member(object, member)) return fallback;
  JsonNode *node = json_object_get_member(object, member);
  if (!node || json_node_get_value_type(node) != G_TYPE_INT64) return fallback;
  return json_node_get_int(node);
}

static void bind_channel_stream(CommunityState *state,
                                GnConcordChannelItem *channel) {
  nostr_concord_group_key_t key;
  if (!derive_channel_key(state, channel, &key)) return;
  char pk_hex[65];
  nostr_concord_hex_encode_32(key.pk, pk_hex);
  gn_concord_channel_item_set_stream_pubkey(channel, pk_hex);
  nostr_concord_group_key_clear(&key);
}

gboolean gn_concord_community_service_accept_bundle(
    GnConcordCommunityService *self, const char *bundle_json, GError **error) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), FALSE);
  g_return_val_if_fail(bundle_json != NULL, FALSE);

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, bundle_json, -1, error)) return FALSE;
  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "The invite bundle is not a JSON object");
    return FALSE;
  }
  JsonObject *bundle = json_node_get_object(root);

  const char *community_id = object_string(bundle, "community_id");
  const char *owner = object_string(bundle, "owner");
  const char *owner_salt = object_string(bundle, "owner_salt");
  const char *community_root = object_string(bundle, "community_root");
  if (!nostr_concord_is_lower_hex_32(community_id) ||
      !nostr_concord_is_lower_hex_32(owner) ||
      !nostr_concord_is_lower_hex_32(owner_salt) ||
      !nostr_concord_is_lower_hex_32(community_root)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "The invite bundle is missing a required 32-byte field");
    return FALSE;
  }

  /* The inviter's identity is irrelevant to trust: the community_id
   * self-certifies the owner, so a bundle cannot smuggle a false owner or a
   * fake key for a real Community (CORD-05 §1). */
  uint8_t id_bytes[32], owner_bytes[32], salt_bytes[32];
  if (!nostr_concord_hex_decode_32(community_id, id_bytes) ||
      !nostr_concord_hex_decode_32(owner, owner_bytes) ||
      !nostr_concord_hex_decode_32(owner_salt, salt_bytes) ||
      !nostr_concord_verify_community_id(id_bytes, owner_bytes, salt_bytes)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "The bundle's owner proof does not reproduce its community_id");
    return FALSE;
  }

  /* Past `expires_at` (unix ms) the preview still renders, joining refuses. */
  gint64 expires_at = object_int(bundle, "expires_at", 0);
  if (expires_at > 0 && expires_at < g_get_real_time() / 1000) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                "This invite expired");
    return FALSE;
  }

  gint64 root_epoch = object_int(bundle, "root_epoch", 0);
  if (root_epoch < 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "The invite bundle carries a negative epoch");
    return FALSE;
  }

  const char *control_pk = object_string(bundle, "control_pk");
  if (control_pk && !nostr_concord_is_lower_hex_32(control_pk)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "The invite bundle carries a malformed control_pk");
    return FALSE;
  }

  CommunityState *state = community_state_new();
  state->community_id = g_strdup(community_id);
  state->owner = g_strdup(owner);
  state->owner_salt = g_strdup(owner_salt);
  state->community_root = g_strdup(community_root);
  state->root_epoch = (guint64)root_epoch;
  state->control_pk = g_strdup(control_pk);
  state->name = g_strdup(object_string(bundle, "name"));
  state->item = gn_concord_community_item_new(
    community_id, owner, state->name, state->root_epoch, control_pk != NULL);

  /* A bundle is attacker-crafted input reached by following a link, so it is
   * bounded before anything is allocated from it (CORD-05 §1). */
  if (json_object_has_member(bundle, "channels")) {
    JsonArray *channels = json_object_get_array_member(bundle, "channels");
    guint n = channels ? json_array_get_length(channels) : 0;
    if (n > CONCORD_MAX_CHANNELS_IN_INVITE) {
      community_state_free(state);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "The invite bundle names more than %d channels",
                  CONCORD_MAX_CHANNELS_IN_INVITE);
      return FALSE;
    }
    for (guint i = 0; i < n; i++) {
      JsonObject *entry = json_array_get_object_element(channels, i);
      if (!entry) continue;
      const char *channel_id = object_string(entry, "id");
      if (!nostr_concord_is_lower_hex_32(channel_id)) continue;
      const char *channel_key = object_string(entry, "key");
      if (channel_key && !nostr_concord_is_lower_hex_32(channel_key)) continue;
      g_autoptr(GnConcordChannelItem) channel = gn_concord_channel_item_new(
        channel_id, channel_key,
        (guint64)object_int(entry, "epoch", root_epoch),
        object_string(entry, "name"), channel_key != NULL);
      bind_channel_stream(state, channel);
      gn_concord_community_item_add_channel(state->item, channel);
    }
  }

  /* Up to 5 stable relays is the recommendation; a longer set is truncated,
   * and a bundle MUST stay usable when it is (CORD-02 §6). */
  if (json_object_has_member(bundle, "relays")) {
    JsonArray *relays = json_object_get_array_member(bundle, "relays");
    guint n = relays ? json_array_get_length(relays) : 0;
    if (n > CONCORD_MAX_RELAYS_IN_BUNDLE) n = CONCORD_MAX_RELAYS_IN_BUNDLE;
    g_autoptr(GPtrArray) urls = g_ptr_array_new();
    for (guint i = 0; i < n; i++) {
      const char *url = json_array_get_string_element(relays, i);
      if (url && *url) g_ptr_array_add(urls, (gpointer)url);
    }
    g_ptr_array_add(urls, NULL);
    gn_concord_community_item_set_relays(
      state->item, (const char *const *)urls->pdata, urls->len - 1);
  }

  /* A re-accepted link refreshes the membership in place (CORD-05 §2: the
   * coordinate is stable, so the same URL survives every rotation). */
  CommunityState *existing = find_state(self, community_id);
  if (existing) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(self->communities));
    for (guint i = 0; i < n; i++) {
      g_autoptr(GnConcordCommunityItem) current =
        g_list_model_get_item(G_LIST_MODEL(self->communities), i);
      if (g_strcmp0(gn_concord_community_item_get_community_id(current),
                    community_id) == 0) {
        g_list_store_remove(self->communities, i);
        break;
      }
    }
  }
  g_hash_table_replace(self->states, g_strdup(community_id), state);
  g_list_store_append(self->communities, state->item);

  save_memberships(self);
  emit_update(self, community_id,
              GN_CONCORD_UPDATE_MEMBERSHIP | GN_CONCORD_UPDATE_CHANNELS);

  /* Only this Community's channels: adopting one membership must not re-sync
   * every other, or restoring N stored memberships costs N² backfills. */
  if (self->context) {
    GListModel *channels = gn_concord_community_item_get_channels(state->item);
    guint n = g_list_model_get_n_items(channels);
    for (guint i = 0; i < n; i++) {
      g_autoptr(GnConcordChannelItem) channel =
        g_list_model_get_item(channels, i);
      gn_concord_community_service_refresh_channel(
        self, community_id, gn_concord_channel_item_get_id(channel));
    }
  }
  return TRUE;
}

/* ------------------------------------------------------------------ *
 * persistence
 *
 * The host API exposes no NIP-44 self-decrypt, so a relay-hosted kind-13302
 * Community List (CORD-02 §8) cannot be read here yet. Memberships live in
 * plugin-local storage instead, in the List's own join-material shape so a
 * later bead can lift the document onto the wire unchanged.
 * ------------------------------------------------------------------ */

static gchar *serialize_membership(CommunityState *state) {
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "community_id");
  json_builder_add_string_value(builder, state->community_id);
  json_builder_set_member_name(builder, "owner");
  json_builder_add_string_value(builder, state->owner);
  json_builder_set_member_name(builder, "owner_salt");
  json_builder_add_string_value(builder, state->owner_salt);
  json_builder_set_member_name(builder, "community_root");
  json_builder_add_string_value(builder, state->community_root);
  json_builder_set_member_name(builder, "root_epoch");
  json_builder_add_int_value(builder, (gint64)state->root_epoch);
  if (state->control_pk) {
    json_builder_set_member_name(builder, "control_pk");
    json_builder_add_string_value(builder, state->control_pk);
  }
  if (state->name) {
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, state->name);
  }

  json_builder_set_member_name(builder, "channels");
  json_builder_begin_array(builder);
  GListModel *channels = gn_concord_community_item_get_channels(state->item);
  guint n = g_list_model_get_n_items(channels);
  for (guint i = 0; i < n; i++) {
    g_autoptr(GnConcordChannelItem) channel =
      g_list_model_get_item(channels, i);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder,
                                  gn_concord_channel_item_get_id(channel));
    if (gn_concord_channel_item_get_key(channel)) {
      json_builder_set_member_name(builder, "key");
      json_builder_add_string_value(builder,
                                    gn_concord_channel_item_get_key(channel));
    }
    json_builder_set_member_name(builder, "epoch");
    json_builder_add_int_value(
      builder, (gint64)gn_concord_channel_item_get_epoch(channel));
    if (gn_concord_channel_item_get_name(channel)) {
      json_builder_set_member_name(builder, "name");
      json_builder_add_string_value(builder,
                                    gn_concord_channel_item_get_name(channel));
    }
    json_builder_end_object(builder);
  }
  json_builder_end_array(builder);

  json_builder_set_member_name(builder, "relays");
  json_builder_begin_array(builder);
  guint n_relays = 0;
  const char *const *relays =
    gn_concord_community_item_get_relays(state->item, &n_relays);
  for (guint i = 0; i < n_relays; i++)
    json_builder_add_string_value(builder, relays[i]);
  json_builder_end_array(builder);
  json_builder_end_object(builder);

  g_autoptr(JsonGenerator) generator = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  gchar *json = json_generator_to_data(generator, NULL);
  json_node_free(root);
  return json;
}

static void save_memberships(GnConcordCommunityService *self) {
  if (!self->context || self->shutting_down) return;
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->states);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    CommunityState *state = value;
    if (!state->item) continue;
    g_autofree gchar *entry = serialize_membership(state);
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, entry, -1, NULL)) continue;
    json_builder_add_value(builder,
                           json_node_copy(json_parser_get_root(parser)));
  }
  json_builder_end_array(builder);

  g_autoptr(JsonGenerator) generator = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  g_autofree gchar *document = json_generator_to_data(generator, NULL);
  json_node_free(root);
  if (!document) return;

  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = g_bytes_new(document, strlen(document));
  if (!gnostr_plugin_context_store_data(self->context, CONCORD_STORAGE_KEY,
                                        bytes, &error) && error)
    emit_error(self, error->message);
}

static void load_memberships(GnConcordCommunityService *self) {
  if (!self->context) return;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes =
    gnostr_plugin_context_load_data(self->context, CONCORD_STORAGE_KEY, &error);
  if (!bytes) return;

  gsize size = 0;
  const char *data = g_bytes_get_data(bytes, &size);
  if (!data || !size) return;
  g_autofree gchar *document = g_strndup(data, size);

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, document, -1, NULL)) return;
  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY(root)) return;
  JsonArray *entries = json_node_get_array(root);
  for (guint i = 0; i < json_array_get_length(entries); i++) {
    JsonNode *entry = json_array_get_element(entries, i);
    if (!entry || !JSON_NODE_HOLDS_OBJECT(entry)) continue;
    g_autoptr(JsonGenerator) generator = json_generator_new();
    json_generator_set_root(generator, entry);
    g_autofree gchar *json = json_generator_to_data(generator, NULL);
    g_autoptr(GError) accept_error = NULL;
    if (json &&
        !gn_concord_community_service_accept_bundle(self, json, &accept_error))
      g_warning("Concord: dropped a stored membership: %s",
                accept_error ? accept_error->message : "unknown reason");
  }
}

/* ------------------------------------------------------------------ *
 * following an invite link (CORD-05 §2)
 * ------------------------------------------------------------------ */

/* `$BASE/invite/<naddr>#<fragment>`. Only the naddr and the fragment are
 * protocol — the base is interchangeable — so the locator is simply the last
 * path segment before the fragment separator. */
static gboolean split_invite_uri(const char *uri, gchar **out_naddr,
                                 gchar **out_fragment) {
  if (!uri) return FALSE;
  g_autofree gchar *trimmed = g_strstrip(g_strdup(uri));
  const char *hash = strchr(trimmed, '#');
  if (!hash || !hash[1]) return FALSE;

  g_autofree gchar *locator = g_strndup(trimmed, (gsize)(hash - trimmed));
  g_strstrip(locator);
  /* Tolerate a trailing slash before the fragment. */
  gsize len = strlen(locator);
  while (len && locator[len - 1] == '/') locator[--len] = '\0';
  const char *slash = strrchr(locator, '/');
  const char *bare = slash ? slash + 1 : locator;
  if (g_str_has_prefix(bare, "nostr:")) bare += strlen("nostr:");
  if (!*bare) return FALSE;

  *out_naddr = g_strdup(bare);
  *out_fragment = g_strdup(hash + 1);
  return TRUE;
}

static gboolean fetch_and_accept_bundle(GnConcordCommunityService *self,
                                        const char *invite_uri,
                                        GError **error) {
  g_autofree gchar *naddr = NULL;
  g_autofree gchar *fragment = NULL;
  if (!split_invite_uri(invite_uri, &naddr, &fragment)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "That is not an invite link: expected <naddr>#<fragment>");
    return FALSE;
  }

  NostrEntityPointer *pointer = NULL;
  if (nostr_nip19_decode_naddr(naddr, &pointer) != 0 || !pointer) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "The invite link's naddr could not be decoded");
    return FALSE;
  }
  /* The bundle is an addressable event at (33301, link_signer, ""): a
   * different author is a different coordinate, which is what makes the
   * locator unsquattable (CORD-05 §2). */
  gboolean coordinate_ok =
    pointer->kind == CONCORD_INVITE_BUNDLE && pointer->public_key &&
    (!pointer->identifier || !*pointer->identifier);
  g_autofree gchar *signer = g_strdup(pointer->public_key);
  nostr_entity_pointer_free(pointer);
  if (!coordinate_ok || !signer) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "The invite link does not name a kind-33301 bundle coordinate");
    return FALSE;
  }

  nostr_concord_invite_fragment_t parsed;
  nostr_concord_status_t status =
    nostr_concord_invite_fragment_parse(fragment, &parsed);
  if (status != NOSTR_CONCORD_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                "The invite fragment is unusable: %s",
                nostr_concord_status_string(status));
    return FALSE;
  }

  if (!self->context) {
    nostr_concord_invite_fragment_clear(&parsed);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                "The Concord service is not connected");
    return FALSE;
  }

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "kinds");
  json_builder_begin_array(builder);
  json_builder_add_int_value(builder, CONCORD_INVITE_BUNDLE);
  json_builder_end_array(builder);
  json_builder_set_member_name(builder, "authors");
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, signer);
  json_builder_end_array(builder);
  json_builder_end_object(builder);
  g_autoptr(JsonGenerator) generator = json_generator_new();
  JsonNode *filter_root = json_builder_get_root(builder);
  json_generator_set_root(generator, filter_root);
  g_autofree gchar *filter = json_generator_to_data(generator, NULL);
  json_node_free(filter_root);

  g_autoptr(GError) query_error = NULL;
  g_autoptr(GPtrArray) events =
    gnostr_plugin_context_query_events(self->context, filter, &query_error);
  gboolean accepted = FALSE;
  gboolean revoked = FALSE;
  for (guint i = 0; events && i < events->len && !accepted; i++) {
    g_autoptr(NostrEvent) bundle_event =
      parse_verified_event(g_ptr_array_index(events, i));
    if (!bundle_event) continue;
    const char *identifier = event_tag_value(bundle_event, "d");
    if (identifier && *identifier) continue;

    /* Retiring a link re-posts the coordinate as a revocation tombstone, so a
     * fetcher finds the grave instead of keys — exactly as durable as the
     * bundle it replaced (CORD-05 §2). */
    const char *vsk = event_tag_value(bundle_event, "vsk");
    if (g_strcmp0(vsk, "9") == 0) {
      revoked = TRUE;
      continue;
    }

    char *bundle_json = NULL;
    if (nostr_concord_invite_bundle_decrypt(
          nostr_event_get_content(bundle_event), parsed.token,
          &bundle_json) != NOSTR_CONCORD_OK)
      continue;
    accepted = gn_concord_community_service_accept_bundle(self, bundle_json,
                                                          error);
    memset(bundle_json, 0, strlen(bundle_json));
    free(bundle_json);
    if (accepted) break;
    /* A bundle that decrypted but failed validation is terminal for this
     * link: report it rather than trying the next coordinate. */
    nostr_concord_invite_fragment_clear(&parsed);
    return FALSE;
  }
  nostr_concord_invite_fragment_clear(&parsed);

  if (!accepted) {
    if (revoked)
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                  "This invite link was revoked");
    else if (query_error)
      g_propagate_error(error, g_steal_pointer(&query_error));
    else
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                  "No invite bundle was found at that link's coordinate");
  }
  return accepted;
}

void gn_concord_community_service_accept_invite_async(
    GnConcordCommunityService *self, const char *invite_uri,
    GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self));
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  if (self->shutting_down || !self->context) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED,
                            "The Concord service is not connected");
    g_object_unref(task);
    return;
  }
  g_autoptr(GError) error = NULL;
  if (fetch_and_accept_bundle(self, invite_uri, &error))
    g_task_return_boolean(task, TRUE);
  else if (error)
    g_task_return_error(task, g_steal_pointer(&error));
  else
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "The invite could not be accepted");
  g_object_unref(task);
}

gboolean gn_concord_community_service_accept_invite_finish(
    GnConcordCommunityService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ------------------------------------------------------------------ *
 * the write path: rumor -> seal -> wrap (CORD-01)
 * ------------------------------------------------------------------ */

typedef struct {
  gchar *community_id;
  gchar *channel_id;
  gchar *author;
  nostr_concord_group_key_t key;
} PublishContext;

static void publish_context_free(gpointer data) {
  PublishContext *publish = data;
  if (!publish) return;
  g_free(publish->community_id);
  g_free(publish->channel_id);
  g_free(publish->author);
  nostr_concord_group_key_clear(&publish->key);
  g_free(publish);
}

static void on_wrap_published(GObject *source, GAsyncResult *result,
                              gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  GnConcordCommunityService *self = g_task_get_source_object(task);
  if (self->shutting_down || !self->context) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "The Concord service was deactivated");
    g_object_unref(task);
    return;
  }
  g_autoptr(GError) error = NULL;
  if (!gnostr_plugin_context_publish_event_finish(self->context, result,
                                                  &error))
    g_task_return_error(task, g_steal_pointer(&error));
  else
    g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

/* The wrap reverses NIP-59: a fixed author (the stream key, which every
 * keyholder has) and an ephemeral, single-use `p` tag (CORD-01). */
static NostrEvent *build_wrap(PublishContext *publish, const char *seal_json,
                              GError **error) {
  char *content = NULL;
  if (nostr_concord_stream_seal(publish->key.conv_key, seal_json, &content) !=
      NOSTR_CONCORD_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to encrypt the stream wrap");
    return NULL;
  }

  g_autofree gchar *ephemeral_sk = nostr_key_generate_private();
  g_autofree gchar *ephemeral_pk =
    ephemeral_sk ? nostr_key_get_public(ephemeral_sk) : NULL;
  if (ephemeral_sk) memset(ephemeral_sk, 0, strlen(ephemeral_sk));
  if (!ephemeral_pk) {
    free(content);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to mint the wrap's ephemeral recipient");
    return NULL;
  }

  char stream_sk_hex[65], stream_pk_hex[65];
  nostr_concord_hex_encode_32(publish->key.sk, stream_sk_hex);
  nostr_concord_hex_encode_32(publish->key.pk, stream_pk_hex);

  NostrEvent *wrap = nostr_event_new();
  nostr_event_set_kind(wrap, CONCORD_STREAM_WRAP);
  nostr_event_set_pubkey(wrap, stream_pk_hex);
  /* created_at is untweaked: Concord's sub-second ordering rides the rumor's
   * ms tag, never a mutated timestamp (CORD-01 "Encoding"). */
  nostr_event_set_created_at(wrap, (gint64)time(NULL));
  nostr_event_set_content(wrap, content);
  nostr_event_set_tags(wrap, nostr_tags_new(1, nostr_tag_new("p", ephemeral_pk,
                                                             NULL)));
  free(content);

  int signed_ok = nostr_event_sign(wrap, stream_sk_hex);
  memset(stream_sk_hex, 0, sizeof(stream_sk_hex));
  if (signed_ok != 0) {
    nostr_event_free(wrap);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to sign the stream wrap");
    return NULL;
  }
  return wrap;
}

static void on_seal_signed(GObject *source, GAsyncResult *result,
                           gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  GnConcordCommunityService *self = g_task_get_source_object(task);
  PublishContext *publish = g_task_get_task_data(task);

  if (self->shutting_down || !self->context) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "The Concord service was deactivated");
    g_object_unref(task);
    return;
  }

  g_autoptr(GError) error = NULL;
  g_autofree gchar *seal_json = gnostr_plugin_context_request_sign_event_finish(
    self->context, result, &error);
  if (!seal_json) {
    if (error) g_task_return_error(task, g_steal_pointer(&error));
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "Failed to sign the Concord seal");
    g_object_unref(task);
    return;
  }

  /* The signer is the host's, not ours: re-verify that what came back is the
   * seal we asked for, signed by the identity we built it for. */
  g_autoptr(NostrEvent) seal = parse_verified_event(seal_json);
  if (!seal || nostr_event_get_kind(seal) != CONCORD_SEAL_ENCRYPTED ||
      g_strcmp0(nostr_event_get_pubkey(seal), publish->author) != 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "The signer returned a different event");
    g_object_unref(task);
    return;
  }

  g_autoptr(NostrEvent) wrap = build_wrap(publish, seal_json, &error);
  if (!wrap) {
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }
  g_autofree gchar *wrap_json = nostr_event_serialize_compact(wrap);
  if (!wrap_json) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to serialize the stream wrap");
    g_object_unref(task);
    return;
  }
  gnostr_plugin_context_publish_event_async(
    self->context, wrap_json, g_task_get_cancellable(task),
    on_wrap_published, task);
}

static void return_publish_error(GnConcordCommunityService *self,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data, GIOErrorEnum code,
                                 const char *message) {
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_return_new_error(task, G_IO_ERROR, code, "%s", message);
  g_object_unref(task);
}

void gn_concord_community_service_publish_message_async(
    GnConcordCommunityService *self, const char *community_id,
    const char *channel_id, const char *content, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self));

  const char *author = gn_concord_community_service_get_current_pubkey(self);
  CommunityState *state = find_state(self, community_id);
  g_autoptr(GnConcordChannelItem) channel =
    state && state->item
      ? gn_concord_community_item_find_channel(state->item, channel_id)
      : NULL;
  if (!author || !content || !*content || !channel) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_PERMISSION_DENIED,
                         "Sign in and pick a channel you hold the key for");
    return;
  }
  if (self->shutting_down || !self->context) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_CLOSED,
                         "The Concord service is not connected");
    return;
  }

  PublishContext *publish = g_new0(PublishContext, 1);
  publish->community_id = g_strdup(community_id);
  publish->channel_id = g_strdup(channel_id);
  publish->author = g_strdup(author);
  if (!derive_channel_key(state, channel, &publish->key)) {
    publish_context_free(publish);
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_FAILED,
                         "This channel's key could not be derived");
    return;
  }

  /* The rumor commits its channel and epoch so a keyholder cannot re-publish
   * it into another Channel or epoch (NIP-CAS-0008, CORD-03). */
  gint64 now_us = g_get_real_time();
  gint64 created_at = now_us / G_USEC_PER_SEC;
  int ms = (int)((now_us / 1000) % 1000);
  g_autofree gchar *epoch_text =
    g_strdup_printf("%" G_GUINT64_FORMAT,
                    gn_concord_channel_item_get_epoch(channel));
  g_autofree gchar *ms_text = g_strdup_printf("%d", ms);

  g_autoptr(NostrEvent) rumor = nostr_event_new();
  nostr_event_set_kind(rumor, CONCORD_KIND_MESSAGE);
  nostr_event_set_pubkey(rumor, author);
  nostr_event_set_created_at(rumor, created_at);
  nostr_event_set_content(rumor, content);
  nostr_event_set_tags(rumor, nostr_tags_new(
    3,
    nostr_tag_new("channel", channel_id, NULL),
    nostr_tag_new("epoch", epoch_text, NULL),
    nostr_tag_new("ms", ms_text, NULL)));

  g_autofree gchar *rumor_json = nostr_event_serialize_compact(rumor);
  char *seal_content = NULL;
  if (!rumor_json ||
      nostr_concord_stream_seal(publish->key.conv_key, rumor_json,
                                &seal_content) != NOSTR_CONCORD_OK) {
    publish_context_free(publish);
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_FAILED,
                         "Failed to encrypt the message rumor");
    return;
  }

  /* The seal is signed by the author's real key — that signature is what
   * proves who wrote the rumor, and it is the only place the identity
   * appears (CORD-01). */
  g_autoptr(NostrEvent) seal = nostr_event_new();
  nostr_event_set_kind(seal, CONCORD_SEAL_ENCRYPTED);
  nostr_event_set_pubkey(seal, author);
  nostr_event_set_created_at(seal, created_at);
  nostr_event_set_content(seal, seal_content);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  free(seal_content);

  g_autofree gchar *seal_json = nostr_event_serialize_compact(seal);
  if (!seal_json) {
    publish_context_free(publish);
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_FAILED,
                         "Failed to serialize the Concord seal");
    return;
  }

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_task_data(task, publish, publish_context_free);
  gnostr_plugin_context_request_sign_event(self->context, seal_json,
                                           cancellable, on_seal_signed, task);
}

gboolean gn_concord_community_service_publish_message_finish(
    GnConcordCommunityService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ------------------------------------------------------------------ *
 * lifecycle and accessors
 * ------------------------------------------------------------------ */

void gn_concord_community_service_shutdown(GnConcordCommunityService *self) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self));
  if (self->shutting_down) return;
  self->shutting_down = TRUE;

  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->states);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    CommunityState *state = value;
    if (!state->subscriptions) continue;
    if (self->context) {
      GHashTableIter subs;
      gpointer sub_value;
      g_hash_table_iter_init(&subs, state->subscriptions);
      while (g_hash_table_iter_next(&subs, NULL, &sub_value))
        gnostr_plugin_context_unsubscribe_events(self->context,
                                                 *(guint64 *)sub_value);
    }
    g_hash_table_remove_all(state->subscriptions);
  }
  /* The context is borrowed from the host and may be freed immediately after
   * deactivation. Late async completions must observe NULL and cancel. */
  self->context = NULL;
}

static void gn_concord_community_service_dispose(GObject *object) {
  GnConcordCommunityService *self = GN_CONCORD_COMMUNITY_SERVICE(object);
  gn_concord_community_service_shutdown(self);
  g_clear_object(&self->communities);
  g_clear_pointer(&self->states, g_hash_table_unref);
  self->context = NULL;
  G_OBJECT_CLASS(gn_concord_community_service_parent_class)->dispose(object);
}

static void gn_concord_community_service_finalize(GObject *object) {
  GnConcordCommunityService *self = GN_CONCORD_COMMUNITY_SERVICE(object);
  g_free(self->offline_user_pubkey);
  G_OBJECT_CLASS(gn_concord_community_service_parent_class)->finalize(object);
}

static void gn_concord_community_service_class_init(
    GnConcordCommunityServiceClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gn_concord_community_service_dispose;
  object_class->finalize = gn_concord_community_service_finalize;
  signals[COMMUNITY_UPDATED] = g_signal_new(
    "community-updated", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
    NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_UINT);
  signals[ERROR_REPORTED] = g_signal_new(
    "error-reported", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
    NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gn_concord_community_service_init(
    GnConcordCommunityService *self) {
  self->communities = g_list_store_new(GN_TYPE_CONCORD_COMMUNITY_ITEM);
  self->states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       community_state_free);
}

GnConcordCommunityService *gn_concord_community_service_new(
    GnostrPluginContext *context) {
  g_return_val_if_fail(context != NULL, NULL);
  GnConcordCommunityService *self =
    g_object_new(GN_TYPE_CONCORD_COMMUNITY_SERVICE, NULL);
  self->context = context;
  load_memberships(self);
  gn_concord_community_service_refresh(self);
  return self;
}

GnConcordCommunityService *gn_concord_community_service_new_offline(
    const char *user_pubkey) {
  GnConcordCommunityService *self =
    g_object_new(GN_TYPE_CONCORD_COMMUNITY_SERVICE, NULL);
  self->offline_user_pubkey = g_strdup(user_pubkey);
  return self;
}

GListModel *gn_concord_community_service_get_model(
    GnConcordCommunityService *self) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), NULL);
  return G_LIST_MODEL(self->communities);
}

GnConcordCommunityItem *gn_concord_community_service_lookup_community(
    GnConcordCommunityService *self, const char *community_id) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), NULL);
  CommunityState *state = find_state(self, community_id);
  return state && state->item ? g_object_ref(state->item) : NULL;
}

GListModel *gn_concord_community_service_get_messages(
    GnConcordCommunityService *self, const char *community_id,
    const char *channel_id) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), NULL);
  CommunityState *state = find_state(self, community_id);
  if (!state || !channel_id) return NULL;
  return G_LIST_MODEL(channel_messages(state, channel_id));
}

const char *gn_concord_community_service_get_current_pubkey(
    GnConcordCommunityService *self) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), NULL);
  if (self->offline_user_pubkey) return self->offline_user_pubkey;
  return self->context
    ? gnostr_plugin_context_get_user_pubkey(self->context) : NULL;
}
