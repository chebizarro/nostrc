#include "gn-concord-community-service.h"

#include "gn-concord-control-plane.h"
#include "gn-concord-guestbook.h"

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

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrEvent, nostr_event_free)

typedef struct {
  gchar *community_id;
  gchar *owner;
  gchar *owner_salt;
  gchar *community_root; /* secret, hex */
  guint64 root_epoch;
  gchar *control_pk;     /* NULL on a legacy, pre-split Community */
  /* The staff write key (CORD-02 §2). Never in an invite bundle: it arrives
   * inside a staff-making Grant and syncs across its holder's own devices in
   * the Community List. A plain member holds none and needs none. */
  gchar *control_root;
  gchar *name;
  GnConcordCommunityItem *item;
  GHashTable *messages;  /* channel id -> GListStore(GnConcordMessageItem) */
  GHashTable *seen;      /* rumor id -> GINT_TO_POINTER(1) */
  GHashTable *subscriptions; /* channel id -> subscription id in a guint64 box */
  /* The Control Plane fold: the authority over everything the invite bundle
   * only snapshotted at join time (CORD-02 §6, CORD-04). */
  GnConcordControlPlane *control;
  gchar *control_address; /* control_pk, or the legacy derivation's own pk */
  guint64 control_subscription;
  /* The Guestbook: membership motion, off-consensus (CORD-02 §5). */
  GnConcordGuestbook *guestbook;
  gchar *guestbook_address;
  guint64 guestbook_subscription;
  /* CORD-05 §1: the link's optional attribution, echoed in the Join. */
  gchar *invite_creator;
  gchar *invite_label;
  /* CORD-06 §3: the first root_epoch that would pay the Refounding this
   * Community owes — one past the epoch its last live link was retired at —
   * or 0 when it owes none. The debt is outstanding while `root_epoch` has
   * not reached it, so rolling the root is what clears it and nothing has to
   * remember to reset a flag. */
  guint64 refounding_due_epoch;
  /* CORD-06 §2: the next base rotation's address, derived from the root held
   * now at root_epoch + 1, and the rotations arriving there that are not yet
   * complete. A member who missed a rotation also missed the address it
   * published the next one at — which is exactly the removal signal. */
  gchar *rekey_address;
  guint64 rekey_subscription;
  GHashTable *rotations; /* rotation key -> PendingRotation */
  /* Community List bookkeeping (CORD-02 §8) */
  JsonNode *list_seed;    /* the earliest epoch held; only ever moves back */
  JsonObject *list_extra; /* members of the List entry we don't understand */
  gint64 added_at;        /* ms; tiebreaks against a tombstone */
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

  /* The Invite List document as last read from the wire (CORD-05 §4): the
   * creator's private bookkeeping, holding every minted link's unlock token
   * and link signer secret. Retained verbatim so a republish round-trips the
   * tombstones and whatever another client wrote. */
  JsonObject *invite_document;
  gchar *invite_author;   /* the npub the document was read for */
  gboolean invite_loaded; /* a definitive read happened; publishing is safe */

  /* The Community List document as last read from the wire (CORD-02 §8).
   * Retained so a republish round-trips the tombstones and the fields this
   * client doesn't understand rather than deleting another client's work. */
  JsonObject *list_document;
  GHashTable *list_orphans; /* community id -> JsonNode: entries we could not
                             * adopt, re-emitted verbatim */
  gchar *list_author;       /* the npub the document was read for */
  gboolean list_loaded;     /* a definitive read happened; publishing is safe */
  gboolean list_applying;   /* adopting stored entries: do not republish */
};

enum { COMMUNITY_UPDATED, ERROR_REPORTED, INVITE_OFFERED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE(GnConcordCommunityService, gn_concord_community_service,
              G_TYPE_OBJECT)

static void publish_community_list(GnConcordCommunityService *self);
static void load_community_list(GnConcordCommunityService *self);
static JsonNode *build_list_entry(CommunityState *state);
static void refresh_control_plane(GnConcordCommunityService *self,
                                  CommunityState *state);
static void apply_control_fold(GnConcordCommunityService *self,
                              CommunityState *state);
static void refresh_guestbook(GnConcordCommunityService *self,
                              CommunityState *state);
static void refresh_rekey_plane(GnConcordCommunityService *self,
                                CommunityState *state);
static gboolean ensure_guestbook(CommunityState *state);
static gboolean guestbook_kick_authority(const char *actor, const char *target,
                                         gpointer user_data);
static void publish_membership_verb(GnConcordCommunityService *self,
                                    CommunityState *state, const char *verb,
                                    gboolean attribute,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);

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
  clear_secret(&state->control_root);
  g_free(state->control_pk);
  g_free(state->name);
  g_clear_object(&state->item);
  g_clear_pointer(&state->messages, g_hash_table_unref);
  g_clear_pointer(&state->seen, g_hash_table_unref);
  g_clear_pointer(&state->subscriptions, g_hash_table_unref);
  g_clear_pointer(&state->control, gn_concord_control_plane_free);
  g_free(state->control_address);
  g_clear_pointer(&state->guestbook, gn_concord_guestbook_free);
  g_free(state->guestbook_address);
  g_free(state->rekey_address);
  g_clear_pointer(&state->rotations, g_hash_table_unref);
  g_free(state->invite_creator);
  g_free(state->invite_label);
  g_clear_pointer(&state->list_seed, json_node_free);
  g_clear_pointer(&state->list_extra, json_object_unref);
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
 * the write path's shared pieces
 *
 * Every durable Concord plane is written the same way — rumor, seal, wrap
 * (CORD-01) — so Chat and Guestbook share one mint path and one carrier.
 * ------------------------------------------------------------------ */

typedef struct {
  gchar *author;
  nostr_concord_group_key_t key;
} PublishContext;

static void publish_context_free(gpointer data) {
  PublishContext *publish = data;
  if (!publish) return;
  g_free(publish->author);
  nostr_concord_group_key_clear(&publish->key);
  g_free(publish);
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

/* Concord's sub-second ordering rides the rumor's ms tag, never a mutated
 * created_at (CORD-02 §4). */
static int split_now(gint64 *out_created_at) {
  gint64 now_us = g_get_real_time();
  *out_created_at = now_us / G_USEC_PER_SEC;
  return (int)((now_us / 1000) % 1000);
}

/* Seals @rumor to the author's real key and publishes it as a stream wrap on
 * the plane @publish->key addresses. Takes ownership of @publish. */
static void publish_sealed_rumor(GnConcordCommunityService *self,
                                 PublishContext *publish, NostrEvent *rumor,
                                 gint64 created_at, GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);

/* ------------------------------------------------------------------ *
 * derivations
 * ------------------------------------------------------------------ */

/* CORD-03 §1: a Channel's plane derives from its own key when private, and
 * from the community_root when public — one label, two secrets. The folded
 * type decides which, so a Control Plane edition converting a Channel moves
 * it to the other secret at the same channel_id (§2). */
static gboolean derive_channel_key(CommunityState *state,
                                   GnConcordChannelItem *channel,
                                   nostr_concord_group_key_t *out) {
  const char *key_hex = NULL;
  if (gn_concord_channel_item_get_is_private(channel)) {
    /* A private Channel the Control Plane defines but no invite or rekey ever
     * delivered a key for is listed and unreadable — never silently derived
     * from the community_root, which would address a plane nobody writes. */
    key_hex = gn_concord_channel_item_get_key(channel);
    if (!key_hex || !*key_hex) return FALSE;
  } else {
    key_hex = state->community_root;
  }

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

  /* Every honest client drops *every* event from a banned npub — message,
   * reaction, edit, or authority action — so a banned member vanishes
   * entirely (CORD-04 §4). Silencing is instant and free; the read-cut is a
   * rekey, the separate and heavier step. */
  if (state->control &&
      gn_concord_control_plane_is_banned(state->control,
                                         nostr_event_get_pubkey(seal)))
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
  /* Every valid event a client decrypts names its real author, and an author
   * seen publishing is *observably present* — auto-included even if their
   * Join never arrived (CORD-02 §5). */
  if (ensure_guestbook(state))
    gn_concord_guestbook_observe(state->guestbook,
                                 nostr_event_get_pubkey(seal), order_key);
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
    /* Last, and unconditionally: the Control Plane is what tells this member
     * about Channels no invite ever granted, the rekey address whether they
     * are still a member at all, and the Guestbook who else is here. The
     * Guestbook is off-consensus, so it genuinely goes last. */
    refresh_control_plane(self, state);
    refresh_rekey_plane(self, state);
    refresh_guestbook(self, state);
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

/* Typed member accessors, never the *_get_array_member/_get_object_member
 * shortcuts: those log a critical when the member is present with the wrong
 * type, and every document read here is attacker-crafted or foreign-client
 * input where a mistyped member is an expected shape, not a bug. */
static JsonArray *object_array(JsonObject *object, const char *member) {
  if (!object || !json_object_has_member(object, member)) return NULL;
  JsonNode *node = json_object_get_member(object, member);
  return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

static JsonObject *object_object(JsonObject *object, const char *member) {
  if (!object || !json_object_has_member(object, member)) return NULL;
  JsonNode *node = json_object_get_member(object, member);
  return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node)
                                              : NULL;
}

static gchar *node_to_json(JsonNode *node) {
  if (!node) return NULL;
  g_autoptr(JsonGenerator) generator = json_generator_new();
  json_generator_set_root(generator, node);
  return json_generator_to_data(generator, NULL);
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

  /* A staffer's own `control_root` rides the Community List between their own
   * devices (CORD-02 §2, §8) — never an invite bundle, which is why a
   * *link* can never hand one out. Malformed is refused rather than ignored:
   * a wrong write key mints wraps every member drops. */
  const char *control_root = object_string(bundle, "control_root");
  if (control_root && !nostr_concord_is_lower_hex_32(control_root)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "The invite bundle carries a malformed control_root");
    return FALSE;
  }

  CommunityState *state = community_state_new();
  state->community_id = g_strdup(community_id);
  state->owner = g_strdup(owner);
  state->owner_salt = g_strdup(owner_salt);
  state->community_root = g_strdup(community_root);
  state->root_epoch = (guint64)root_epoch;
  state->control_pk = g_strdup(control_pk);
  state->control_root = g_strdup(control_root);
  state->name = g_strdup(object_string(bundle, "name"));
  /* Optional attribution: a link may name its creator and a human label
   * ("Reddit", "Conf 2026"), and an accepting joiner echoes both in their
   * Join, which is what makes per-link usage counters possible (CORD-05 §1). */
  state->invite_creator = g_strdup(object_string(bundle, "creator_npub"));
  state->invite_label = g_strdup(object_string(bundle, "label"));
  state->item = gn_concord_community_item_new(
    community_id, owner, state->name, state->root_epoch, control_pk != NULL);

  /* A bundle is attacker-crafted input reached by following a link, so it is
   * bounded before anything is allocated from it (CORD-05 §1). */
  {
    JsonArray *channels = object_array(bundle, "channels");
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
  {
    JsonArray *relays = object_array(bundle, "relays");
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
   * coordinate is stable, so the same URL survives every rotation). Its List
   * bookkeeping carries over: `added_at` is when this membership began, not
   * when it was last refreshed, and the seed is the *earliest* epoch ever
   * held, so a refresh must never advance it (CORD-02 §8). */
  CommunityState *existing = find_state(self, community_id);
  if (existing) {
    /* A refreshed link carries no `control_root` — no bundle ever does — so a
     * staffer re-accepting their own link must not lose their write key. */
    if (!state->control_root)
      state->control_root = g_steal_pointer(&existing->control_root);
    state->list_seed = g_steal_pointer(&existing->list_seed);
    state->list_extra = g_steal_pointer(&existing->list_extra);
    state->added_at = existing->added_at;
    /* The folded Control Plane survives a refresh of the same epoch — it is
     * the authority the refreshed bundle is only a snapshot of. A Refounding
     * moves the plane's address, so a new epoch starts a new fold. */
    if (existing->root_epoch == state->root_epoch &&
        g_strcmp0(existing->control_pk, state->control_pk) == 0) {
      state->control = g_steal_pointer(&existing->control);
      state->control_address = g_steal_pointer(&existing->control_address);
      state->control_subscription = existing->control_subscription;
      existing->control_subscription = 0;
      state->guestbook = g_steal_pointer(&existing->guestbook);
      state->guestbook_address = g_steal_pointer(&existing->guestbook_address);
      state->guestbook_subscription = existing->guestbook_subscription;
      existing->guestbook_subscription = 0;
      if (state->guestbook)
        gn_concord_guestbook_set_kick_authority(
          state->guestbook, guestbook_kick_authority, state);
    }
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
  /* A Join is announced for a *new* membership only: refreshing a rotated
   * link, or restoring one this npub already holds from the Community List,
   * is not a join and must not re-announce one. */
  gboolean joining = existing == NULL && !self->list_applying;
  if (!state->added_at) state->added_at = g_get_real_time() / 1000;
  g_hash_table_replace(self->states, g_strdup(community_id), state);
  g_list_store_append(self->communities, state->item);
  /* Adopting a membership retires whatever the stored document said about it:
   * the fresh join material is the entry now. */
  if (self->list_orphans) g_hash_table_remove(self->list_orphans, community_id);

  publish_community_list(self);
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
    refresh_control_plane(self, state);
    refresh_guestbook(self, state);
    if (joining)
      publish_membership_verb(self, state, "join", TRUE, NULL, NULL, NULL);
  }
  return TRUE;
}

/* ------------------------------------------------------------------ *
 * the Control Plane (CORD-02 §5, CORD-04)
 *
 * The bundle a member joined with is a join-time snapshot; the fold is the
 * authority. It carries Channels no invite granted, renames, visibility
 * flips, the Roster and the Banlist — so a member who joined a year ago sees
 * the Community as it is, not as it was handed to them.
 * ------------------------------------------------------------------ */

/* Every member derives the *read* key from the community_root; the signer
 * derives from the staff-held control_root, which a member never has and
 * never needs — they hold its pubkey from their invite (CORD-02 §2). */
static gboolean derive_control_read_key(CommunityState *state,
                                        nostr_concord_group_key_t *out) {
  uint8_t root[32], id[32];
  if (!nostr_concord_hex_decode_32(state->community_root, root) ||
      !nostr_concord_hex_decode_32(state->community_id, id))
    return FALSE;
  nostr_concord_status_t status =
    nostr_concord_control_read_key(root, id, state->root_epoch, out);
  memset(root, 0, sizeof(root));
  return status == NOSTR_CONCORD_OK;
}

static gboolean ensure_control_plane(CommunityState *state) {
  if (state->control) return state->control_address != NULL;
  if (!state->community_id || !state->owner) return FALSE;

  state->control =
    gn_concord_control_plane_new(state->community_id, state->owner);
  if (!state->control) return FALSE;

  if (state->control_pk) {
    state->control_address = g_strdup(state->control_pk);
  } else {
    /* An epoch minted before the split had no signer key: the
     * `concord/control` derivation alone was the plane — its pk the address
     * and the wrap signer, its conv_key the encryption. A client MUST retain
     * that use to read such epochs (CORD-02 §5). */
    nostr_concord_group_key_t key;
    if (!derive_control_read_key(state, &key)) return FALSE;
    char address[65];
    nostr_concord_hex_encode_32(key.pk, address);
    state->control_address = g_strdup(address);
    nostr_concord_group_key_clear(&key);
  }
  return TRUE;
}

gboolean gn_concord_community_service_ingest_control_wrap(
    GnConcordCommunityService *self, const char *community_id,
    const char *wrap_json) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), FALSE);
  if (self->shutting_down) return FALSE;
  CommunityState *state = find_state(self, community_id);
  if (!state || !ensure_control_plane(state)) return FALSE;

  nostr_concord_group_key_t key;
  if (!derive_control_read_key(state, &key)) return FALSE;
  gboolean accepted = gn_concord_control_plane_ingest_wrap(
    state->control, key.conv_key, state->control_address, wrap_json);
  nostr_concord_group_key_clear(&key);

  if (accepted) apply_control_fold(self, state);
  return accepted;
}

static void apply_control_fold(GnConcordCommunityService *self,
                               CommunityState *state) {
  if (!state->control || !state->item) return;
  GnConcordUpdateFlags flags = 0;

  /* The name and icon inside an invite bundle are a preview so a parked
   * invite can render; the fold is always the authority (CORD-02 §6). */
  const char *name = gn_concord_control_plane_get_name(state->control);
  if (name &&
      g_strcmp0(name, gn_concord_community_item_get_name(state->item)) != 0) {
    gn_concord_community_item_set_name(state->item, name);
    g_free(state->name);
    state->name = g_strdup(name);
    flags |= GN_CONCORD_UPDATE_MEMBERSHIP;
  }

  const char *description =
    gn_concord_control_plane_get_description(state->control);
  if (description &&
      g_strcmp0(description,
                gn_concord_community_item_get_description(state->item)) != 0) {
    gn_concord_community_item_set_description(state->item, description);
    flags |= GN_CONCORD_UPDATE_MEMBERSHIP;
  }

  /* The relay list lives in the metadata entity so it can evolve: a client
   * follows the fold rather than the join-time copy (CORD-02 §6). */
  guint n_relays = 0;
  const char *const *relays =
    gn_concord_control_plane_get_relays(state->control, &n_relays);
  if (relays) {
    gn_concord_community_item_set_relays(state->item, relays, n_relays);
    flags |= GN_CONCORD_UPDATE_MEMBERSHIP;
  }

  GPtrArray *channels = gn_concord_control_plane_get_channels(state->control);
  for (guint i = 0; channels && i < channels->len; i++) {
    const GnConcordControlChannel *folded = g_ptr_array_index(channels, i);
    g_autoptr(GnConcordChannelItem) existing =
      gn_concord_community_item_find_channel(state->item, folded->channel_id);

    /* Deletion is terminal: clients drop the Channel from display and may
     * discard its keys, and the id is never reused (CORD-03 §2). */
    if (folded->deleted) {
      if (!existing) continue;
      guint64 *subscription =
        g_hash_table_lookup(state->subscriptions, folded->channel_id);
      if (subscription && self->context)
        gnostr_plugin_context_unsubscribe_events(self->context, *subscription);
      g_hash_table_remove(state->subscriptions, folded->channel_id);
      g_hash_table_remove(state->messages, folded->channel_id);
      gn_concord_community_item_remove_channel(state->item,
                                               folded->channel_id);
      flags |= GN_CONCORD_UPDATE_CHANNELS;
      continue;
    }

    if (!existing) {
      /* Keys never travel on the Control Plane, so a Channel learned here
       * arrives keyless: public means derivable from the community_root,
       * private means listed and unreadable until a key is delivered. */
      g_autoptr(GnConcordChannelItem) channel = gn_concord_channel_item_new(
        folded->channel_id, NULL, state->root_epoch, folded->name,
        folded->is_private);
      bind_channel_stream(state, channel);
      gn_concord_community_item_add_channel(state->item, channel);
      flags |= GN_CONCORD_UPDATE_CHANNELS;
      continue;
    }

    if (g_strcmp0(folded->name,
                  gn_concord_channel_item_get_name(existing)) != 0) {
      gn_concord_channel_item_set_name(existing, folded->name);
      flags |= GN_CONCORD_UPDATE_CHANNELS;
    }
    /* A visibility flip moves the Channel to the other secret at the same
     * channel_id, so its address changes and must be re-derived. */
    if (folded->is_private !=
        gn_concord_channel_item_get_is_private(existing)) {
      gn_concord_channel_item_set_is_private(existing, folded->is_private);
      bind_channel_stream(state, existing);
      flags |= GN_CONCORD_UPDATE_CHANNELS;
    }
  }

  if (!flags) return;
  emit_update(self, state->community_id, flags);
  /* A rename or a Channel change moves this membership's join material, and
   * `current` is replaced on every rename (CORD-02 §8). */
  publish_community_list(self);
}

guint32 gn_concord_community_service_get_position(
    GnConcordCommunityService *self, const char *community_id,
    const char *pubkey_hex) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self),
                       CONCORD_POSITION_LAST);
  CommunityState *state = find_state(self, community_id);
  return state && state->control
    ? gn_concord_control_plane_get_position(state->control, pubkey_hex)
    : CONCORD_POSITION_LAST;
}

guint64 gn_concord_community_service_get_permissions(
    GnConcordCommunityService *self, const char *community_id,
    const char *pubkey_hex) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), 0);
  CommunityState *state = find_state(self, community_id);
  return state && state->control
    ? gn_concord_control_plane_get_permissions(state->control, pubkey_hex)
    : 0;
}

GPtrArray *gn_concord_community_service_get_invite_links(
    GnConcordCommunityService *self, const char *community_id) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), NULL);
  CommunityState *state = find_state(self, community_id);
  return state && state->control
    ? gn_concord_control_plane_get_invite_links(state->control)
    : NULL;
}

gboolean gn_concord_community_service_refounding_due(
    GnConcordCommunityService *self, const char *community_id) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), FALSE);
  CommunityState *state = find_state(self, community_id);
  if (!state) return FALSE;
  /* A debt of 0 is no debt, and one the root has already rolled past was
   * paid by that rotation. */
  return state->refounding_due_epoch > state->root_epoch;
}

gboolean gn_concord_community_service_is_public(
    GnConcordCommunityService *self, const char *community_id) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), FALSE);
  CommunityState *state = find_state(self, community_id);
  return state && state->control &&
         gn_concord_control_plane_is_public(state->control);
}

/* ------------------------------------------------------------------ *
 * the Guestbook Plane (CORD-02 §5)
 *
 * Member-writable, unlike Control, because Joins and Leaves are each member's
 * own word. Off-consensus: nothing in Control or Chat depends on it, so it
 * loads last and can lag without harm.
 * ------------------------------------------------------------------ */

static gboolean derive_guestbook_key(CommunityState *state,
                                     nostr_concord_group_key_t *out) {
  uint8_t root[32], id[32];
  if (!nostr_concord_hex_decode_32(state->community_root, root) ||
      !nostr_concord_hex_decode_32(state->community_id, id))
    return FALSE;
  nostr_concord_status_t status =
    nostr_concord_guestbook_key(root, id, state->root_epoch, out);
  memset(root, 0, sizeof(root));
  return status == NOSTR_CONCORD_OK;
}

/* A Kick is honored only if its signer holds KICK and *strictly* outranks its
 * target — equal cannot act on equal (CORD-04 §3). The Guestbook holds no
 * authority of its own, so it asks the Roster. */
static gboolean guestbook_kick_authority(const char *actor, const char *target,
                                         gpointer user_data) {
  CommunityState *state = user_data;
  if (!state->control) return FALSE;
  if ((gn_concord_control_plane_get_permissions(state->control, actor) &
       CONCORD_PERM_KICK) == 0)
    return FALSE;
  return gn_concord_control_plane_get_position(state->control, actor) <
         gn_concord_control_plane_get_position(state->control, target);
}

static gboolean ensure_guestbook(CommunityState *state) {
  if (state->guestbook) return state->guestbook_address != NULL;
  nostr_concord_group_key_t key;
  if (!derive_guestbook_key(state, &key)) return FALSE;
  char address[65];
  nostr_concord_hex_encode_32(key.pk, address);
  nostr_concord_group_key_clear(&key);

  state->guestbook = gn_concord_guestbook_new();
  state->guestbook_address = g_strdup(address);
  gn_concord_guestbook_set_kick_authority(state->guestbook,
                                          guestbook_kick_authority, state);
  return TRUE;
}

gboolean gn_concord_community_service_ingest_guestbook_wrap(
    GnConcordCommunityService *self, const char *community_id,
    const char *wrap_json) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), FALSE);
  if (self->shutting_down) return FALSE;
  CommunityState *state = find_state(self, community_id);
  if (!state || !ensure_guestbook(state)) return FALSE;

  nostr_concord_group_key_t key;
  if (!derive_guestbook_key(state, &key)) return FALSE;
  gboolean changed = gn_concord_guestbook_ingest_wrap(
    state->guestbook, key.conv_key, state->guestbook_address, wrap_json);
  nostr_concord_group_key_clear(&key);

  if (changed) emit_update(self, community_id, GN_CONCORD_UPDATE_MEMBERSHIP);
  return changed;
}

void gn_concord_community_service_set_refounder(
    GnConcordCommunityService *self, const char *community_id,
    const char *pubkey_hex) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self));
  CommunityState *state = find_state(self, community_id);
  if (!state || !ensure_guestbook(state)) return;
  gn_concord_guestbook_set_refounder(state->guestbook, pubkey_hex);
}

GPtrArray *gn_concord_community_service_get_members(
    GnConcordCommunityService *self, const char *community_id) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), NULL);
  CommunityState *state = find_state(self, community_id);
  if (!state || !ensure_guestbook(state)) return g_ptr_array_new();

  /* The coalesced Guestbook, merged with observed authors, minus the
   * Banlist, is the Complete Memberlist (CORD-02 §5). */
  GPtrArray *members = gn_concord_guestbook_get_members(state->guestbook);
  if (!state->control) return members;
  for (guint i = members->len; i > 0; i--) {
    const char *member = g_ptr_array_index(members, i - 1);
    if (gn_concord_control_plane_is_banned(state->control, member))
      g_ptr_array_remove_index(members, i - 1);
  }
  return members;
}

typedef struct {
  GnConcordCommunityService *service;
  gchar *community_id;
} GuestbookBinding;

static void guestbook_binding_free(gpointer data) {
  GuestbookBinding *binding = data;
  if (!binding) return;
  g_free(binding->community_id);
  g_free(binding);
}

static void on_guestbook_event(const char *event_json, gpointer user_data) {
  GuestbookBinding *binding = user_data;
  if (!binding || !binding->service ||
      !GN_IS_CONCORD_COMMUNITY_SERVICE(binding->service))
    return;
  gn_concord_community_service_ingest_guestbook_wrap(
    binding->service, binding->community_id, event_json);
}

static void refresh_guestbook(GnConcordCommunityService *self,
                              CommunityState *state) {
  if (!self->context || self->shutting_down) return;
  if (!ensure_guestbook(state)) return;

  if (!state->guestbook_subscription) {
    GuestbookBinding *binding = g_new0(GuestbookBinding, 1);
    binding->service = self;
    binding->community_id = g_strdup(state->community_id);
    g_autofree gchar *filter = build_stream_filter(state->guestbook_address, 0);
    guint64 id = gnostr_plugin_context_subscribe_events(
      self->context, filter, G_CALLBACK(on_guestbook_event), binding,
      guestbook_binding_free);
    if (id) state->guestbook_subscription = id;
    else guestbook_binding_free(binding);
  }

  g_autofree gchar *filter = build_stream_filter(state->guestbook_address, 0);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events =
    gnostr_plugin_context_query_events(self->context, filter, &error);
  if (!events) {
    /* Off-consensus: a Guestbook that fails to load costs nothing else. */
    if (error) emit_error(self, error->message);
    return;
  }
  for (guint i = 0; i < events->len; i++)
    gn_concord_community_service_ingest_guestbook_wrap(
      self, state->community_id, g_ptr_array_index(events, i));
}

/* CORD-02 §5's three rumors, minus the snapshot: a Join or a Leave is the
 * member's own self-signed word, and an accepting joiner echoes the link's
 * creator and label so per-link usage counters are possible — all inside the
 * token-encrypted bundle, visible to link-holders alone (CORD-05 §1). */
static void publish_membership_verb(GnConcordCommunityService *self,
                                    CommunityState *state, const char *verb,
                                    gboolean attribute,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data) {
  const char *author = gn_concord_community_service_get_current_pubkey(self);
  if (!author || self->shutting_down || !self->context) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_CLOSED,
                         "Sign in to announce membership");
    return;
  }

  PublishContext *publish = g_new0(PublishContext, 1);
  publish->author = g_strdup(author);
  if (!derive_guestbook_key(state, &publish->key)) {
    publish_context_free(publish);
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_FAILED,
                         "This Community's Guestbook key could not be derived");
    return;
  }

  gint64 created_at = 0;
  int ms = split_now(&created_at);
  g_autofree gchar *ms_text = g_strdup_printf("%d", ms);

  g_autoptr(NostrEvent) rumor = nostr_event_new();
  nostr_event_set_kind(rumor, CONCORD_KIND_JOIN_LEAVE);
  nostr_event_set_pubkey(rumor, author);
  nostr_event_set_created_at(rumor, created_at);
  nostr_event_set_content(rumor, verb);
  NostrTags *tags = nostr_tags_new(1, nostr_tag_new("ms", ms_text, NULL));
  if (attribute && state->invite_creator)
    tags = nostr_tags_append_unique(
      tags, nostr_tag_new("invite", state->invite_creator,
                          state->invite_label ? state->invite_label : "",
                          NULL));
  nostr_event_set_tags(rumor, tags);

  publish_sealed_rumor(self, publish, rumor, created_at, cancellable, callback,
                       user_data);
}

/* ------------------------------------------------------------------ *
 * leaving (CORD-02 §5, §8)
 * ------------------------------------------------------------------ */

static void on_leave_published(GObject *source, GAsyncResult *result,
                               gpointer user_data) {
  GnConcordCommunityService *self = GN_CONCORD_COMMUNITY_SERVICE(source);
  GTask *task = G_TASK(user_data);
  const char *community_id = g_task_get_task_data(task);

  g_autoptr(GError) error = NULL;
  if (!gn_concord_community_service_publish_message_finish(self, result,
                                                           &error)) {
    /* The Leave is the member's own word: if it never reached a relay, the
     * membership stays, or this device would go quiet while every other one
     * still believes it is here. */
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }

  CommunityState *state = find_state(self, community_id);
  if (state) {
    /* A tombstone is per-Community and timestamped, and the entry stays *in*
     * the document — pruning it would let a long-offline device resurrect a
     * Community you left, and merges would depend on gossip order
     * (CORD-02 §8). */
    if (!self->list_document) self->list_document = json_object_new();
    JsonArray *stones = object_array(self->list_document, "tombstones");
    if (!stones) {
      stones = json_array_new();
      json_object_set_array_member(self->list_document, "tombstones", stones);
    }
    JsonObject *stone = json_object_new();
    json_object_set_string_member(stone, "community_id", community_id);
    json_object_set_int_member(stone, "removed_at", g_get_real_time() / 1000);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, stone);
    json_array_add_element(stones, node);

    g_hash_table_replace(self->list_orphans, g_strdup(community_id),
                         build_list_entry(state));

    if (self->context) {
      if (state->control_subscription)
        gnostr_plugin_context_unsubscribe_events(self->context,
                                                 state->control_subscription);
      if (state->guestbook_subscription)
        gnostr_plugin_context_unsubscribe_events(
          self->context, state->guestbook_subscription);
      GHashTableIter subs;
      gpointer sub_value;
      g_hash_table_iter_init(&subs, state->subscriptions);
      while (g_hash_table_iter_next(&subs, NULL, &sub_value))
        gnostr_plugin_context_unsubscribe_events(self->context,
                                                 *(guint64 *)sub_value);
    }

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
    g_hash_table_remove(self->states, community_id);
  }

  publish_community_list(self);
  emit_update(self, community_id, GN_CONCORD_UPDATE_MEMBERSHIP);
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

void gn_concord_community_service_leave_async(GnConcordCommunityService *self,
                                              const char *community_id,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self));
  CommunityState *state = find_state(self, community_id);
  if (!state || self->shutting_down || !self->context) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_NOT_FOUND,
                         "That Community is not one of yours");
    return;
  }

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_task_data(task, g_strdup(community_id), g_free);
  publish_membership_verb(self, state, "leave", FALSE, cancellable,
                          on_leave_published, task);
}

gboolean gn_concord_community_service_leave_finish(
    GnConcordCommunityService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ------------------------------------------------------------------ *
 * Direct Invites (CORD-05 §6)
 *
 * Nostr already has an encrypted, authenticated lane to a specific npub, so a
 * Direct Invite drops the link machinery and hands over the §1 bundle itself
 * — a *standard* NIP-59 giftwrap, not CORD-01's reversed stream wrap: an
 * ephemeral wrap author, the recipient in the `p` tag, and a kind-13 seal.
 * ------------------------------------------------------------------ */

typedef struct {
  gchar *wrap_author; /* ephemeral, single-use: proves nothing */
  gchar *inviter;     /* the seal's verified npub: who invited them */
} DirectInvite;

static void direct_invite_free(gpointer data) {
  DirectInvite *invite = data;
  if (!invite) return;
  g_free(invite->wrap_author);
  g_free(invite->inviter);
  g_free(invite);
}

static void on_direct_seal_opened(GObject *source, GAsyncResult *result,
                                  gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  GnConcordCommunityService *self = g_task_get_source_object(task);
  DirectInvite *invite = g_task_get_task_data(task);

  g_autoptr(GError) error = NULL;
  g_autofree gchar *rumor_json =
    self->context ? gnostr_plugin_context_nip44_decrypt_finish(self->context,
                                                               result, &error)
                  : NULL;
  if (!rumor_json) {
    g_task_return_error(task, error ? g_steal_pointer(&error)
                                    : g_error_new(G_IO_ERROR,
                                                  G_IO_ERROR_INVALID_DATA,
                                                  "The invite's seal did not "
                                                  "open"));
    g_object_unref(task);
    return;
  }

  g_autoptr(NostrEvent) rumor = nostr_event_new();
  /* NIP-59's impersonation check: renderers display rumor fields, so a rumor
   * claiming a different author than the seal that carried it is a forgery. */
  if (!nostr_event_deserialize_compact(rumor, rumor_json, NULL) ||
      nostr_event_get_kind(rumor) != CONCORD_DIRECT_INVITE ||
      g_strcmp0(nostr_event_get_pubkey(rumor), invite->inviter) != 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "That giftwrap is not a Concord invite");
    g_object_unref(task);
    return;
  }

  g_task_return_pointer(task, g_strdup(nostr_event_get_content(rumor)),
                        g_free);
  g_object_unref(task);
}

static void on_direct_wrap_opened(GObject *source, GAsyncResult *result,
                                  gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  GnConcordCommunityService *self = g_task_get_source_object(task);
  DirectInvite *invite = g_task_get_task_data(task);

  g_autoptr(GError) error = NULL;
  g_autofree gchar *seal_json =
    self->context ? gnostr_plugin_context_nip44_decrypt_finish(self->context,
                                                               result, &error)
                  : NULL;
  if (!seal_json) {
    g_task_return_error(task, error ? g_steal_pointer(&error)
                                    : g_error_new(G_IO_ERROR,
                                                  G_IO_ERROR_INVALID_DATA,
                                                  "The giftwrap did not open"));
    g_object_unref(task);
    return;
  }

  /* The seal's verified npub proves who invited them — the one thing the
   * ephemeral wrap author cannot. */
  g_autoptr(NostrEvent) seal = parse_verified_event(seal_json);
  if (!seal || nostr_event_get_kind(seal) != 13) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "That giftwrap carries no NIP-59 seal");
    g_object_unref(task);
    return;
  }
  invite->inviter = g_strdup(nostr_event_get_pubkey(seal));

  gnostr_plugin_context_nip44_decrypt_async(
    self->context, invite->inviter, nostr_event_get_content(seal),
    g_task_get_cancellable(task), on_direct_seal_opened, task);
}

void gn_concord_community_service_open_direct_invite_async(
    GnConcordCommunityService *self, const char *wrap_json,
    GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self));
  GTask *task = g_task_new(self, cancellable, callback, user_data);

  const char *me = gn_concord_community_service_get_current_pubkey(self);
  g_autoptr(NostrEvent) wrap = parse_verified_event(wrap_json);
  if (!me || self->shutting_down || !self->context || !wrap ||
      nostr_event_get_kind(wrap) != CONCORD_STREAM_WRAP) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "That is not a giftwrap this npub can open");
    g_object_unref(task);
    return;
  }
  /* Addressed to a person, never a plane: the recipient rides the `p` tag. */
  if (g_strcmp0(event_tag_value(wrap, "p"), me) != 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "That giftwrap is addressed to someone else");
    g_object_unref(task);
    return;
  }

  DirectInvite *invite = g_new0(DirectInvite, 1);
  invite->wrap_author = g_strdup(nostr_event_get_pubkey(wrap));
  g_task_set_task_data(task, invite, direct_invite_free);

  gnostr_plugin_context_nip44_decrypt_async(
    self->context, invite->wrap_author, nostr_event_get_content(wrap),
    cancellable, on_direct_wrap_opened, task);
}

gchar *gn_concord_community_service_open_direct_invite_finish(
    GnConcordCommunityService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void on_offered_invite_opened(GObject *source, GAsyncResult *result,
                                     gpointer user_data) {
  (void)user_data;
  GnConcordCommunityService *self = GN_CONCORD_COMMUNITY_SERVICE(source);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *bundle =
    gn_concord_community_service_open_direct_invite_finish(self, result,
                                                           &error);
  /* A giftwrap that isn't an invite is not an error worth surfacing: an
   * inbox carries other people's traffic. */
  if (!bundle) return;
  /* Nothing joins, subscribes, or announces presence until the user
   * explicitly accepts (CORD-05 §1). */
  g_signal_emit(self, signals[INVITE_OFFERED], 0, bundle, "");
}

void gn_concord_community_service_refresh_direct_invites(
    GnConcordCommunityService *self) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self));
  if (!self->context || self->shutting_down) return;
  const char *me = gn_concord_community_service_get_current_pubkey(self);
  if (!me) return;

  /* The wrap's outer `k` tag is what makes invites *indexed*: a recipient
   * looks up exactly their invites instead of decrypting everything ever
   * p-tagged at them. It is an unsigned hint and never authority — a client
   * decrypting its general giftwrap inbox honors an untagged invite all the
   * same (CORD-05 §6). */
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "kinds");
  json_builder_begin_array(builder);
  json_builder_add_int_value(builder, CONCORD_STREAM_WRAP);
  json_builder_end_array(builder);
  json_builder_set_member_name(builder, "#p");
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, me);
  json_builder_end_array(builder);
  json_builder_set_member_name(builder, "#k");
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "3313");
  json_builder_end_array(builder);
  json_builder_end_object(builder);
  JsonNode *root = json_builder_get_root(builder);
  g_autofree gchar *filter = node_to_json(root);
  json_node_free(root);

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events =
    gnostr_plugin_context_query_events(self->context, filter, &error);
  if (!events) {
    if (error) emit_error(self, error->message);
    return;
  }
  for (guint i = 0; i < events->len; i++)
    gn_concord_community_service_open_direct_invite_async(
      self, g_ptr_array_index(events, i), NULL, on_offered_invite_opened, NULL);
}

typedef struct {
  GnConcordCommunityService *service; /* borrowed; the service owns the sub */
  gchar *community_id;
} ControlBinding;

static void control_binding_free(gpointer data) {
  ControlBinding *binding = data;
  if (!binding) return;
  g_free(binding->community_id);
  g_free(binding);
}

static void on_control_event(const char *event_json, gpointer user_data) {
  ControlBinding *binding = user_data;
  if (!binding || !binding->service ||
      !GN_IS_CONCORD_COMMUNITY_SERVICE(binding->service))
    return;
  gn_concord_community_service_ingest_control_wrap(
    binding->service, binding->community_id, event_json);
}

static void refresh_control_plane(GnConcordCommunityService *self,
                                  CommunityState *state) {
  if (!self->context || self->shutting_down) return;
  if (!ensure_control_plane(state)) return;

  if (!state->control_subscription) {
    ControlBinding *binding = g_new0(ControlBinding, 1);
    binding->service = self;
    binding->community_id = g_strdup(state->community_id);
    g_autofree gchar *filter = build_stream_filter(state->control_address, 0);
    guint64 id = gnostr_plugin_context_subscribe_events(
      self->context, filter, G_CALLBACK(on_control_event), binding,
      control_binding_free);
    if (id) state->control_subscription = id;
    else control_binding_free(binding);
  }

  /* Every member keeps the *entire* Control Plane in sync — it is small and
   * must stay complete, which is exactly why members cannot write to it
   * (CORD-02 §5). So this backfill carries no page limit. */
  g_autofree gchar *filter = build_stream_filter(state->control_address, 0);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events =
    gnostr_plugin_context_query_events(self->context, filter, &error);
  if (!events) {
    if (error) emit_error(self, error->message);
    return;
  }
  for (guint i = 0; i < events->len; i++)
    gn_concord_community_service_ingest_control_wrap(
      self, state->community_id, g_ptr_array_index(events, i));
}

/* ------------------------------------------------------------------ *
 * the Community List (CORD-02 §8)
 *
 * A member's memberships sync across their devices — and their *clients* — as
 * one kind-13302 replaceable, NIP-44-encrypted to self. The host's self-ECDH
 * pair (nostrc-2ilq) is what makes that readable from a plugin at all: the
 * key never leaves the signer, so both directions route through it.
 * ------------------------------------------------------------------ */

/* The entry's join material: the bundle's *membership* subset. Never the icon
 * (a device folds it from the Control Plane) and never the link fields —
 * expiry and attribution belong to the invite, not to the membership. A
 * staffer's own `control_root` rides here and nowhere else — it syncs across
 * that staffer's own devices, and an invite bundle built from this material
 * asks for it to be left out (CORD-02 §2, §8). */
static JsonNode *build_join_material(CommunityState *state,
                                     gboolean include_control_root) {
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
  if (include_control_root && state->control_root) {
    json_builder_set_member_name(builder, "control_root");
    json_builder_add_string_value(builder, state->control_root);
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

  return json_builder_get_root(builder);
}

static guint64 snapshot_epoch(JsonNode *snapshot) {
  if (!snapshot || !JSON_NODE_HOLDS_OBJECT(snapshot)) return 0;
  gint64 epoch = object_int(json_node_get_object(snapshot), "root_epoch", 0);
  return epoch > 0 ? (guint64)epoch : 0;
}

/* The two snapshots solve opposite problems: `seed` holds the *earliest*
 * epoch ever held (the anchor for a full-history backfill) and `current` the
 * latest (so a fresh device reconstructs instantly). The merges mirror each
 * other — seed keeps the lower epoch, current the higher — and an epoch tie
 * breaks on the lexicographically lowest canonical bytes, a total order, so
 * two devices never flap competing republishes (CORD-02 §8). */
static void merge_seed(CommunityState *state, JsonNode *candidate) {
  if (!candidate) return;
  if (!state->list_seed) {
    state->list_seed = json_node_copy(candidate);
    return;
  }
  guint64 held = snapshot_epoch(state->list_seed);
  guint64 offered = snapshot_epoch(candidate);
  if (held < offered) return;
  if (offered == held) {
    g_autofree gchar *left = node_to_json(state->list_seed);
    g_autofree gchar *right = node_to_json(candidate);
    if (g_strcmp0(left, right) <= 0) return;
  }
  json_node_free(state->list_seed);
  state->list_seed = json_node_copy(candidate);
}

/* One List entry: the two snapshots, when the membership began, and whatever
 * fields this client doesn't understand, carried through untouched. */
static JsonNode *build_list_entry(CommunityState *state) {
  JsonNode *current = build_join_material(state, TRUE);
  merge_seed(state, current);

  JsonObject *entry = json_object_new();
  if (state->list_extra) {
    GList *members = json_object_get_members(state->list_extra);
    for (GList *l = members; l; l = l->next)
      json_object_set_member(
        entry, l->data,
        json_node_copy(json_object_get_member(state->list_extra, l->data)));
    g_list_free(members);
  }
  json_object_set_string_member(entry, "community_id", state->community_id);
  json_object_set_member(entry, "seed", json_node_copy(state->list_seed));
  json_object_set_member(entry, "current", current);
  json_object_set_int_member(entry, "added_at", state->added_at);
  /* The Refounding debt syncs with the membership it belongs to: a staffer
   * who retired the last link on a laptop should not find the Community
   * looking settled on a phone (CORD-06 §3). */
  if (state->refounding_due_epoch)
    json_object_set_int_member(entry, "refounding_due_epoch",
                               (gint64)state->refounding_due_epoch);

  JsonNode *node = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(node, entry);
  return node;
}

static gint64 tombstone_time(JsonObject *document, const char *community_id) {
  JsonArray *stones = object_array(document, "tombstones");
  if (!stones) return 0;
  gint64 newest = 0;
  for (guint i = 0; i < json_array_get_length(stones); i++) {
    JsonNode *node = json_array_get_element(stones, i);
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) continue;
    JsonObject *stone = json_node_get_object(node);
    if (g_strcmp0(object_string(stone, "community_id"), community_id) != 0)
      continue;
    gint64 removed_at = object_int(stone, "removed_at", 0);
    if (removed_at > newest) newest = removed_at;
  }
  return newest;
}

/* Rebuilds the document from live state, carrying forward everything this
 * client is not the authority on: the tombstones, the top-level members it
 * doesn't understand, the per-entry members it doesn't understand, and the
 * entries it could not adopt. Two clients share this one document, so the
 * round-trip discipline is what keeps one from deleting the other's work
 * (CORD-02 §6, §8). */
static JsonNode *build_list_document(GnConcordCommunityService *self,
                                     GError **error) {
  JsonObject *root = json_object_new();
  if (self->list_document) {
    GList *members = json_object_get_members(self->list_document);
    for (GList *l = members; l; l = l->next) {
      const char *name = l->data;
      if (g_strcmp0(name, "entries") == 0) continue;
      json_object_set_member(
        root, name,
        json_node_copy(json_object_get_member(self->list_document, name)));
    }
    g_list_free(members);
  }

  JsonArray *entries = json_array_new();
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->states);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    CommunityState *state = value;
    if (!state->item) continue;

    json_array_add_element(entries, build_list_entry(state));
  }

  if (self->list_orphans) {
    g_hash_table_iter_init(&iter, self->list_orphans);
    while (g_hash_table_iter_next(&iter, NULL, &value))
      json_array_add_element(entries, json_node_copy(value));
  }

  json_object_set_array_member(root, "entries", entries);
  JsonNode *document = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(document, root);

  /* The 50-membership cap is a protocol constant, not client taste: the List
   * is one NIP-44 event and NIP-44 plaintext hard-caps at 65,535 bytes. The
   * count is not the whole budget either — join material carrying private
   * Channel keys can overflow the event well below 50 — so the serialized
   * size is verified before publishing, never assumed (CORD-02 §8). */
  if (json_array_get_length(entries) > CONCORD_MAX_COMMUNITIES_IN_LIST) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                "The Community List caps at %d memberships",
                CONCORD_MAX_COMMUNITIES_IN_LIST);
    json_node_free(document);
    return NULL;
  }
  return document;
}

/* The self-encrypted document chain, shared by the Community List (CORD-02
 * §8) and the Invite List (CORD-05 §4): both are one NIP-44 replaceable
 * encrypted to self, and both round-trip what this client does not own. */
typedef struct {
  GnConcordCommunityService *service; /* strong: the chain outlives a panel */
  gchar *author;
  int kind;
} ListPublish;

static void list_publish_free(ListPublish *publish) {
  if (!publish) return;
  g_clear_object(&publish->service);
  g_free(publish->author);
  g_free(publish);
}

static void on_list_published(GObject *source, GAsyncResult *result,
                              gpointer user_data) {
  (void)source;
  ListPublish *publish = user_data;
  GnConcordCommunityService *self = publish->service;
  g_autoptr(GError) error = NULL;
  if (self->context &&
      !gnostr_plugin_context_publish_event_finish(self->context, result,
                                                  &error) && error)
    emit_error(self, error->message);
  list_publish_free(publish);
}

static void on_list_signed(GObject *source, GAsyncResult *result,
                           gpointer user_data) {
  (void)source;
  ListPublish *publish = user_data;
  GnConcordCommunityService *self = publish->service;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json =
    self->context ? gnostr_plugin_context_request_sign_event_finish(
                      self->context, result, &error)
                  : NULL;
  if (!signed_json || self->shutting_down || !self->context) {
    if (error) emit_error(self, error->message);
    list_publish_free(publish);
    return;
  }

  /* The signer is the host's: confirm what came back is the document we asked
   * for, signed by the identity we built it for. */
  g_autoptr(NostrEvent) event = parse_verified_event(signed_json);
  if (!event || nostr_event_get_kind(event) != publish->kind ||
      g_strcmp0(nostr_event_get_pubkey(event), publish->author) != 0) {
    emit_error(self, "The signer returned a different document");
    list_publish_free(publish);
    return;
  }

  gnostr_plugin_context_publish_event_async(self->context, signed_json, NULL,
                                            on_list_published, publish);
}

static void on_list_encrypted(GObject *source, GAsyncResult *result,
                              gpointer user_data) {
  (void)source;
  ListPublish *publish = user_data;
  GnConcordCommunityService *self = publish->service;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *content =
    self->context ? gnostr_plugin_context_nip44_self_encrypt_finish(
                      self->context, result, &error)
                  : NULL;
  if (!content || self->shutting_down || !self->context) {
    if (error) emit_error(self, error->message);
    list_publish_free(publish);
    return;
  }

  g_autoptr(NostrEvent) event = nostr_event_new();
  nostr_event_set_kind(event, publish->kind);
  nostr_event_set_pubkey(event, publish->author);
  nostr_event_set_created_at(event, (gint64)time(NULL));
  nostr_event_set_content(event, content);
  nostr_event_set_tags(event, nostr_tags_new(0));

  g_autofree gchar *unsigned_json = nostr_event_serialize_compact(event);
  if (!unsigned_json) {
    emit_error(self, "Failed to serialize the document");
    list_publish_free(publish);
    return;
  }
  gnostr_plugin_context_request_sign_event(self->context, unsigned_json, NULL,
                                           on_list_signed, publish);
}

static void publish_community_list(GnConcordCommunityService *self) {
  if (!self->context || self->shutting_down || self->list_applying) return;

  /* Publishing before a definitive read would replace the wire document with
   * whatever this session happens to hold — a failed query and a genuinely
   * empty List are indistinguishable from here, and one of them costs the
   * user every membership on every other device. Fail closed. */
  const char *author = gn_concord_community_service_get_current_pubkey(self);
  if (!self->list_loaded || !author ||
      g_strcmp0(author, self->list_author) != 0)
    return;

  g_autoptr(GError) error = NULL;
  JsonNode *document = build_list_document(self, &error);
  if (!document) {
    if (error) emit_error(self, error->message);
    return;
  }
  g_autofree gchar *plaintext = node_to_json(document);
  json_node_free(document);
  if (!plaintext) return;

  if (strlen(plaintext) > CONCORD_MAX_NIP44_PLAINTEXT) {
    emit_error(self,
               "This Community List no longer fits one NIP-44 event; it was "
               "not published");
    return;
  }

  ListPublish *publish = g_new0(ListPublish, 1);
  publish->service = g_object_ref(self);
  publish->author = g_strdup(author);
  publish->kind = CONCORD_COMMUNITY_LIST;
  gnostr_plugin_context_nip44_self_encrypt_async(
    self->context, plaintext, NULL, on_list_encrypted, publish);
}

/* Adopts one decrypted document. Each entry's `current` snapshot is exactly a
 * CORD-05 §1 bundle's membership subset, so it runs the same validation an
 * invite does — a document synced from another client is no more trusted than
 * a link. */
static void apply_list_document(GnConcordCommunityService *self,
                                JsonNode *root) {
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return;
  JsonObject *document = json_node_get_object(root);

  g_clear_pointer(&self->list_document, json_object_unref);
  self->list_document = json_object_ref(document);
  if (self->list_orphans) g_hash_table_remove_all(self->list_orphans);

  JsonArray *entries = object_array(document, "entries");
  guint n = entries ? json_array_get_length(entries) : 0;
  if (n > CONCORD_MAX_COMMUNITIES_IN_LIST)
    n = CONCORD_MAX_COMMUNITIES_IN_LIST;

  self->list_applying = TRUE;
  for (guint i = 0; i < n; i++) {
    JsonNode *node = json_array_get_element(entries, i);
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) continue;
    JsonObject *entry = json_node_get_object(node);
    const char *community_id = object_string(entry, "community_id");
    if (!nostr_concord_is_lower_hex_32(community_id)) continue;

    /* A tombstone is permanent and the entry stays *in* the document, so a
     * backfill can never re-add a Community you left; only a later re-join
     * (a newer `added_at`) resurrects it (CORD-02 §8). */
    gint64 added_at = object_int(entry, "added_at", 0);
    gboolean adopt = added_at > tombstone_time(document, community_id);

    JsonObject *current = object_object(entry, "current");
    g_autofree gchar *material =
      current ? node_to_json(json_object_get_member(entry, "current")) : NULL;
    g_autoptr(GError) error = NULL;
    if (adopt && material &&
        gn_concord_community_service_accept_bundle(self, material, &error)) {
      CommunityState *state = find_state(self, community_id);
      if (state) {
        state->added_at = added_at;
        gint64 due = object_int(entry, "refounding_due_epoch", 0);
        if (due > 0) state->refounding_due_epoch = (guint64)due;
        JsonNode *seed = json_object_has_member(entry, "seed")
                           ? json_object_get_member(entry, "seed") : NULL;
        if (seed && JSON_NODE_HOLDS_OBJECT(seed))
          merge_seed(state, seed);
        /* Round-trip whatever this client doesn't understand. */
        g_clear_pointer(&state->list_extra, json_object_unref);
        state->list_extra = json_object_new();
        GList *members = json_object_get_members(entry);
        for (GList *l = members; l; l = l->next) {
          const char *name = l->data;
          if (g_strcmp0(name, "community_id") == 0 ||
              g_strcmp0(name, "seed") == 0 ||
              g_strcmp0(name, "current") == 0 ||
              g_strcmp0(name, "added_at") == 0 ||
              g_strcmp0(name, "refounding_due_epoch") == 0)
            continue;
          json_object_set_member(
            state->list_extra, name,
            json_node_copy(json_object_get_member(entry, name)));
        }
        g_list_free(members);
      }
      continue;
    }

    /* An entry this client cannot use is still another device's membership:
     * keep it verbatim so a republish does not silently drop it. */
    if (error)
      g_warning("Concord: keeping an unusable Community List entry: %s",
                error->message);
    g_hash_table_replace(self->list_orphans, g_strdup(community_id),
                         json_node_copy(node));
  }
  self->list_applying = FALSE;
}

static void on_list_decrypted(GObject *source, GAsyncResult *result,
                              gpointer user_data) {
  (void)source;
  ListPublish *load = user_data;
  GnConcordCommunityService *self = load->service;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *document =
    self->context ? gnostr_plugin_context_nip44_self_decrypt_finish(
                      self->context, result, &error)
                  : NULL;
  if (self->shutting_down || !self->context) {
    list_publish_free(load);
    return;
  }
  if (!document) {
    /* Unreadable is not empty: leave the List unloaded so nothing republishes
     * over a document this client could not open. */
    emit_error(self, error ? error->message
                           : "The Community List could not be decrypted");
    list_publish_free(load);
    return;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  if (json_parser_load_from_data(parser, document, -1, NULL)) {
    apply_list_document(self, json_parser_get_root(parser));
    self->list_loaded = TRUE;
    g_free(self->list_author);
    self->list_author = g_strdup(load->author);
  } else {
    emit_error(self, "The Community List is not valid JSON");
  }
  memset(document, 0, strlen(document));

  gn_concord_community_service_refresh(self);
  list_publish_free(load);
}

static gchar *build_own_document_filter(const char *author, int kind) {
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "kinds");
  json_builder_begin_array(builder);
  json_builder_add_int_value(builder, kind);
  json_builder_end_array(builder);
  json_builder_set_member_name(builder, "authors");
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, author);
  json_builder_end_array(builder);
  json_builder_end_object(builder);
  JsonNode *root = json_builder_get_root(builder);
  gchar *json = node_to_json(root);
  json_node_free(root);
  return json;
}

static void load_community_list(GnConcordCommunityService *self) {
  if (!self->context || self->shutting_down) return;
  const char *author = gn_concord_community_service_get_current_pubkey(self);
  if (!author) return;

  g_autofree gchar *filter =
    build_own_document_filter(author, CONCORD_COMMUNITY_LIST);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events =
    gnostr_plugin_context_query_events(self->context, filter, &error);
  if (!events) {
    /* A failed read is not an empty List; publishing stays disarmed. */
    if (error) emit_error(self, error->message);
    return;
  }

  /* One replaceable per user: the newest wins, and a NIP-44 payload binds no
   * author, so the enclosing event's kind and author are checked here rather
   * than trusted from the plaintext. */
  const char *newest = NULL;
  gint64 newest_at = -1;
  for (guint i = 0; i < events->len; i++) {
    const char *json = g_ptr_array_index(events, i);
    g_autoptr(NostrEvent) event = parse_verified_event(json);
    if (!event || nostr_event_get_kind(event) != CONCORD_COMMUNITY_LIST ||
        g_strcmp0(nostr_event_get_pubkey(event), author) != 0 ||
        !nostr_event_get_content(event))
      continue;
    gint64 created_at = nostr_event_get_created_at(event);
    if (created_at > newest_at) {
      newest_at = created_at;
      newest = json;
    }
  }

  if (!newest) {
    /* No document is a definitive answer: this npub has no List yet, and the
     * first accepted invite mints one. */
    self->list_loaded = TRUE;
    g_free(self->list_author);
    self->list_author = g_strdup(author);
    return;
  }

  g_autoptr(NostrEvent) event = parse_verified_event(newest);
  ListPublish *load = g_new0(ListPublish, 1);
  load->service = g_object_ref(self);
  load->author = g_strdup(author);
  gnostr_plugin_context_nip44_self_decrypt_async(
    self->context, nostr_event_get_content(event), NULL, on_list_decrypted,
    load);
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

static void publish_sealed_rumor(GnConcordCommunityService *self,
                                 PublishContext *publish, NostrEvent *rumor,
                                 gint64 created_at, GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data) {
  g_autofree gchar *rumor_json = nostr_event_serialize_compact(rumor);
  char *seal_content = NULL;
  if (!rumor_json ||
      nostr_concord_stream_seal(publish->key.conv_key, rumor_json,
                                &seal_content) != NOSTR_CONCORD_OK) {
    publish_context_free(publish);
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_FAILED, "Failed to encrypt the rumor");
    return;
  }

  /* The seal is signed by the author's real key — that signature is what
   * proves who wrote the rumor, and it is the only place the identity
   * appears (CORD-01). */
  g_autoptr(NostrEvent) seal = nostr_event_new();
  nostr_event_set_kind(seal, CONCORD_SEAL_ENCRYPTED);
  nostr_event_set_pubkey(seal, publish->author);
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
  gint64 created_at = 0;
  int ms = split_now(&created_at);
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

  publish_sealed_rumor(self, publish, rumor, created_at, cancellable, callback,
                       user_data);
}

gboolean gn_concord_community_service_publish_message_finish(
    GnConcordCommunityService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ------------------------------------------------------------------ *
 * writing the Control Plane (CORD-04 §1)
 *
 * A member reads this plane; staff write it. The wrap is signed by the
 * control_root-derived signer, which is the address itself, and encrypted
 * under the community_root-derived read key every member holds. The seal is
 * plaintext (kind 20014) — the one plane where it must be — so a compaction
 * can re-wrap the signed edition into a new epoch with its signature intact.
 * ------------------------------------------------------------------ */

typedef struct {
  gchar *author;
  nostr_concord_group_key_t write_key; /* signs the wrap: the plane's address */
  nostr_concord_group_key_t read_key;  /* encrypts it: every member holds this */
} ControlPublish;

static void control_publish_free(gpointer data) {
  ControlPublish *publish = data;
  if (!publish) return;
  g_free(publish->author);
  nostr_concord_group_key_clear(&publish->write_key);
  nostr_concord_group_key_clear(&publish->read_key);
  g_free(publish);
}

/* The signer sk is staff-only and a plain member never holds one — which is
 * the whole point of the split (CORD-02 §2). A legacy, pre-split epoch had no
 * signer key at all: the read derivation was the plane, address and signer
 * both, so writing it takes only membership (§5). */
static gboolean derive_control_write_key(CommunityState *state,
                                         nostr_concord_group_key_t *out,
                                         GError **error) {
  if (!state->control_pk) {
    if (derive_control_read_key(state, out)) return TRUE;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "This Community's Control Plane key could not be derived");
    return FALSE;
  }

  if (!state->control_root) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "Only staff holding this Community's control key can publish "
                "to its Control Plane");
    return FALSE;
  }

  uint8_t root[32], id[32];
  if (!nostr_concord_hex_decode_32(state->control_root, root) ||
      !nostr_concord_hex_decode_32(state->community_id, id) ||
      nostr_concord_control_signer_key(root, id, state->root_epoch, out) !=
        NOSTR_CONCORD_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "This Community's Control Plane signer could not be derived");
    return FALSE;
  }

  /* A wrap the plane's own address does not sign is one every member drops.
   * Catching that here beats minting events nobody can fold. */
  char pk[65];
  nostr_concord_hex_encode_32(out->pk, pk);
  if (g_strcmp0(pk, state->control_pk) != 0) {
    nostr_concord_group_key_clear(out);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "This device's control key does not address this Community's "
                "Control Plane");
    return FALSE;
  }
  return TRUE;
}

static void on_control_wrap_published(GObject *source, GAsyncResult *result,
                                      gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  GnConcordCommunityService *self = g_task_get_source_object(task);
  g_autoptr(GError) error = NULL;
  if (self->shutting_down || !self->context)
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "The Concord service was deactivated");
  else if (!gnostr_plugin_context_publish_event_finish(self->context, result,
                                                       &error))
    g_task_return_error(task, g_steal_pointer(&error));
  else
    g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

static void on_control_seal_signed(GObject *source, GAsyncResult *result,
                                   gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  GnConcordCommunityService *self = g_task_get_source_object(task);
  ControlPublish *publish = g_task_get_task_data(task);

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
    else
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Failed to sign the Control Plane edition");
    g_object_unref(task);
    return;
  }

  /* The signer is the host's: confirm what came back is the seal we asked
   * for, signed by the identity we built it for. */
  g_autoptr(NostrEvent) seal = parse_verified_event(seal_json);
  if (!seal || nostr_event_get_kind(seal) != CONCORD_SEAL_PLAINTEXT ||
      g_strcmp0(nostr_event_get_pubkey(seal), publish->author) != 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "The signer returned a different event");
    g_object_unref(task);
    return;
  }

  char *wrap_content = NULL;
  if (nostr_concord_stream_seal(publish->read_key.conv_key, seal_json,
                                &wrap_content) != NOSTR_CONCORD_OK) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to encrypt the Control Plane wrap");
    g_object_unref(task);
    return;
  }

  g_autofree gchar *ephemeral_sk = nostr_key_generate_private();
  g_autofree gchar *ephemeral_pk =
    ephemeral_sk ? nostr_key_get_public(ephemeral_sk) : NULL;
  if (ephemeral_sk) memset(ephemeral_sk, 0, strlen(ephemeral_sk));
  char stream_sk[65], stream_pk[65];
  nostr_concord_hex_encode_32(publish->write_key.sk, stream_sk);
  nostr_concord_hex_encode_32(publish->write_key.pk, stream_pk);

  g_autoptr(NostrEvent) wrap = nostr_event_new();
  nostr_event_set_kind(wrap, CONCORD_STREAM_WRAP);
  nostr_event_set_pubkey(wrap, stream_pk);
  nostr_event_set_created_at(wrap, (gint64)time(NULL));
  nostr_event_set_content(wrap, wrap_content);
  nostr_event_set_tags(wrap, nostr_tags_new(1, nostr_tag_new("p", ephemeral_pk,
                                                             NULL)));
  free(wrap_content);
  int signed_ok = ephemeral_pk ? nostr_event_sign(wrap, stream_sk) : -1;
  memset(stream_sk, 0, sizeof(stream_sk));
  g_autofree gchar *wrap_json =
    signed_ok == 0 ? nostr_event_serialize_compact(wrap) : NULL;
  if (!wrap_json) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to sign the Control Plane wrap");
    g_object_unref(task);
    return;
  }

  gnostr_plugin_context_publish_event_async(self->context, wrap_json,
                                            g_task_get_cancellable(task),
                                            on_control_wrap_published, task);
}

/* The authority citation every ranked action carries: the exact Grant this
 * actor's rank rests on, pinned by coordinate, version and content hash, so a
 * reader that has not synced it parks the action instead of dropping it, and
 * a just-demoted admin's action is never honored by a lagging client (CORD-04
 * §5). The owner cites nothing — their rank comes from the community_id
 * itself, which is what makes the Roster fold non-circular. */
static void append_grant_citation(CommunityState *state, const char *author,
                                  NostrTags *tags) {
  if (!state->control) return;
  const char *grant_hash = NULL;
  guint64 grant_version =
    gn_concord_control_plane_get_grant_head(state->control, author,
                                            &grant_hash);
  if (!grant_version || !grant_hash) return;

  uint8_t community[32], member[32], locator[32];
  char locator_hex[65];
  if (!nostr_concord_hex_decode_32(state->community_id, community) ||
      !nostr_concord_hex_decode_32(author, member) ||
      nostr_concord_grant_locator(community, member, locator) !=
        NOSTR_CONCORD_OK)
    return;
  nostr_concord_hex_encode_32(locator, locator_hex);
  g_autofree gchar *grant_version_text =
    g_strdup_printf("%" G_GUINT64_FORMAT, grant_version);
  nostr_tags_append_unique(
    tags, nostr_tag_new("vac", locator_hex, grant_version_text, grant_hash,
                        NULL));
}

/* Publishes one CORD-04 edition. @prev_hash is the superseded edition's hash,
 * NULL on a first edition, and the version discipline is the caller's: it
 * only ever climbs, starting at 1, with `prev` absent on the first alone. */
static void publish_control_edition_async(GnConcordCommunityService *self,
                                          CommunityState *state, guint vsk,
                                          const char *eid, guint64 version,
                                          const char *prev_hash,
                                          const char *content,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
  const char *author = gn_concord_community_service_get_current_pubkey(self);
  if (!author || !self->context || self->shutting_down) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_CLOSED,
                         "Sign in to publish to this Community");
    return;
  }

  g_autoptr(GError) error = NULL;
  ControlPublish *publish = g_new0(ControlPublish, 1);
  publish->author = g_strdup(author);
  if (!derive_control_write_key(state, &publish->write_key, &error) ||
      !derive_control_read_key(state, &publish->read_key)) {
    control_publish_free(publish);
    return_publish_error(self, cancellable, callback, user_data,
                         error ? (GIOErrorEnum)error->code : G_IO_ERROR_FAILED,
                         error ? error->message
                               : "This Community's Control Plane read key "
                                 "could not be derived");
    return;
  }

  g_autofree gchar *vsk_text = g_strdup_printf("%u", vsk);
  g_autofree gchar *version_text =
    g_strdup_printf("%" G_GUINT64_FORMAT, version);

  g_autoptr(NostrEvent) rumor = nostr_event_new();
  nostr_event_set_kind(rumor, CONCORD_KIND_CONTROL_EDITION);
  nostr_event_set_pubkey(rumor, author);
  nostr_event_set_created_at(rumor, (gint64)time(NULL));
  nostr_event_set_content(rumor, content);

  NostrTags *tags = nostr_tags_new(0);
  tags = nostr_tags_append_unique(tags, nostr_tag_new("vsk", vsk_text, NULL));
  tags = nostr_tags_append_unique(tags, nostr_tag_new("eid", eid, NULL));
  tags = nostr_tags_append_unique(tags,
                                  nostr_tag_new("ev", version_text, NULL));
  if (prev_hash)
    tags = nostr_tags_append_unique(tags,
                                    nostr_tag_new("ep", prev_hash, NULL));
  append_grant_citation(state, author, tags);
  nostr_event_set_tags(rumor, tags);

  g_autofree gchar *rumor_json = nostr_event_serialize_compact(rumor);
  if (!rumor_json) {
    control_publish_free(publish);
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_FAILED,
                         "Failed to serialize the Control Plane edition");
    return;
  }

  /* Plaintext means exactly that: the seal's content is the rumor's
   * serialized JSON, carried byte-verbatim, never a NIP-44 payload. */
  g_autoptr(NostrEvent) seal = nostr_event_new();
  nostr_event_set_kind(seal, CONCORD_SEAL_PLAINTEXT);
  nostr_event_set_pubkey(seal, author);
  nostr_event_set_created_at(seal, (gint64)time(NULL));
  nostr_event_set_content(seal, rumor_json);
  nostr_event_set_tags(seal, nostr_tags_new(0));
  g_autofree gchar *seal_json = nostr_event_serialize_compact(seal);
  if (!seal_json) {
    control_publish_free(publish);
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_FAILED,
                         "Failed to serialize the Control Plane seal");
    return;
  }

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_task_data(task, publish, control_publish_free);
  gnostr_plugin_context_request_sign_event(self->context, seal_json,
                                           cancellable, on_control_seal_signed,
                                           task);
}

static gboolean publish_control_edition_finish(GnConcordCommunityService *self,
                                               GAsyncResult *result,
                                               GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ------------------------------------------------------------------ *
 * minting and retiring invite links (CORD-05 §2, §4, §5)
 *
 * Three things have to land for a link to exist, and the order is not
 * arbitrary. The **bundle** goes first: a Registry naming a coordinate with
 * no bundle behind it reads to every member as a live link that cannot be
 * followed, where a bundle no Registry names is merely an unlisted link that
 * works. The **Registry** edit goes second, because §5 requires one to
 * accompany every mint and the aggregate active-set it feeds is the
 * Community's Public/Private truth. The creator's own **Invite List** goes
 * last: it is private bookkeeping, and an entry recorded for a link that was
 * never published is a URL that opens nothing.
 *
 * Retiring runs the same order for the same reason, tombstone first. A
 * Registry still naming a tombstoned coordinate costs a follower a clear
 * "this link was revoked"; the reverse would leave live keys behind a URL
 * nobody lists.
 * ------------------------------------------------------------------ */

/* The base is interchangeable — only the naddr and the fragment are protocol
 * (CORD-05 §2) — so this is a default that opens in any Concord client, not
 * part of the wire format. */
#define CONCORD_INVITE_URL_BASE "https://vectorapp.io/invite/"

static JsonArray *document_array(JsonObject *document, const char *member) {
  JsonArray *array = object_array(document, member);
  if (array) return array;
  array = json_array_new();
  json_object_set_array_member(document, member, array);
  return array;
}

/* A tombstone always beats an entry, terminally, so a stale device can never
 * resurrect a revoked link (CORD-05 §4). */
static gboolean invite_is_revoked(JsonObject *document, const char *token) {
  JsonArray *stones = document ? object_array(document, "tombstones") : NULL;
  for (guint i = 0; stones && i < json_array_get_length(stones); i++) {
    JsonNode *node = json_array_get_element(stones, i);
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) continue;
    if (g_strcmp0(object_string(json_node_get_object(node), "token"),
                  token) == 0)
      return TRUE;
  }
  return FALSE;
}

/* (element-type JsonObject) (transfer container): every live entry, borrowed
 * from @document. @community_id filters; NULL takes them all. */
static GPtrArray *invite_live_entries(JsonObject *document,
                                      const char *community_id) {
  GPtrArray *live = g_ptr_array_new();
  JsonArray *entries = document ? object_array(document, "entries") : NULL;
  for (guint i = 0; entries && i < json_array_get_length(entries); i++) {
    JsonNode *node = json_array_get_element(entries, i);
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) continue;
    JsonObject *entry = json_node_get_object(node);
    const char *token = object_string(entry, "token");
    if (!token || !*token || invite_is_revoked(document, token)) continue;
    if (community_id &&
        g_strcmp0(object_string(entry, "community_id"), community_id) != 0)
      continue;
    g_ptr_array_add(live, entry);
  }
  return live;
}

/* The link signer's pubkey — the coordinate the Registry publishes — from the
 * secret the creator kept. Deriving it beats storing it: one field cannot
 * then disagree with the other. */
static gchar *invite_entry_signer(JsonObject *entry) {
  const char *signer_sk = object_string(entry, "signer_sk");
  if (!nostr_concord_is_lower_hex_32(signer_sk)) return NULL;
  return nostr_key_get_public(signer_sk);
}

static void publish_invite_list(GnConcordCommunityService *self) {
  if (!self->context || self->shutting_down || !self->invite_document) return;

  /* The same fail-closed rule the Community List lives by, and this document
   * needs it more: it holds every link's signing secret, so replacing an
   * unread one costs the creator the ability to refresh or retire the links
   * their other devices minted (CORD-05 §4). */
  const char *author = gn_concord_community_service_get_current_pubkey(self);
  if (!self->invite_loaded || !author ||
      g_strcmp0(author, self->invite_author) != 0)
    return;

  JsonNode *document = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(document, self->invite_document);
  g_autofree gchar *plaintext = node_to_json(document);
  json_node_free(document);
  if (!plaintext) return;

  if (strlen(plaintext) > CONCORD_MAX_NIP44_PLAINTEXT) {
    memset(plaintext, 0, strlen(plaintext));
    emit_error(self,
               "This Invite List no longer fits one NIP-44 event; it was not "
               "published");
    return;
  }

  ListPublish *publish = g_new0(ListPublish, 1);
  publish->service = g_object_ref(self);
  publish->author = g_strdup(author);
  publish->kind = CONCORD_INVITE_LIST;
  gnostr_plugin_context_nip44_self_encrypt_async(
    self->context, plaintext, NULL, on_list_encrypted, publish);
  memset(plaintext, 0, strlen(plaintext));
}

static void on_invite_list_decrypted(GObject *source, GAsyncResult *result,
                                     gpointer user_data) {
  (void)source;
  ListPublish *load = user_data;
  GnConcordCommunityService *self = load->service;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *document =
    self->context ? gnostr_plugin_context_nip44_self_decrypt_finish(
                      self->context, result, &error)
                  : NULL;
  if (self->shutting_down || !self->context) {
    list_publish_free(load);
    return;
  }
  if (!document) {
    /* Unreadable is not empty: leave the List unloaded so nothing republishes
     * over a document this client could not open. */
    emit_error(self, error ? error->message
                           : "The Invite List could not be decrypted");
    list_publish_free(load);
    return;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  JsonNode *root = json_parser_load_from_data(parser, document, -1, NULL)
                     ? json_parser_get_root(parser) : NULL;
  if (root && JSON_NODE_HOLDS_OBJECT(root)) {
    /* Kept verbatim: two clients can share this one document, so a republish
     * has to round-trip the entries and fields this one doesn't understand
     * rather than deleting another's work (CORD-02 §6, CORD-05 §4). */
    g_clear_pointer(&self->invite_document, json_object_unref);
    self->invite_document = json_object_ref(json_node_get_object(root));
    self->invite_loaded = TRUE;
    g_free(self->invite_author);
    self->invite_author = g_strdup(load->author);
  } else {
    emit_error(self, "The Invite List is not valid JSON");
  }
  memset(document, 0, strlen(document));
  list_publish_free(load);
}

static void load_invite_list(GnConcordCommunityService *self) {
  if (!self->context || self->shutting_down) return;
  const char *author = gn_concord_community_service_get_current_pubkey(self);
  if (!author) return;

  g_autofree gchar *filter =
    build_own_document_filter(author, CONCORD_INVITE_LIST);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events =
    gnostr_plugin_context_query_events(self->context, filter, &error);
  if (!events) {
    /* A failed read is not an empty List; publishing stays disarmed. */
    if (error) emit_error(self, error->message);
    return;
  }

  const char *newest = NULL;
  gint64 newest_at = -1;
  for (guint i = 0; i < events->len; i++) {
    const char *json = g_ptr_array_index(events, i);
    g_autoptr(NostrEvent) event = parse_verified_event(json);
    if (!event || nostr_event_get_kind(event) != CONCORD_INVITE_LIST ||
        g_strcmp0(nostr_event_get_pubkey(event), author) != 0 ||
        !nostr_event_get_content(event))
      continue;
    gint64 created_at = nostr_event_get_created_at(event);
    if (created_at > newest_at) {
      newest_at = created_at;
      newest = json;
    }
  }

  if (!newest) {
    /* No document is a definitive answer: this npub has minted no links yet,
     * and the first one mints the List. */
    if (!self->invite_document) self->invite_document = json_object_new();
    self->invite_loaded = TRUE;
    g_free(self->invite_author);
    self->invite_author = g_strdup(author);
    return;
  }

  g_autoptr(NostrEvent) event = parse_verified_event(newest);
  ListPublish *load = g_new0(ListPublish, 1);
  load->service = g_object_ref(self);
  load->author = g_strdup(author);
  load->kind = CONCORD_INVITE_LIST;
  gnostr_plugin_context_nip44_self_decrypt_async(
    self->context, nostr_event_get_content(event), NULL,
    on_invite_list_decrypted, load);
}

void gn_concord_invite_link_free(GnConcordInviteLink *link) {
  if (!link) return;
  g_free(link->token);
  g_free(link->community_id);
  g_free(link->url);
  g_free(link->label);
  g_free(link);
}

static gint compare_invite_links(gconstpointer a, gconstpointer b) {
  const GnConcordInviteLink *left = *(const GnConcordInviteLink **)a;
  const GnConcordInviteLink *right = *(const GnConcordInviteLink **)b;
  if (left->created_at == right->created_at)
    return g_strcmp0(left->token, right->token);
  return left->created_at > right->created_at ? -1 : 1;
}

GPtrArray *gn_concord_community_service_get_invites(
    GnConcordCommunityService *self, const char *community_id) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), NULL);
  GPtrArray *links =
    g_ptr_array_new_with_free_func((GDestroyNotify)gn_concord_invite_link_free);
  if (!self->invite_document) return links;

  g_autoptr(GPtrArray) entries =
    invite_live_entries(self->invite_document, community_id);
  for (guint i = 0; i < entries->len; i++) {
    JsonObject *entry = g_ptr_array_index(entries, i);
    const char *url = object_string(entry, "url");
    if (!url || !*url) continue;
    GnConcordInviteLink *link = g_new0(GnConcordInviteLink, 1);
    link->token = g_strdup(object_string(entry, "token"));
    link->community_id = g_strdup(object_string(entry, "community_id"));
    link->url = g_strdup(url);
    link->label = g_strdup(object_string(entry, "label"));
    link->created_at = object_int(entry, "created_at", 0);
    link->expires_at = object_int(entry, "expires_at", 0);
    g_ptr_array_add(links, link);
  }
  g_ptr_array_sort(links, compare_invite_links);
  return links;
}

/* This creator's live link coordinates with one edit applied: sorted, so two
 * of the creator's devices holding the same set mint the same bytes — and
 * therefore the same edition hash — rather than forking the version chain
 * over ordering alone.
 *
 * The wire fold and the creator's own List are unioned deliberately — a
 * session that has not yet synced its own Registry would otherwise publish a
 * version that silently retires every link it hasn't seen.
 *
 * (element-type utf8) (transfer full) */
static GPtrArray *registry_live_set(GnConcordCommunityService *self,
                                    CommunityState *state, const char *author,
                                    const char *add_signer,
                                    const char *drop_signer) {
  g_autoptr(GHashTable) seen =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GPtrArray) links = g_ptr_array_new_with_free_func(g_free);

  GPtrArray *folded =
    state->control ? gn_concord_control_plane_get_creator_invite_links(
                       state->control, author)
                   : NULL;
  for (guint i = 0; folded && i < folded->len; i++) {
    const char *signer = g_ptr_array_index(folded, i);
    if (g_strcmp0(signer, drop_signer) == 0) continue;
    if (g_hash_table_contains(seen, signer)) continue;
    g_hash_table_add(seen, g_strdup(signer));
    g_ptr_array_add(links, g_strdup(signer));
  }

  g_autoptr(GPtrArray) entries =
    invite_live_entries(self->invite_document, state->community_id);
  for (guint i = 0; i < entries->len; i++) {
    g_autofree gchar *signer = invite_entry_signer(g_ptr_array_index(entries, i));
    if (!signer || g_strcmp0(signer, drop_signer) == 0) continue;
    if (g_hash_table_contains(seen, signer)) continue;
    g_hash_table_add(seen, g_strdup(signer));
    g_ptr_array_add(links, g_strdup(signer));
  }

  if (add_signer && !g_hash_table_contains(seen, add_signer))
    g_ptr_array_add(links, g_strdup(add_signer));

  g_ptr_array_sort_values(links, (GCompareFunc)g_strcmp0);
  return g_steal_pointer(&links);
}

/* CORD-06 §3: did this retire take the Community's *last* live link — the
 * moment CORD-05 §5 turns it Private and CORD-06 owes it a Refounding?
 *
 * The aggregate is the truth, so this creator emptying their own Registry is
 * not enough: any other creator still listing a link keeps the Community
 * Public. Their sets come from the fold, which is also where this creator's
 * own pre-retire set sits — the edition just published has not echoed back
 * yet, so the fold's copy of it is skipped in favour of the set actually
 * written. */
static gboolean retire_emptied_active_set(GnConcordCommunityService *self,
                                          CommunityState *state,
                                          const char *author,
                                          const char *retired_signer) {
  g_autoptr(GPtrArray) mine =
    registry_live_set(self, state, author, NULL, retired_signer);
  if (mine->len > 0 || !state->control) return FALSE;

  GPtrArray *folded_mine =
    gn_concord_control_plane_get_creator_invite_links(state->control, author);
  GPtrArray *aggregate =
    gn_concord_control_plane_get_invite_links(state->control);
  for (guint i = 0; aggregate && i < aggregate->len; i++) {
    const char *signer = g_ptr_array_index(aggregate, i);
    gboolean ours = FALSE;
    for (guint j = 0; folded_mine && j < folded_mine->len && !ours; j++)
      ours = g_strcmp0(signer, g_ptr_array_index(folded_mine, j)) == 0;
    if (!ours) return FALSE;
  }
  return TRUE;
}

/* The Registry edition's content: the same set, as the JSON array §5 puts on
 * the wire. */
static gchar *build_registry_content(GnConcordCommunityService *self,
                                     CommunityState *state, const char *author,
                                     const char *add_signer,
                                     const char *drop_signer) {
  g_autoptr(GPtrArray) links =
    registry_live_set(self, state, author, add_signer, drop_signer);

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);
  for (guint i = 0; i < links->len; i++)
    json_builder_add_string_value(builder, g_ptr_array_index(links, i));
  json_builder_end_array(builder);
  JsonNode *root = json_builder_get_root(builder);
  gchar *content = node_to_json(root);
  json_node_free(root);
  return content;
}

static gchar *registry_coordinate_for(CommunityState *state,
                                      const char *author) {
  uint8_t community[32], creator[32], eid[32];
  if (!nostr_concord_hex_decode_32(state->community_id, community) ||
      !nostr_concord_hex_decode_32(author, creator) ||
      nostr_concord_invite_registry_locator(community, creator, eid) !=
        NOSTR_CONCORD_OK)
    return NULL;
  char hex[65];
  nostr_concord_hex_encode_32(eid, hex);
  return g_strdup(hex);
}

typedef struct {
  gchar *community_id;
  gchar *author;
  gchar *label;
  gint64 expires_at;
  gchar *token_hex;
  gchar *signer_sk; /* secret: the link signer, wiped on free */
  gchar *signer_pk;
  gchar *url;
  gboolean retiring;
} InviteMint;

static void invite_mint_free(gpointer data) {
  InviteMint *mint = data;
  if (!mint) return;
  g_free(mint->community_id);
  g_free(mint->author);
  g_free(mint->label);
  clear_secret(&mint->token_hex);
  clear_secret(&mint->signer_sk);
  g_free(mint->signer_pk);
  g_free(mint->url);
  g_free(mint);
}

/* `$BASE/invite/<naddr>#<fragment>`. The naddr is a locator and rides in the
 * open; the fragment carries the unlock token and is never sent to any
 * server, which is what keeps the base domain and the relays unable to open
 * a bundle they can both see (CORD-05 §2). */
static gchar *build_invite_url(CommunityState *state,
                               const uint8_t token[CONCORD_INVITE_TOKEN_BYTES],
                               const char *signer_pk, GError **error) {
  guint n_relays = 0;
  const char *const *relays =
    gn_concord_community_item_get_relays(state->item, &n_relays);
  if (n_relays > CONCORD_MAX_RELAYS_IN_FRAGMENT)
    n_relays = CONCORD_MAX_RELAYS_IN_FRAGMENT;

  /* With no relays of its own the fragment selects the stock set by one flag
   * and carries zero relay bytes — the common invite's shortest form (§3). */
  char *fragment = NULL;
  nostr_concord_status_t status = nostr_concord_invite_fragment_encode(
    token, relays, n_relays, n_relays == 0, &fragment);
  if (status != NOSTR_CONCORD_OK || !fragment) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "The invite fragment could not be encoded: %s",
                nostr_concord_status_string(status));
    return NULL;
  }

  /* The per-link pubkey alone makes the coordinate unique, so the naddr
   * carries an empty identifier and no relay entries — as short as an naddr
   * gets, which is what length-restricted platforms need. */
  NostrEntityPointer pointer = {
    .public_key = (char *)signer_pk,
    .kind = CONCORD_INVITE_BUNDLE,
    .identifier = (char *)"",
    .relays = NULL,
    .relays_count = 0
  };
  char *naddr = NULL;
  if (nostr_nip19_encode_naddr(&pointer, &naddr) != 0 || !naddr) {
    memset(fragment, 0, strlen(fragment));
    free(fragment);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "The invite link's naddr could not be encoded");
    return NULL;
  }

  gchar *url = g_strconcat(CONCORD_INVITE_URL_BASE, naddr, "#", fragment, NULL);
  memset(fragment, 0, strlen(fragment));
  free(fragment);
  free(naddr);
  return url;
}

/* Everything a mint or a retire needs before it publishes anything: the
 * membership, the CREATE_INVITE the Control Plane fold resolves, and an
 * Invite List this session has definitively read. */
static CommunityState *invite_precheck(GnConcordCommunityService *self,
                                       const char *community_id,
                                       const char **out_author,
                                       GError **error) {
  CommunityState *state = find_state(self, community_id);
  if (!state || !state->item) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "That Community is not one of yours");
    return NULL;
  }
  if (!self->context || self->shutting_down) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                "The Concord service is not connected");
    return NULL;
  }
  const char *author = gn_concord_community_service_get_current_pubkey(self);
  if (!author) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                "Sign in to manage this Community's invite links");
    return NULL;
  }

  /* CORD-05 §5: minting and retiring are gated by CREATE_INVITE, which is
   * also a staff bit — so a holder has the control_root the Registry edit
   * needs. The owner is supreme and proven by the community_id itself. */
  if (!ensure_control_plane(state)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "This Community's Control Plane could not be derived");
    return NULL;
  }
  guint64 permissions =
    gn_concord_control_plane_get_permissions(state->control, author);
  if (!(permissions & CONCORD_PERM_CREATE_INVITE)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "You do not hold CREATE_INVITE in this Community");
    return NULL;
  }

  if (!self->invite_loaded || !self->invite_document ||
      g_strcmp0(author, self->invite_author) != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PENDING,
                "Your Invite List has not been read yet; a link minted now "
                "could retire the ones your other devices hold");
    return NULL;
  }

  if (out_author) *out_author = author;
  return state;
}

static void on_invite_registry_published(GObject *source, GAsyncResult *result,
                                         gpointer user_data);

static void on_invite_bundle_published(GObject *source, GAsyncResult *result,
                                       gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  GnConcordCommunityService *self = g_task_get_source_object(task);
  InviteMint *mint = g_task_get_task_data(task);

  g_autoptr(GError) error = NULL;
  if (self->shutting_down || !self->context) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "The Concord service was deactivated");
    g_object_unref(task);
    return;
  }
  if (!gnostr_plugin_context_publish_event_finish(self->context, result,
                                                  &error)) {
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }

  CommunityState *state = find_state(self, mint->community_id);
  g_autofree gchar *eid =
    state ? registry_coordinate_for(state, mint->author) : NULL;
  if (!state || !eid) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "This creator's Registry coordinate could not be "
                            "derived");
    g_object_unref(task);
    return;
  }

  g_autofree gchar *content = build_registry_content(
    self, state, mint->author, mint->retiring ? NULL : mint->signer_pk,
    mint->retiring ? mint->signer_pk : NULL);
  const char *prev = NULL;
  guint64 version = gn_concord_control_plane_get_registry_head(
                      state->control, mint->author, &prev) + 1;
  g_autofree gchar *prev_hash = g_strdup(prev);

  publish_control_edition_async(
    self, state, CONCORD_VSK_INVITE_REGISTRY, eid, version,
    version > 1 ? prev_hash : NULL, content, g_task_get_cancellable(task),
    on_invite_registry_published, task);
}

static void on_invite_registry_published(GObject *source, GAsyncResult *result,
                                         gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  GnConcordCommunityService *self = g_task_get_source_object(task);
  InviteMint *mint = g_task_get_task_data(task);

  g_autoptr(GError) error = NULL;
  if (!publish_control_edition_finish(self, result, &error)) {
    /* The bundle is already on relays, so the link works — it is simply not
     * listed. Say so: the user's next move is to retry, not to re-mint. */
    g_task_return_new_error(
      task, G_IO_ERROR, G_IO_ERROR_FAILED,
      mint->retiring
        ? "The link was revoked but its Registry entry could not be removed: %s"
        : "The link was published but could not be listed in this Community's "
          "Registry: %s",
      error ? error->message : "unknown error");
    g_object_unref(task);
    return;
  }

  if (!self->invite_document) self->invite_document = json_object_new();
  if (mint->retiring) {
    /* CORD-06 §3: the Registry that just landed may have been the last live
     * link in the Community, which converts it to Private — and a
     * Public-to-Private conversion is exactly a Refounding's other trigger.
     * Nothing rotates yet, so record the debt against the epoch it was
     * incurred at rather than letting the Community look settled: every
     * holder of the link just retired still holds the `community_root` its
     * bundle handed out, and only rolling that root severs them. */
    CommunityState *retired_in = find_state(self, mint->community_id);
    if (retired_in &&
        retire_emptied_active_set(self, retired_in, mint->author,
                                  mint->signer_pk)) {
      retired_in->refounding_due_epoch = retired_in->root_epoch + 1;
      publish_community_list(self);
    }

    JsonArray *stones = document_array(self->invite_document, "tombstones");
    JsonObject *stone = json_object_new();
    json_object_set_string_member(stone, "token", mint->token_hex);
    json_object_set_string_member(stone, "community_id", mint->community_id);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, stone);
    json_array_add_element(stones, node);
  } else {
    JsonArray *entries = document_array(self->invite_document, "entries");
    JsonObject *entry = json_object_new();
    json_object_set_string_member(entry, "token", mint->token_hex);
    json_object_set_string_member(entry, "signer_sk", mint->signer_sk);
    json_object_set_string_member(entry, "community_id", mint->community_id);
    json_object_set_string_member(entry, "url", mint->url);
    if (mint->label && *mint->label)
      json_object_set_string_member(entry, "label", mint->label);
    json_object_set_int_member(entry, "created_at",
                               g_get_real_time() / G_USEC_PER_SEC);
    if (mint->expires_at > 0)
      json_object_set_int_member(entry, "expires_at", mint->expires_at);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, entry);
    json_array_add_element(entries, node);
  }
  publish_invite_list(self);

  emit_update(self, mint->community_id, GN_CONCORD_UPDATE_MEMBERSHIP);
  if (mint->retiring)
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_pointer(task, g_strdup(mint->url), g_free);
  g_object_unref(task);
}

void gn_concord_community_service_create_invite_async(
    GnConcordCommunityService *self, const char *community_id,
    const char *label, gint64 expires_at, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self));

  const char *author = NULL;
  g_autoptr(GError) error = NULL;
  CommunityState *state =
    invite_precheck(self, community_id, &author, &error);
  if (!state) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }

  /* A fresh 16-byte unlock token and a fresh link signer, both from the same
   * CSPRNG the rest of the client's key material comes from. The signer is
   * used for nothing else, so a link-holder can never replace or tombstone
   * the bundle: posting to the coordinate takes a secret only the creator
   * holds (CORD-05 §2). */
  uint8_t token[CONCORD_INVITE_TOKEN_BYTES];
  uint8_t entropy[32];
  g_autofree gchar *entropy_hex = nostr_key_generate_private();
  g_autofree gchar *signer_sk =
    entropy_hex ? nostr_key_generate_private() : NULL;
  g_autofree gchar *signer_pk =
    signer_sk ? nostr_key_get_public(signer_sk) : NULL;
  if (!signer_pk || !nostr_concord_hex_decode_32(entropy_hex, entropy)) {
    if (entropy_hex) memset(entropy_hex, 0, strlen(entropy_hex));
    if (signer_sk) memset(signer_sk, 0, strlen(signer_sk));
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_FAILED,
                         "This device could not mint an invite link's keys");
    return;
  }
  memcpy(token, entropy, CONCORD_INVITE_TOKEN_BYTES);
  memset(entropy, 0, sizeof(entropy));
  memset(entropy_hex, 0, strlen(entropy_hex));

  g_autofree gchar *url = build_invite_url(state, token, signer_pk, &error);
  if (!url) {
    memset(token, 0, sizeof(token));
    memset(signer_sk, 0, strlen(signer_sk));
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }

  /* The bundle is the membership subset plus the link's own attribution. The
   * `control_root` is deliberately absent: a link hands out membership, never
   * the staff write key (CORD-02 §2). */
  JsonNode *material = build_join_material(state, FALSE);
  JsonObject *bundle = json_node_get_object(material);
  json_object_set_string_member(bundle, "creator_npub", author);
  if (label && *label) json_object_set_string_member(bundle, "label", label);
  if (expires_at > 0)
    json_object_set_int_member(bundle, "expires_at", expires_at);
  g_autofree gchar *bundle_json = node_to_json(material);
  json_node_free(material);

  char *content = NULL;
  nostr_concord_status_t status =
    bundle_json ? nostr_concord_invite_bundle_encrypt(bundle_json, token,
                                                      &content)
                : NOSTR_CONCORD_ERR_NULL;
  if (bundle_json) memset(bundle_json, 0, strlen(bundle_json));
  if (status != NOSTR_CONCORD_OK || !content) {
    memset(token, 0, sizeof(token));
    memset(signer_sk, 0, strlen(signer_sk));
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_FAILED,
                         "The invite bundle could not be encrypted");
    return;
  }

  /* One of the few Concord events relays see bare: an addressable event at
   * (33301, link_signer, ""), signed by the link signer itself. */
  g_autoptr(NostrEvent) event = nostr_event_new();
  nostr_event_set_kind(event, CONCORD_INVITE_BUNDLE);
  nostr_event_set_pubkey(event, signer_pk);
  nostr_event_set_created_at(event, (gint64)time(NULL));
  nostr_event_set_content(event, content);
  nostr_event_set_tags(event, nostr_tags_new(2, nostr_tag_new("d", "", NULL),
                                             nostr_tag_new("vsk", "6", NULL)));
  free(content);
  g_autofree gchar *event_json =
    nostr_event_sign(event, signer_sk) == 0
      ? nostr_event_serialize_compact(event) : NULL;
  if (!event_json) {
    memset(token, 0, sizeof(token));
    memset(signer_sk, 0, strlen(signer_sk));
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_FAILED,
                         "The invite bundle could not be signed");
    return;
  }

  char token_hex[65];
  uint8_t padded[32] = { 0 };
  memcpy(padded, token, CONCORD_INVITE_TOKEN_BYTES);
  nostr_concord_hex_encode_32(padded, token_hex);
  token_hex[CONCORD_INVITE_TOKEN_BYTES * 2] = '\0';

  InviteMint *mint = g_new0(InviteMint, 1);
  mint->community_id = g_strdup(community_id);
  mint->author = g_strdup(author);
  mint->label = g_strdup(label);
  mint->expires_at = expires_at;
  mint->token_hex = g_strdup(token_hex);
  mint->signer_sk = g_strdup(signer_sk);
  mint->signer_pk = g_strdup(signer_pk);
  mint->url = g_steal_pointer(&url);
  memset(token, 0, sizeof(token));
  memset(token_hex, 0, sizeof(token_hex));
  memset(signer_sk, 0, strlen(signer_sk));

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_task_data(task, mint, invite_mint_free);
  gnostr_plugin_context_publish_event_async(self->context, event_json,
                                            cancellable,
                                            on_invite_bundle_published, task);
}

gchar *gn_concord_community_service_create_invite_finish(
    GnConcordCommunityService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

void gn_concord_community_service_revoke_invite_async(
    GnConcordCommunityService *self, const char *community_id,
    const char *token_hex, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self));

  const char *author = NULL;
  g_autoptr(GError) error = NULL;
  CommunityState *state =
    invite_precheck(self, community_id, &author, &error);
  if (!state) {
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_return_error(task, g_steal_pointer(&error));
    g_object_unref(task);
    return;
  }

  /* Retiring takes the link signer's secret, which lives in exactly one
   * place: the creator's own Invite List (CORD-05 §2, §4). */
  g_autoptr(GPtrArray) entries =
    invite_live_entries(self->invite_document, community_id);
  JsonObject *entry = NULL;
  for (guint i = 0; i < entries->len && !entry; i++) {
    JsonObject *candidate = g_ptr_array_index(entries, i);
    if (g_strcmp0(object_string(candidate, "token"), token_hex) == 0)
      entry = candidate;
  }
  g_autofree gchar *signer_sk =
    entry ? g_strdup(object_string(entry, "signer_sk")) : NULL;
  g_autofree gchar *signer_pk = entry ? invite_entry_signer(entry) : NULL;
  if (!signer_pk || !signer_sk) {
    if (signer_sk) memset(signer_sk, 0, strlen(signer_sk));
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_NOT_FOUND,
                         "That link is not one this npub can retire");
    return;
  }

  /* The tombstone replaces the bundle at its own coordinate, so it is exactly
   * as durable as what it replaces — unlike a relay deletion, which is
   * best-effort and ignorable (CORD-05 §2). */
  g_autoptr(NostrEvent) event = nostr_event_new();
  nostr_event_set_kind(event, CONCORD_INVITE_BUNDLE);
  nostr_event_set_pubkey(event, signer_pk);
  nostr_event_set_created_at(event, (gint64)time(NULL));
  nostr_event_set_content(event, "");
  nostr_event_set_tags(event, nostr_tags_new(2, nostr_tag_new("d", "", NULL),
                                             nostr_tag_new("vsk", "9", NULL)));
  g_autofree gchar *event_json =
    nostr_event_sign(event, signer_sk) == 0
      ? nostr_event_serialize_compact(event) : NULL;
  memset(signer_sk, 0, strlen(signer_sk));
  if (!event_json) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_FAILED,
                         "The revocation tombstone could not be signed");
    return;
  }

  InviteMint *mint = g_new0(InviteMint, 1);
  mint->community_id = g_strdup(community_id);
  mint->author = g_strdup(author);
  mint->token_hex = g_strdup(token_hex);
  mint->signer_pk = g_strdup(signer_pk);
  mint->retiring = TRUE;

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_task_data(task, mint, invite_mint_free);
  gnostr_plugin_context_publish_event_async(self->context, event_json,
                                            cancellable,
                                            on_invite_bundle_published, task);
}

gboolean gn_concord_community_service_revoke_invite_finish(
    GnConcordCommunityService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ------------------------------------------------------------------ *
 * lifecycle and accessors
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * Refoundings: the base rotation (CORD-06)
 *
 * A Refounding is the whole-Community rekey that actually severs a removed
 * member: retiring their invite link stops new joins and nothing else, since
 * everyone who ever opened it still holds the `community_root` and with it
 * every plane. Only rolling the root cuts them off.
 *
 * The rotation is one act with two halves, and both live here because they
 * are the same act seen from either side. A Rotator mints a fresh root and a
 * fresh `control_root` beside it (CORD-02 §2), wraps the pair once per
 * recipient under the pairwise key it shares with them, and publishes the
 * located blobs at an address derived from the *prior* root — so a member who
 * missed the last rotation also missed this one's address, which is exactly
 * what removal is. A recipient finds its own blob by locator, checks
 * continuity and authority, and adopts the same keys. Nothing distinguishes
 * the two beyond who holds the pen.
 *
 * Every step is idempotent: re-sending blobs re-delivers the same keys, and
 * the state being rotated is acquired in full before the first publish, so a
 * crashed Rotator resumes rather than leaving half a rotation as the only
 * copy.
 * ------------------------------------------------------------------ */

/* The address the next base rotation lands at. Both the key and the epoch are
 * the *prior* root at root_epoch + 1: precomputable by every current
 * keyholder and by nobody else (CORD-06 §2). */
static gboolean derive_base_rekey_key(CommunityState *state, guint64 new_epoch,
                                      nostr_concord_group_key_t *out) {
  uint8_t root[32], id[32];
  if (!state->community_root ||
      !nostr_concord_hex_decode_32(state->community_root, root) ||
      !nostr_concord_hex_decode_32(state->community_id, id))
    return FALSE;
  nostr_concord_status_t status =
    nostr_concord_base_rekey_key(root, id, new_epoch, out);
  memset(root, 0, sizeof(root));
  return status == NOSTR_CONCORD_OK;
}

/* The continuity commitment over the key this device holds right now
 * (CORD-02 A.5). A rotation carries it as `prevcommit`, and equality proves
 * the rotation extends this very key rather than forking from another. */
static gboolean held_epoch_commitment(CommunityState *state, char out[65]) {
  uint8_t root[32], commitment[32];
  if (!state->community_root ||
      !nostr_concord_hex_decode_32(state->community_root, root))
    return FALSE;
  nostr_concord_status_t status =
    nostr_concord_epoch_commitment(state->root_epoch, root, commitment);
  memset(root, 0, sizeof(root));
  if (status != NOSTR_CONCORD_OK) return FALSE;
  nostr_concord_hex_encode_32(commitment, out);
  return TRUE;
}

/* Where @recipient finds its blob in a base rotation @rotator minted for
 * @epoch. Every input is public, so a NIP-46 bunker account computes its own
 * locator without touching a private key (CORD-06 §2). */
static gboolean base_locator_hex(const char *rotator, const char *recipient,
                                 guint64 epoch, char out[65]) {
  uint8_t rotator_x[32], recipient_x[32], scope[32], locator[32];
  nostr_concord_base_scope_id(scope);
  if (!rotator || !recipient ||
      !nostr_concord_hex_decode_32(rotator, rotator_x) ||
      !nostr_concord_hex_decode_32(recipient, recipient_x) ||
      nostr_concord_rekey_locator(rotator_x, recipient_x, scope, epoch,
                                  locator) != NOSTR_CONCORD_OK)
    return FALSE;
  nostr_concord_hex_encode_32(locator, out);
  return TRUE;
}

/* CORD-04 §3: staff is the set holding any Control-writing permission, plus
 * always the owner — exactly the set that holds the `control_root`, so
 * exactly the set whose blob carries it forward. */
static gboolean recipient_is_staff(CommunityState *state,
                                   const char *pubkey_hex) {
  if (g_strcmp0(pubkey_hex, state->owner) == 0) return TRUE;
  if (!state->control) return FALSE;
  return (gn_concord_control_plane_get_permissions(state->control,
                                                   pubkey_hex) &
          CONCORD_PERMS_STAFF) != 0;
}

/* A Rotator strictly outranks a target, never merely ties: equal cannot act
 * on equal (CORD-04 §3). */
static gboolean rotator_outranks(CommunityState *state, const char *rotator,
                                 const char *target) {
  if (!state->control) return FALSE;
  return gn_concord_control_plane_get_position(state->control, rotator) <
         gn_concord_control_plane_get_position(state->control, target);
}

/* CORD-06 "Authority": a Refounding takes BAN, and holding a key is never
 * authority — a removed member still holding the prior root can construct a
 * perfectly shaped rotation, and every honest member drops it here. */
static gboolean rotator_may_refound(CommunityState *state,
                                    const char *rotator) {
  if (!rotator || !state->control) return FALSE;
  if (gn_concord_control_plane_is_banned(state->control, rotator))
    return FALSE;
  return (gn_concord_control_plane_get_permissions(state->control, rotator) &
          CONCORD_PERM_BAN) != 0;
}

/* Re-points a plane at the epoch just adopted. The folds are deliberately
 * kept: an existing member carries its Control fold and its Guestbook across
 * the boundary, and only a fresh joiner waits on the re-anchor (CORD-06 §3).
 * A rotation that dropped them would turn every Refounding into an outage. */
static void readdress_planes(GnConcordCommunityService *self,
                             CommunityState *state) {
  if (ensure_control_plane(state)) {
    if (self->context && state->control_subscription)
      gnostr_plugin_context_unsubscribe_events(self->context,
                                               state->control_subscription);
    state->control_subscription = 0;
    g_free(state->control_address);
    state->control_address = NULL;
    if (state->control_pk) {
      state->control_address = g_strdup(state->control_pk);
    } else {
      nostr_concord_group_key_t key;
      if (derive_control_read_key(state, &key)) {
        char address[65];
        nostr_concord_hex_encode_32(key.pk, address);
        state->control_address = g_strdup(address);
        nostr_concord_group_key_clear(&key);
      }
    }
  }

  if (ensure_guestbook(state)) {
    if (self->context && state->guestbook_subscription)
      gnostr_plugin_context_unsubscribe_events(self->context,
                                               state->guestbook_subscription);
    state->guestbook_subscription = 0;
    nostr_concord_group_key_t key;
    if (derive_guestbook_key(state, &key)) {
      char address[65];
      nostr_concord_hex_encode_32(key.pk, address);
      g_free(state->guestbook_address);
      state->guestbook_address = g_strdup(address);
      nostr_concord_group_key_clear(&key);
    }
  }

  /* A Public Channel's key comes from the community_root, so its address
   * moved with the root; a Private Channel is independently keyed and did
   * not (CORD-03), but re-deriving both is cheap and keeps one path. */
  if (state->item) {
    if (self->context && state->subscriptions) {
      GHashTableIter subs;
      gpointer value;
      g_hash_table_iter_init(&subs, state->subscriptions);
      while (g_hash_table_iter_next(&subs, NULL, &value))
        gnostr_plugin_context_unsubscribe_events(self->context,
                                                 *(guint64 *)value);
      g_hash_table_remove_all(state->subscriptions);
    }
    GListModel *channels = gn_concord_community_item_get_channels(state->item);
    guint n = g_list_model_get_n_items(channels);
    for (guint i = 0; i < n; i++) {
      g_autoptr(GnConcordChannelItem) channel =
        g_list_model_get_item(channels, i);
      bind_channel_stream(state, channel);
    }
  }
}

static void refresh_rekey_plane(GnConcordCommunityService *self,
                                CommunityState *state);

/* The local half of a rotation, run identically by the Rotator that minted it
 * and by every recipient that opened a blob. The debt a Refounding pays needs
 * no clearing: it is recorded as the epoch that would pay it, so rolling the
 * root that far *is* the payment (CORD-06 §3). */
static void adopt_base_rotation(GnConcordCommunityService *self,
                                CommunityState *state,
                                const char *new_root_hex,
                                const char *new_control_pk_hex,
                                const char *new_control_root_hex,
                                guint64 new_epoch, const char *refounder) {
  clear_secret(&state->community_root);
  state->community_root = g_strdup(new_root_hex);
  state->root_epoch = new_epoch;
  g_free(state->control_pk);
  state->control_pk = g_strdup(new_control_pk_hex);
  /* A member's blob carries no control_root, and the one this device held is
   * dead the moment the root rolls: keeping it would leave a staffer writing
   * at an address nobody reads. Staff receive the new secret in their own
   * 136-byte blob (CORD-06 §1). */
  clear_secret(&state->control_root);
  if (new_control_root_hex) state->control_root = g_strdup(new_control_root_hex);

  readdress_planes(self, state);

  /* The Rotator's npub is the only one whose Guestbook snapshots this client
   * honors at the new epoch (CORD-02 §5). */
  if (refounder && ensure_guestbook(state))
    gn_concord_guestbook_set_refounder(state->guestbook, refounder);

  /* Drop rotations pending at the epoch just left: their address, their
   * continuity point and their blobs all belong to a boundary now behind us. */
  if (state->rotations) g_hash_table_remove_all(state->rotations);

  refresh_rekey_plane(self, state);
  if (self->context && !self->shutting_down) {
    GListModel *channels =
      state->item ? gn_concord_community_item_get_channels(state->item) : NULL;
    guint n = channels ? g_list_model_get_n_items(channels) : 0;
    for (guint i = 0; i < n; i++) {
      g_autoptr(GnConcordChannelItem) channel =
        g_list_model_get_item(channels, i);
      gn_concord_community_service_refresh_channel(
        self, state->community_id, gn_concord_channel_item_get_id(channel));
    }
    refresh_control_plane(self, state);
    refresh_guestbook(self, state);
  }

  emit_update(self, state->community_id,
              GN_CONCORD_UPDATE_MEMBERSHIP | GN_CONCORD_UPDATE_CHANNELS);
  /* The new keys are this membership's join material now, and a staffer's own
   * devices learn the fresh control_root the same way (CORD-02 §8). */
  publish_community_list(self);
}

/* ---- minting a rotation ---- */

typedef struct {
  gchar *community_id;
  gchar *author;         /* the Rotator: this npub */
  GPtrArray *recipients; /* (element-type utf8) the Complete Memberlist */
  guint index;           /* the recipient being wrapped */
  JsonArray *blobs;      /* {locator, wrapped}, one per recipient so far */
  gchar *new_root;
  gchar *new_control_root;
  gchar *new_control_pk;
  gchar *prevcommit;
  guint64 new_epoch;
  guint64 prev_epoch;
  guint chunk;
  guint n_chunks;
} RefoundMint;

static void refound_mint_free(gpointer data) {
  RefoundMint *mint = data;
  if (!mint) return;
  g_free(mint->community_id);
  g_free(mint->author);
  g_clear_pointer(&mint->recipients, g_ptr_array_unref);
  g_clear_pointer(&mint->blobs, json_array_unref);
  clear_secret(&mint->new_root);
  clear_secret(&mint->new_control_root);
  g_free(mint->new_control_pk);
  g_free(mint->prevcommit);
  g_free(mint);
}

static void refound_wrap_next(GTask *task);
static void refound_publish_next_chunk(GTask *task);

static void on_refound_blob_wrapped(GObject *source, GAsyncResult *result,
                                    gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  GnConcordCommunityService *self = g_task_get_source_object(task);
  RefoundMint *mint = g_task_get_task_data(task);

  if (self->shutting_down || !self->context) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "The Concord service was deactivated");
    g_object_unref(task);
    return;
  }

  g_autoptr(GError) error = NULL;
  g_autofree gchar *wrapped =
    gnostr_plugin_context_nip44_encrypt_bytes_finish(self->context, result,
                                                     &error);
  if (!wrapped) {
    /* One unwrappable recipient fails the whole rotation rather than
     * publishing a set that silently drops them: an absent blob is a removal,
     * and a removal must be deliberate. */
    if (error) g_task_return_error(task, g_steal_pointer(&error));
    else
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "A rekey blob could not be wrapped");
    g_object_unref(task);
    return;
  }

  const char *recipient = g_ptr_array_index(mint->recipients, mint->index);
  char locator[65];
  if (!base_locator_hex(mint->author, recipient, mint->new_epoch, locator)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "A recipient's locator could not be derived");
    g_object_unref(task);
    return;
  }

  JsonObject *entry = json_object_new();
  json_object_set_string_member(entry, "locator", locator);
  json_object_set_string_member(entry, "wrapped", wrapped);
  json_array_add_object_element(mint->blobs, entry);

  mint->index++;
  refound_wrap_next(task);
}

/* One blob per member, wrapped under the Rotator↔recipient conversation key.
 * The plaintext is 104 or 136 raw bytes whose width declares its form, so it
 * rides the binary pairwise lane (nostrc-3m86) — the string one would rewrite
 * a byte and turn the blob into garbage of the wrong length. */
static void refound_wrap_next(GTask *task) {
  GnConcordCommunityService *self = g_task_get_source_object(task);
  RefoundMint *mint = g_task_get_task_data(task);
  CommunityState *state = find_state(self, mint->community_id);

  if (!state || self->shutting_down || !self->context) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "The Concord service was deactivated");
    g_object_unref(task);
    return;
  }

  if (mint->index >= mint->recipients->len) {
    mint->n_chunks =
      (mint->recipients->len + CONCORD_REKEY_BLOBS_PER_EVENT - 1) /
      CONCORD_REKEY_BLOBS_PER_EVENT;
    refound_publish_next_chunk(task);
    return;
  }

  const char *recipient = g_ptr_array_index(mint->recipients, mint->index);
  nostr_concord_rekey_blob_t blob;
  memset(&blob, 0, sizeof(blob));
  blob.form = recipient_is_staff(state, recipient)
                ? NOSTR_CONCORD_REKEY_BASE_STAFF
                : NOSTR_CONCORD_REKEY_BASE_MEMBER;
  nostr_concord_base_scope_id(blob.scope_id);
  blob.epoch = mint->new_epoch;

  gboolean packed_ok =
    nostr_concord_hex_decode_32(mint->new_root, blob.new_key) &&
    nostr_concord_hex_decode_32(mint->new_control_pk, blob.new_control_pk);
  blob.has_control_pk = packed_ok;
  if (packed_ok && blob.form == NOSTR_CONCORD_REKEY_BASE_STAFF) {
    packed_ok =
      nostr_concord_hex_decode_32(mint->new_control_root, blob.new_control_root);
    blob.has_control_root = packed_ok;
  }

  uint8_t plaintext[CONCORD_REKEY_BLOB_STAFF_BYTES];
  size_t len = 0;
  if (!packed_ok ||
      nostr_concord_rekey_blob_pack(&blob, plaintext, sizeof(plaintext),
                                    &len) != NOSTR_CONCORD_OK) {
    nostr_concord_rekey_blob_clear(&blob);
    memset(plaintext, 0, sizeof(plaintext));
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "A rekey blob could not be packed");
    g_object_unref(task);
    return;
  }
  nostr_concord_rekey_blob_clear(&blob);

  g_autoptr(GBytes) bytes = g_bytes_new(plaintext, len);
  memset(plaintext, 0, sizeof(plaintext));
  gnostr_plugin_context_nip44_encrypt_bytes_async(
    self->context, recipient, bytes, g_task_get_cancellable(task),
    on_refound_blob_wrapped, task);
}

static void on_refound_chunk_published(GObject *source, GAsyncResult *result,
                                       gpointer user_data) {
  (void)source;
  GTask *task = G_TASK(user_data);
  RefoundMint *mint = g_task_get_task_data(task);

  g_autoptr(GError) error = NULL;
  if (!g_task_propagate_boolean(G_TASK(result), &error)) {
    if (error) g_task_return_error(task, g_steal_pointer(&error));
    else
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "A rekey chunk could not be published");
    g_object_unref(task);
    return;
  }
  mint->chunk++;
  refound_publish_next_chunk(task);
}

static void refound_publish_next_chunk(GTask *task) {
  GnConcordCommunityService *self = g_task_get_source_object(task);
  RefoundMint *mint = g_task_get_task_data(task);
  CommunityState *state = find_state(self, mint->community_id);

  if (!state || self->shutting_down || !self->context) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "The Concord service was deactivated");
    g_object_unref(task);
    return;
  }

  if (mint->chunk >= mint->n_chunks) {
    /* Publication confirmed for every chunk: only now does this device move,
     * so a mid-flight failure leaves the community on the old epoch with the
     * rotation resumable rather than stranded on a root nobody received. */
    adopt_base_rotation(self, state, mint->new_root, mint->new_control_pk,
                        mint->new_control_root, mint->new_epoch, mint->author);
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
    return;
  }

  guint first = mint->chunk * CONCORD_REKEY_BLOBS_PER_EVENT;
  guint count =
    MIN(CONCORD_REKEY_BLOBS_PER_EVENT, mint->recipients->len - first);

  g_autoptr(JsonArray) chunk_blobs = json_array_new();
  for (guint i = 0; i < count; i++)
    json_array_add_element(
      chunk_blobs, json_node_copy(json_array_get_element(mint->blobs,
                                                         first + i)));
  g_autoptr(JsonNode) content_node = json_node_new(JSON_NODE_ARRAY);
  json_node_set_array(content_node, chunk_blobs);
  g_autofree gchar *content = node_to_json(content_node);

  PublishContext *publish = g_new0(PublishContext, 1);
  publish->author = g_strdup(mint->author);
  if (!content ||
      !derive_base_rekey_key(state, mint->new_epoch, &publish->key)) {
    publish_context_free(publish);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "The base rekey address could not be derived");
    g_object_unref(task);
    return;
  }

  char scope_hex[65];
  uint8_t scope[32];
  nostr_concord_base_scope_id(scope);
  nostr_concord_hex_encode_32(scope, scope_hex);
  g_autofree gchar *newepoch_text =
    g_strdup_printf("%" G_GUINT64_FORMAT, mint->new_epoch);
  g_autofree gchar *prevepoch_text =
    g_strdup_printf("%" G_GUINT64_FORMAT, mint->prev_epoch);
  g_autofree gchar *index_text = g_strdup_printf("%u", mint->chunk);
  g_autofree gchar *total_text = g_strdup_printf("%u", mint->n_chunks);

  gint64 created_at = 0;
  (void)split_now(&created_at);

  g_autoptr(NostrEvent) rumor = nostr_event_new();
  nostr_event_set_kind(rumor, CONCORD_KIND_REKEY);
  nostr_event_set_pubkey(rumor, mint->author);
  nostr_event_set_created_at(rumor, created_at);
  nostr_event_set_content(rumor, content);

  NostrTags *tags = nostr_tags_new(0);
  tags = nostr_tags_append_unique(tags,
                                  nostr_tag_new("scope", scope_hex, NULL));
  tags = nostr_tags_append_unique(
    tags, nostr_tag_new("newepoch", newepoch_text, NULL));
  tags = nostr_tags_append_unique(
    tags, nostr_tag_new("prevepoch", prevepoch_text, NULL));
  tags = nostr_tags_append_unique(
    tags, nostr_tag_new("prevcommit", mint->prevcommit, NULL));
  tags = nostr_tags_append_unique(
    tags, nostr_tag_new("chunk", index_text, total_text, NULL));
  /* A rotation cites the Grant it acts under like any authority action, so a
   * client that has not synced the Grant parks it instead of dropping it, and
   * a just-demoted admin's rotation is never honored by a lagging one
   * (CORD-04 §5). */
  append_grant_citation(state, mint->author, tags);
  nostr_event_set_tags(rumor, tags);

  publish_sealed_rumor(self, publish, rumor, created_at,
                       g_task_get_cancellable(task),
                       on_refound_chunk_published, task);
}

void gn_concord_community_service_refound_async(
    GnConcordCommunityService *self, const char *community_id,
    GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self));

  const char *author = gn_concord_community_service_get_current_pubkey(self);
  CommunityState *state = find_state(self, community_id);
  if (!author || !state) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_PERMISSION_DENIED,
                         "Sign in to a Community you hold to refound it");
    return;
  }
  if (self->shutting_down || !self->context) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_CLOSED,
                         "The Concord service is not connected");
    return;
  }
  /* A Refounding takes BAN — it is the removal a ban means (CORD-06). The
   * Roster is what answers that, so the plane has to exist to be asked. */
  ensure_control_plane(state);
  if (!rotator_may_refound(state, author)) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_PERMISSION_DENIED,
                         "Refounding this Community takes the BAN permission");
    return;
  }

  /* Acquire the whole state being rotated before the first publish: the
   * Memberlist decides who receives a blob, and a member missing from it is a
   * member removed (CORD-06 "Failure and races"). */
  g_autoptr(GPtrArray) members =
    gn_concord_community_service_get_members(self, community_id);
  if (!members || members->len == 0) {
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_FAILED,
                         "This Community's Memberlist is not loaded yet");
    return;
  }

  RefoundMint *mint = g_new0(RefoundMint, 1);
  mint->community_id = g_strdup(community_id);
  mint->author = g_strdup(author);
  mint->recipients = g_ptr_array_new_with_free_func(g_free);
  mint->blobs = json_array_new();
  mint->prev_epoch = state->root_epoch;
  mint->new_epoch = state->root_epoch + 1;

  gboolean holds_self = FALSE;
  for (guint i = 0; i < members->len; i++) {
    const char *member = g_ptr_array_index(members, i);
    if (!nostr_concord_is_lower_hex_32(member)) continue;
    if (g_strcmp0(member, author) == 0) holds_self = TRUE;
    g_ptr_array_add(mint->recipients, g_strdup(member));
  }
  /* The Rotator is a recipient like any other: a rotation that forgot to wrap
   * a blob for itself would lock its own author out of the epoch it minted. */
  if (!holds_self) g_ptr_array_add(mint->recipients, g_strdup(author));

  /* The root roll and the control_root split are minted together and travel
   * in the same blobs, so converging on a base rotation converges both — and
   * a Community minted before the split upgrades as a side effect of its next
   * ban, with nobody deciding to (CORD-06 §3). */
  mint->new_root = nostr_key_generate_private();
  mint->new_control_root = nostr_key_generate_private();
  char commitment[65];
  uint8_t control_root[32], id[32];
  nostr_concord_group_key_t control_signer;
  gboolean minted =
    mint->new_root && mint->new_control_root &&
    held_epoch_commitment(state, commitment) &&
    nostr_concord_hex_decode_32(mint->new_control_root, control_root) &&
    nostr_concord_hex_decode_32(state->community_id, id) &&
    nostr_concord_control_signer_key(control_root, id, mint->new_epoch,
                                     &control_signer) == NOSTR_CONCORD_OK;
  memset(control_root, 0, sizeof(control_root));
  if (!minted) {
    refound_mint_free(mint);
    return_publish_error(self, cancellable, callback, user_data,
                         G_IO_ERROR_FAILED,
                         "This Community's next epoch could not be minted");
    return;
  }
  char control_pk[65];
  nostr_concord_hex_encode_32(control_signer.pk, control_pk);
  nostr_concord_group_key_clear(&control_signer);
  mint->new_control_pk = g_strdup(control_pk);
  mint->prevcommit = g_strdup(commitment);

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_task_data(task, mint, refound_mint_free);
  refound_wrap_next(task);
}

gboolean gn_concord_community_service_refound_finish(
    GnConcordCommunityService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}

/* ---- receiving a rotation ----
 *
 * A rotation to many recipients spans several events, each tagged
 * ["chunk", i, n] and correlated by its Rotator at one newepoch and
 * prevcommit — so two Rotators racing the same epoch never merge into one
 * set, and a partial set is never mistaken for a complete one. That
 * distinction is the whole safety property on this side: a missing chunk is
 * never a removal, and only a complete set with no blob for this npub means
 * anything at all. */

typedef struct {
  gchar *locator;
  gchar *wrapped;
} RekeyEntry;

static void rekey_entry_free(gpointer data) {
  RekeyEntry *entry = data;
  if (!entry) return;
  g_free(entry->locator);
  g_free(entry->wrapped);
  g_free(entry);
}

typedef struct {
  gchar *rotator;
  guint64 new_epoch;
  guint64 prev_epoch;
  gchar *prevcommit;
  guint n_chunks;
  GHashTable *chunks;  /* chunk index -> GPtrArray(RekeyEntry) */
  gboolean adopting;   /* a blob for this npub is already being opened */
  gboolean concluded;  /* the complete set has been judged once */
} PendingRotation;

static void pending_rotation_free(gpointer data) {
  PendingRotation *pending = data;
  if (!pending) return;
  g_free(pending->rotator);
  g_free(pending->prevcommit);
  g_clear_pointer(&pending->chunks, g_hash_table_unref);
  g_free(pending);
}

/* One rotation is one Rotator at one continuity point. Two Rotators rekeying
 * the same epoch are two rotations under this key, never one merged set. */
static gchar *rotation_key(const char *rotator, guint64 new_epoch,
                           const char *prevcommit) {
  return g_strdup_printf("%s|%" G_GUINT64_FORMAT "|%s", rotator, new_epoch,
                         prevcommit);
}

typedef struct {
  /* Held, not borrowed: opening a blob is the one Concord operation that can
   * outlive the service, because a signer answers on its own schedule. A
   * subscription is cancelled at shutdown and a GTask holds its own source
   * object; this has neither, so it keeps the service alive and finds
   * `shutting_down` set rather than freed memory. */
  GnConcordCommunityService *service;
  gchar *community_id;
  gchar *rotator;
  gchar *rotation_key;
  guint64 new_epoch;
} RekeyAdopt;

static void rekey_adopt_free(gpointer data) {
  RekeyAdopt *adopt = data;
  if (!adopt) return;
  g_clear_object(&adopt->service);
  g_free(adopt->community_id);
  g_free(adopt->rotator);
  g_free(adopt->rotation_key);
  g_free(adopt);
}

static void on_rekey_blob_opened(GObject *source, GAsyncResult *result,
                                 gpointer user_data) {
  (void)source;
  RekeyAdopt *adopt = user_data;
  GnConcordCommunityService *self = adopt->service;
  CommunityState *state =
    self->shutting_down ? NULL : find_state(self, adopt->community_id);
  PendingRotation *pending =
    state && state->rotations
      ? g_hash_table_lookup(state->rotations, adopt->rotation_key)
      : NULL;
  if (pending) pending->adopting = FALSE;

  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) plaintext =
    state ? gnostr_plugin_context_nip44_decrypt_bytes_finish(self->context,
                                                             result, &error)
          : NULL;
  if (!state || !plaintext) {
    if (state && error) emit_error(self, error->message);
    rekey_adopt_free(adopt);
    return;
  }

  /* The blob's own scope and epoch are checked against the event's tags and
   * never read from them, which is what makes it unspliceable: a blob minted
   * for one scope or epoch can never be replayed against another. On the
   * staff form the control pair is verified too — new_control_root must
   * derive to exactly new_control_pk — so a recipient refuses a plane split
   * from its own readers rather than adopting it (CORD-06 §1). */
  uint8_t scope[32], community[32];
  nostr_concord_base_scope_id(scope);
  nostr_concord_rekey_blob_t blob;
  memset(&blob, 0, sizeof(blob));
  gsize len = 0;
  const guchar *bytes = g_bytes_get_data(plaintext, &len);
  if (!nostr_concord_hex_decode_32(state->community_id, community) ||
      nostr_concord_rekey_blob_parse(bytes, (size_t)len, community, scope,
                                     adopt->new_epoch, &blob) !=
        NOSTR_CONCORD_OK) {
    nostr_concord_rekey_blob_clear(&blob);
    emit_error(self, "A rekey blob for this Community was malformed");
    rekey_adopt_free(adopt);
    return;
  }

  char new_root[65], control_pk[65], control_root[65];
  nostr_concord_hex_encode_32(blob.new_key, new_root);
  /* A 72-byte base blob is the legacy, pre-split form: honored when reading
   * an old rotation, never minted anew, and its acceptor folds that epoch's
   * Control at the legacy address — which is what leaving control_pk NULL
   * selects (CORD-02 §5). */
  if (blob.has_control_pk)
    nostr_concord_hex_encode_32(blob.new_control_pk, control_pk);
  if (blob.has_control_root)
    nostr_concord_hex_encode_32(blob.new_control_root, control_root);

  adopt_base_rotation(self, state, new_root,
                      blob.has_control_pk ? control_pk : NULL,
                      blob.has_control_root ? control_root : NULL,
                      adopt->new_epoch, adopt->rotator);
  memset(new_root, 0, sizeof(new_root));
  memset(control_root, 0, sizeof(control_root));
  nostr_concord_rekey_blob_clear(&blob);
  rekey_adopt_free(adopt);
}

/* Runs once the held set is complete. Two questions can only be answered
 * here, and answering either early is the classic rekey bug: whether the
 * Rotator was allowed to remove the people it removed, and whether this npub
 * is one of them. */
static void conclude_rotation(GnConcordCommunityService *self,
                              CommunityState *state,
                              PendingRotation *pending) {
  if (pending->concluded) return;
  for (guint i = 0; i < pending->n_chunks; i++)
    if (!g_hash_table_lookup(pending->chunks, GUINT_TO_POINTER(i))) return;
  pending->concluded = TRUE;

  g_autoptr(GPtrArray) members =
    gn_concord_community_service_get_members(self, state->community_id);
  const char *me = gn_concord_community_service_get_current_pubkey(self);
  gboolean removed_self = FALSE;

  for (guint i = 0; members && i < members->len; i++) {
    const char *member = g_ptr_array_index(members, i);
    char locator[65];
    if (!base_locator_hex(pending->rotator, member, pending->new_epoch,
                          locator))
      continue;

    gboolean present = FALSE;
    for (guint c = 0; c < pending->n_chunks && !present; c++) {
      GPtrArray *entries =
        g_hash_table_lookup(pending->chunks, GUINT_TO_POINTER(c));
      for (guint e = 0; entries && e < entries->len; e++) {
        const RekeyEntry *entry = g_ptr_array_index(entries, e);
        if (g_strcmp0(entry->locator, locator) == 0) {
          present = TRUE;
          break;
        }
      }
    }
    if (present) continue;

    /* An absent blob is a removal, and a Rotator may only remove someone it
     * strictly outranks (CORD-06 "Authority"). A rotation that removes a
     * peer or a superior is not a rotation this client honors at all — not
     * even the part that happens to hand this npub a key. */
    if (!rotator_outranks(state, pending->rotator, member)) {
      emit_error(self,
                 "A Refounding tried to remove a member its Rotator does not "
                 "outrank, and was dropped");
      g_hash_table_remove(state->rotations,
                          rotation_key(pending->rotator, pending->new_epoch,
                                       pending->prevcommit));
      return;
    }
    if (g_strcmp0(member, me) == 0) removed_self = TRUE;
  }

  if (removed_self)
    emit_error(self,
               "This Community was refounded without this account: its new "
               "keys were not delivered here");
}

/* Opens one kind-1059 wrap from the base rekey address and folds the rotation
 * inside it. Public so the service tests can drive the whole adopt path with
 * no relay. */
gboolean gn_concord_community_service_ingest_rekey_wrap(
    GnConcordCommunityService *self, const char *community_id,
    const char *wrap_json) {
  g_return_val_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self), FALSE);
  if (self->shutting_down) return FALSE;
  CommunityState *state = find_state(self, community_id);
  if (!state) return FALSE;

  guint64 new_epoch = state->root_epoch + 1;
  nostr_concord_group_key_t key;
  /* Authority is judged against the folded Roster, so the plane has to exist
   * before a rotation can be honored or refused. */
  ensure_control_plane(state);
  if (!derive_base_rekey_key(state, new_epoch, &key)) return FALSE;

  gboolean accepted = FALSE;
  char *seal_json = NULL;
  char *rumor_json = NULL;
  g_autoptr(NostrEvent) wrap = NULL;
  g_autoptr(NostrEvent) seal = NULL;
  g_autoptr(NostrEvent) rumor = NULL;
  g_autoptr(JsonParser) parser = NULL;
  g_autofree gchar *pending_key = NULL;
  PendingRotation *pending = NULL;
  GPtrArray *entries = NULL;
  const char *rotator = NULL;
  const char *prevcommit = NULL;
  JsonArray *blobs = NULL;
  char address[65];
  char commitment[65];
  char own_locator[65];
  guint64 tag_epoch = 0;
  guint64 prev_epoch = 0;
  guint64 chunk_index = 0;
  guint64 chunk_total = 0;
  uint8_t scope[32], tag_scope[32];

  /* The address is derived from the root this device holds, so an event that
   * arrives here at all was published by someone who held that same root.
   * Ingest is reachable without a subscription, so check it anyway. */
  nostr_concord_hex_encode_32(key.pk, address);
  wrap = parse_verified_event(wrap_json);
  if (!wrap || nostr_event_get_kind(wrap) != CONCORD_STREAM_WRAP) goto done;
  if (g_strcmp0(nostr_event_get_pubkey(wrap), address) != 0) goto done;

  if (nostr_concord_stream_open(key.conv_key, nostr_event_get_content(wrap),
                                &seal_json) != NOSTR_CONCORD_OK)
    goto done;
  seal = parse_verified_event(seal_json);
  if (!seal || nostr_event_get_kind(seal) != CONCORD_SEAL_ENCRYPTED) goto done;
  if (nostr_concord_stream_open(key.conv_key, nostr_event_get_content(seal),
                                &rumor_json) != NOSTR_CONCORD_OK)
    goto done;
  rumor = parse_rumor(rumor_json);
  if (!rumor || nostr_event_get_kind(rumor) != CONCORD_KIND_REKEY) goto done;

  /* Only the seal's signature names the Rotator; a rumor claiming a different
   * author than its seal is a forgery. */
  rotator = nostr_event_get_pubkey(seal);
  if (g_strcmp0(nostr_event_get_pubkey(rumor), rotator) != 0) goto done;

  /* Holding a key is never authority: a removed member still holding the
   * prior root can construct a perfectly shaped rotation, and every honest
   * member opens the seal, folds the Roster, and drops it (CORD-06). */
  if (!rotator_may_refound(state, rotator)) goto done;

  /* A base rotation's scope is all-zeroes, which no Channel id collides with
   * (CORD-06 §1). A Channel rotation arriving here is not this plane's. */
  nostr_concord_base_scope_id(scope);
  if (!nostr_concord_hex_decode_32(event_tag_value(rumor, "scope"), tag_scope) ||
      memcmp(tag_scope, scope, sizeof(scope)) != 0)
    goto done;
  if (!parse_epoch(event_tag_value(rumor, "newepoch"), &tag_epoch) ||
      tag_epoch != new_epoch)
    goto done;

  /* Continuity (CORD-02 A.5): the commitment recomputed over the key held
   * here must equal the rotation's, which proves it extends this very key. A
   * mismatch at a *higher* prevepoch means a rotation was missed and the gap
   * must be fetched first; anything else is a fork or garbage. */
  prevcommit = event_tag_value(rumor, "prevcommit");
  if (!parse_epoch(event_tag_value(rumor, "prevepoch"), &prev_epoch) ||
      !prevcommit || !held_epoch_commitment(state, commitment))
    goto done;
  if (prev_epoch != state->root_epoch ||
      g_strcmp0(prevcommit, commitment) != 0) {
    if (prev_epoch > state->root_epoch)
      emit_error(self,
                 "A Refounding arrived for a later epoch than this device "
                 "holds: the missed rotation must be fetched first");
    goto done;
  }

  {
    NostrTags *tags = nostr_event_get_tags(rumor);
    NostrTag *chunk_tag = NULL;
    for (gsize i = 0; tags && i < nostr_tags_size(tags); i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (tag && nostr_tag_size(tag) >= 3 &&
          g_strcmp0(nostr_tag_get(tag, 0), "chunk") == 0) {
        chunk_tag = tag;
        break;
      }
    }
    if (!chunk_tag ||
        !parse_epoch(nostr_tag_get(chunk_tag, 1), &chunk_index) ||
        !parse_epoch(nostr_tag_get(chunk_tag, 2), &chunk_total) ||
        chunk_total == 0 || chunk_index >= chunk_total ||
        chunk_total > G_MAXUINT16)
      goto done;
  }

  parser = json_parser_new();
  if (!json_parser_load_from_data(parser, nostr_event_get_content(rumor), -1,
                                  NULL))
    goto done;
  {
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root)) goto done;
    blobs = json_node_get_array(root);
  }
  /* §1 caps an event at 120 blobs; a longer one is malformed, not generous. */
  if (json_array_get_length(blobs) > CONCORD_REKEY_BLOBS_PER_EVENT) goto done;

  if (!state->rotations)
    state->rotations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             pending_rotation_free);
  pending_key = rotation_key(rotator, new_epoch, prevcommit);
  pending = g_hash_table_lookup(state->rotations, pending_key);
  if (!pending) {
    pending = g_new0(PendingRotation, 1);
    pending->rotator = g_strdup(rotator);
    pending->new_epoch = new_epoch;
    pending->prev_epoch = prev_epoch;
    pending->prevcommit = g_strdup(prevcommit);
    pending->n_chunks = (guint)chunk_total;
    pending->chunks = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, (GDestroyNotify)g_ptr_array_unref);
    g_hash_table_insert(state->rotations, g_strdup(pending_key), pending);
  }
  /* All chunks of one rotation carry identical continuity fields and one
   * total; a chunk disagreeing about the size of the set it belongs to is not
   * part of it. */
  if (pending->n_chunks != (guint)chunk_total) goto done;

  entries = g_ptr_array_new_with_free_func(rekey_entry_free);
  for (guint i = 0; i < json_array_get_length(blobs); i++) {
    JsonNode *node = json_array_get_element(blobs, i);
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) continue;
    JsonObject *object = json_node_get_object(node);
    const char *locator = object_string(object, "locator");
    const char *wrapped = object_string(object, "wrapped");
    if (!nostr_concord_is_lower_hex_32(locator) || !wrapped || !*wrapped)
      continue;
    RekeyEntry *entry = g_new0(RekeyEntry, 1);
    entry->locator = g_strdup(locator);
    entry->wrapped = g_strdup(wrapped);
    g_ptr_array_add(entries, entry);
  }
  g_hash_table_insert(pending->chunks, GUINT_TO_POINTER((guint)chunk_index),
                      entries);
  accepted = TRUE;

  /* If any chunk carries this npub's locator, shift to the new epoch now —
   * waiting for the rest would stall every member behind the slowest fetch,
   * and a rotation that hands someone a key cannot also be removing them. */
  if (!pending->adopting) {
    const char *me = gn_concord_community_service_get_current_pubkey(self);
    if (me && base_locator_hex(rotator, me, new_epoch, own_locator)) {
      for (guint i = 0; i < entries->len; i++) {
        const RekeyEntry *entry = g_ptr_array_index(entries, i);
        if (g_strcmp0(entry->locator, own_locator) != 0) continue;
        RekeyAdopt *adopt = g_new0(RekeyAdopt, 1);
        adopt->service = g_object_ref(self);
        adopt->community_id = g_strdup(community_id);
        adopt->rotator = g_strdup(rotator);
        adopt->rotation_key = g_strdup(pending_key);
        adopt->new_epoch = new_epoch;
        pending->adopting = TRUE;
        gnostr_plugin_context_nip44_decrypt_bytes_async(
          self->context, rotator, entry->wrapped, NULL, on_rekey_blob_opened,
          adopt);
        break;
      }
    }
  }

  conclude_rotation(self, state, pending);

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
  return accepted;
}

typedef struct {
  GnConcordCommunityService *service; /* borrowed; the service owns the sub */
  gchar *community_id;
} RekeyBinding;

static void rekey_binding_free(gpointer data) {
  RekeyBinding *binding = data;
  if (!binding) return;
  g_free(binding->community_id);
  g_free(binding);
}

static void on_rekey_event(const char *event_json, gpointer user_data) {
  RekeyBinding *binding = user_data;
  if (!binding || !binding->service ||
      !GN_IS_CONCORD_COMMUNITY_SERVICE(binding->service))
    return;
  gn_concord_community_service_ingest_rekey_wrap(
    binding->service, binding->community_id, event_json);
}

/* Subscribes the next base rotation's address alongside every other plane. It
 * is precomputed from the root held now, so it is live before a rotation
 * exists — which is the only way to catch one in real time. */
static void refresh_rekey_plane(GnConcordCommunityService *self,
                                CommunityState *state) {
  if (!self->context || self->shutting_down) return;

  nostr_concord_group_key_t key;
  if (!derive_base_rekey_key(state, state->root_epoch + 1, &key)) return;
  char address[65];
  nostr_concord_hex_encode_32(key.pk, address);
  nostr_concord_group_key_clear(&key);

  if (g_strcmp0(state->rekey_address, address) != 0) {
    if (state->rekey_subscription)
      gnostr_plugin_context_unsubscribe_events(self->context,
                                               state->rekey_subscription);
    state->rekey_subscription = 0;
    g_free(state->rekey_address);
    state->rekey_address = g_strdup(address);
  }

  if (!state->rekey_subscription) {
    RekeyBinding *binding = g_new0(RekeyBinding, 1);
    binding->service = self;
    binding->community_id = g_strdup(state->community_id);
    g_autofree gchar *filter = build_stream_filter(address, 0);
    guint64 id = gnostr_plugin_context_subscribe_events(
      self->context, filter, G_CALLBACK(on_rekey_event), binding,
      rekey_binding_free);
    if (id) state->rekey_subscription = id;
    else rekey_binding_free(binding);
  }

  /* The backfill carries no page limit: a rotation is only meaningful as a
   * complete set, and a truncated one reads as a removal that never
   * happened. */
  g_autofree gchar *filter = build_stream_filter(address, 0);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events =
    gnostr_plugin_context_query_events(self->context, filter, &error);
  if (!events) {
    if (error) emit_error(self, error->message);
    return;
  }
  for (guint i = 0; i < events->len; i++)
    gn_concord_community_service_ingest_rekey_wrap(
      self, state->community_id, g_ptr_array_index(events, i));
}

void gn_concord_community_service_shutdown(GnConcordCommunityService *self) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_SERVICE(self));
  if (self->shutting_down) return;
  self->shutting_down = TRUE;

  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->states);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    CommunityState *state = value;
    if (self->context && state->control_subscription)
      gnostr_plugin_context_unsubscribe_events(self->context,
                                               state->control_subscription);
    state->control_subscription = 0;
    if (self->context && state->guestbook_subscription)
      gnostr_plugin_context_unsubscribe_events(self->context,
                                               state->guestbook_subscription);
    state->guestbook_subscription = 0;
    if (self->context && state->rekey_subscription)
      gnostr_plugin_context_unsubscribe_events(self->context,
                                               state->rekey_subscription);
    state->rekey_subscription = 0;
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
  g_clear_pointer(&self->list_document, json_object_unref);
  g_clear_pointer(&self->list_orphans, g_hash_table_unref);
  g_clear_pointer(&self->invite_document, json_object_unref);
  self->context = NULL;
  G_OBJECT_CLASS(gn_concord_community_service_parent_class)->dispose(object);
}

static void gn_concord_community_service_finalize(GObject *object) {
  GnConcordCommunityService *self = GN_CONCORD_COMMUNITY_SERVICE(object);
  g_free(self->offline_user_pubkey);
  g_free(self->list_author);
  g_free(self->invite_author);
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
  /* A Direct Invite is passive until the user decides: nothing joins,
   * subscribes or announces presence on the strength of one arriving
   * (CORD-05 §1, §6). The signal offers it; accepting is a separate act. */
  signals[INVITE_OFFERED] = g_signal_new(
    "invite-offered", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
    NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}

static void gn_concord_community_service_init(
    GnConcordCommunityService *self) {
  self->communities = g_list_store_new(GN_TYPE_CONCORD_COMMUNITY_ITEM);
  self->states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       community_state_free);
  self->list_orphans = g_hash_table_new_full(
    g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_free);
}

GnConcordCommunityService *gn_concord_community_service_new(
    GnostrPluginContext *context) {
  g_return_val_if_fail(context != NULL, NULL);
  GnConcordCommunityService *self =
    g_object_new(GN_TYPE_CONCORD_COMMUNITY_SERVICE, NULL);
  self->context = context;
  /* The List's decrypt is asynchronous, so the fold it seeds runs from the
   * completion; refresh here covers the no-document and offline paths. */
  load_community_list(self);
  /* The creator's own bookkeeping, and the one document that must be read
   * before anything replaces it: it holds every link's signing secret
   * (CORD-05 §4). */
  load_invite_list(self);
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
