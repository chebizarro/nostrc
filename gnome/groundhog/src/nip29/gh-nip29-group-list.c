#include "gh-nip29-group-list.h"

#include "gh-auth-policy.h"
#include "gh-identity.h"
#include "gh-nip29-group.h"

#include <json-glib/json-glib.h>
#include <nostr-event.h>
#include <nostr-tag.h>

enum { SOURCE_PENDING, SOURCE_EOSE, SOURCE_FAILED };

/* Publish retries for a relay that drops the connection or rate-limits. */
#define PUBLISH_RETRIES 2
#define PUBLISH_RETRY_BASE_MS 500

struct _GhNip29GroupList {
  GObject parent_instance;
  GhAccountController *accounts; /* NULL once disposed */
  GhAccountRelays *relays;
  GSettings *settings;
  GhNip29Service *service;       /* weak */
  GhRelayTransport scope_transport;
  GhRelayAuthTransport scope_auth;
  gboolean custom_scope, custom_scope_auth;
  gpointer scope_data;
  GhRelayPublishTransport publish_transport;
  GhRelayPublishAuthTransport publish_auth;
  gboolean custom_publish, custom_publish_auth;
  gpointer publish_data;
  guint publish_deadline;

  guint64 generation;            /* 0 while unbound */
  gchar *pubkey;
  GCancellable *cancellable;     /* the generation's signer calls */
  GhRelayScope *scope;
  GStrv urls;                    /* the scope's (and the publish's) relays */
  GHashTable *sources;           /* url -> SOURCE_* */

  /* The newest list seen or published, and what it says. */
  gchar *newest_json;
  gchar *newest_id;
  gint64 newest_created_at;
  guint decode_serial;
  gboolean decoding;             /* the newest list's private part is being decrypted */
  gboolean decoded;              /* public/private hold the newest list (or no list exists) */
  gboolean unreadable;
  GPtrArray *public_tags;        /* GStrv */
  GPtrArray *private_tags;       /* GStrv; NULL while unreadable */

  /* This device. */
  GHashTable *rooms;             /* room id -> its last LocalVerdict (+1) */
  GHashTable *removals;          /* room id -> gint64*: when it left this device */
  GHashTable *dismissed;         /* room ids not offered again this session */
  GPtrArray *restorable;         /* GhNip29ListedGroup */

  /* Publication. */
  gboolean publishing;
  gboolean failed;
  gchar *built_on;               /* newest_id the pending list was merged from */
  gchar *published_id;           /* our own event, coming back on the live REQ */
  GPtrArray *pending_public, *pending_private;
  gchar *pending_json;           /* the signed list being published */
  gchar *pending_id;
  gint64 pending_created_at;
  GhRelayPublish *publish;
  guint idle;
  GhNip29GroupListState state;
};

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhNip29GroupList, gh_nip29_group_list, G_TYPE_OBJECT)

void
gh_nip29_listed_group_free(GhNip29ListedGroup *group)
{
  if (!group)
    return;
  g_free(group->relay_url);
  g_free(group->group_id);
  g_free(group->name);
  g_free(group);
}

/* ---- tags --------------------------------------------------------------------- */

static GPtrArray *
tags_new(void)
{
  return g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
}

static GPtrArray *
tags_copy(GPtrArray *tags)
{
  GPtrArray *copy = tags_new();
  for (guint i = 0; tags && i < tags->len; i++)
    g_ptr_array_add(copy, g_strdupv(g_ptr_array_index(tags, i)));
  return copy;
}

static gboolean
tags_equal(GPtrArray *a, GPtrArray *b)
{
  guint na = a ? a->len : 0, nb = b ? b->len : 0;
  if (na != nb)
    return FALSE;
  for (guint i = 0; i < na; i++)
    if (!g_strv_equal((const gchar *const *)g_ptr_array_index(a, i),
                      (const gchar *const *)g_ptr_array_index(b, i)))
      return FALSE;
  return TRUE;
}

static GPtrArray *
tags_of_event(NostrEvent *event)
{
  GPtrArray *out = tags_new();
  NostrTags *tags = nostr_event_get_tags(event);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    size_t n = tag ? nostr_tag_size(tag) : 0;
    if (!n)
      continue;
    GStrv strv = g_new0(gchar *, n + 1);
    for (size_t j = 0; j < n; j++)
      strv[j] = g_strdup(nostr_tag_get(tag, j) ? nostr_tag_get(tag, j) : "");
    g_ptr_array_add(out, strv);
  }
  return out;
}

GPtrArray *
gh_nip29_group_list_parse_private(const gchar *plaintext, GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!plaintext || !json_parser_load_from_data(parser, plaintext, -1, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "The list's private items are not JSON");
    return NULL;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "The list's private items are not an array of tags");
    return NULL;
  }
  JsonArray *array = json_node_get_array(root);
  g_autoptr(GPtrArray) tags = tags_new();
  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonNode *item = json_array_get_element(array, i);
    if (!JSON_NODE_HOLDS_ARRAY(item) || json_array_get_length(json_node_get_array(item)) == 0) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                          "A private item of the list is not a tag");
      return NULL;
    }
    JsonArray *tag = json_node_get_array(item);
    guint n = json_array_get_length(tag);
    g_auto(GStrv) strv = g_new0(gchar *, n + 1);
    for (guint j = 0; j < n; j++) {
      JsonNode *value = json_array_get_element(tag, j);
      if (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_STRING) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "A private item of the list is not a tag");
        return NULL;
      }
      strv[j] = g_strdup(json_node_get_string(value));
    }
    g_ptr_array_add(tags, g_steal_pointer(&strv));
  }
  return g_steal_pointer(&tags);
}

gchar *
gh_nip29_group_list_format_private(GPtrArray *tags)
{
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);
  for (guint i = 0; tags && i < tags->len; i++) {
    const gchar *const *tag = g_ptr_array_index(tags, i);
    json_builder_begin_array(builder);
    for (guint j = 0; tag[j]; j++)
      json_builder_add_string_value(builder, tag[j]);
    json_builder_end_array(builder);
  }
  json_builder_end_array(builder);
  g_autoptr(JsonNode) root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) generator = json_generator_new();
  json_generator_set_root(generator, root);
  return json_generator_to_data(generator, NULL);
}

/* The room id ("<normalized relay>\x1f<id>") a ["group", id, relay, ...] tag
 * names, or NULL when it is not a valid group tag. */
static gchar *
group_tag_key(const gchar *const *tag, gchar **out_relay)
{
  if (g_strcmp0(tag[0], "group") != 0 || !tag[1] || !tag[2])
    return NULL;
  g_autoptr(GhNip29GroupKey) key = gh_nip29_group_key_new(tag[2], tag[1], NULL);
  if (!key)
    return NULL;
  if (out_relay)
    *out_relay = g_strdup(gh_nip29_group_key_get_relay_url(key));
  return g_strdup_printf("%s\x1f%s", gh_nip29_group_key_get_relay_url(key),
                         gh_nip29_group_key_get_group_id(key));
}

/* The normalized relay of an ["r", url] tag, or NULL. */
static gchar *
r_tag_relay(const gchar *const *tag)
{
  if (g_strcmp0(tag[0], "r") != 0 || !tag[1])
    return NULL;
  return gh_nip29_normalize_relay_url(tag[1], NULL);
}

/* ---- this device's view of a group ------------------------------------------------ */

typedef enum { LOCAL_UNKNOWN, LOCAL_PRESENT, LOCAL_ABSENT } LocalVerdict;

static LocalVerdict
verdict_of_state(GhNip29JoinState state)
{
  switch (state) {
  case GH_NIP29_JOIN_MEMBER:
  case GH_NIP29_JOIN_PENDING:
    return LOCAL_PRESENT;
  case GH_NIP29_JOIN_LEFT:
  case GH_NIP29_JOIN_REMOVED:
  case GH_NIP29_JOIN_DENIED:
  case GH_NIP29_JOIN_CLOSED:
    return LOCAL_ABSENT;
  default:
    return LOCAL_UNKNOWN; /* passing states, or never asked */
  }
}

/* Whether this device removed the group (left, removed, refused or
 * forgotten here) no earlier than the list it would edit: the newest list
 * wins over an older local change, never the other way round. */
static gboolean
removed_since(GhNip29GroupList *self, const gchar *room_id, gint64 list_created_at)
{
  const gint64 *at = g_hash_table_lookup(self->removals, room_id);
  return at && *at >= list_created_at;
}

/* ---- merge ------------------------------------------------------------------------- */

static void
drop_absent(GhNip29GroupList *self, GPtrArray *tags, GHashTable *removed_relays)
{
  for (guint i = tags->len; i > 0; i--) {
    const gchar *const *tag = g_ptr_array_index(tags, i - 1);
    gchar *relay = NULL;
    g_autofree gchar *key = group_tag_key(tag, &relay);
    if (key && removed_since(self, key, self->newest_created_at)) {
      g_hash_table_add(removed_relays, relay);
      g_ptr_array_remove_index(tags, i - 1);
    } else {
      g_free(relay);
    }
  }
}

static void
collect(GPtrArray *tags, GHashTable *keys, GHashTable *group_relays, GHashTable *r_relays)
{
  for (guint i = 0; i < tags->len; i++) {
    const gchar *const *tag = g_ptr_array_index(tags, i);
    gchar *relay = NULL;
    gchar *key = group_tag_key(tag, &relay);
    if (key) {
      g_hash_table_add(keys, key);
      g_hash_table_add(group_relays, relay);
    }
    gchar *r = r_tag_relay(tag);
    if (r)
      g_hash_table_add(r_relays, r);
  }
}

static void
prune_r(GPtrArray *tags, GHashTable *removed_relays, GHashTable *group_relays)
{
  for (guint i = tags->len; i > 0; i--) {
    g_autofree gchar *r = r_tag_relay(g_ptr_array_index(tags, i - 1));
    if (r && g_hash_table_contains(removed_relays, r) && !g_hash_table_contains(group_relays, r))
      g_ptr_array_remove_index(tags, i - 1);
  }
}

/* The newest list with this device's changes; TRUE when it differs. */
static gboolean
merge(GhNip29GroupList *self, GPtrArray **out_public, GPtrArray **out_private)
{
  g_autoptr(GPtrArray) pub = tags_copy(self->public_tags);
  g_autoptr(GPtrArray) priv = tags_copy(self->private_tags);
  g_autoptr(GHashTable) removed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  drop_absent(self, pub, removed);
  drop_absent(self, priv, removed);
  g_autoptr(GHashTable) keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) group_relays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) r_relays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  collect(pub, keys, group_relays, r_relays);
  collect(priv, keys, group_relays, r_relays);
  GListModel *rooms = self->service ? G_LIST_MODEL(self->service) : NULL;
  for (guint i = 0; rooms && i < g_list_model_get_n_items(rooms); i++) {
    g_autoptr(GhNip29Room) room = g_list_model_get_item(rooms, i);
    const gchar *room_id = gh_nip29_room_get_room_id(room);
    if (verdict_of_state(gh_nip29_room_get_join_state(room)) != LOCAL_PRESENT ||
        g_hash_table_contains(keys, room_id))
      continue;
    const gchar *relay = gh_nip29_room_get_relay_url(room);
    const gchar *name = gh_nip29_room_get_name(room);
    GStrv tag = g_new0(gchar *, 5);
    tag[0] = g_strdup("group");
    tag[1] = g_strdup(gh_nip29_room_get_group_id(room));
    tag[2] = g_strdup(relay);
    if (name && *name)
      tag[3] = g_strdup(name);
    g_ptr_array_add(priv, tag);
    g_hash_table_add(keys, g_strdup(room_id));
    g_hash_table_add(group_relays, g_strdup(relay));
    if (!g_hash_table_contains(r_relays, relay)) {
      GStrv r = g_new0(gchar *, 3);
      r[0] = g_strdup("r");
      r[1] = g_strdup(relay);
      g_ptr_array_add(priv, r);
      g_hash_table_add(r_relays, g_strdup(relay));
    }
  }
  prune_r(pub, removed, group_relays);
  prune_r(priv, removed, group_relays);
  gboolean changed = !tags_equal(pub, self->public_tags) || !tags_equal(priv, self->private_tags);
  *out_public = g_steal_pointer(&pub);
  *out_private = g_steal_pointer(&priv);
  return changed;
}

/* ---- state ------------------------------------------------------------------------ */

static gboolean
all_answered(GhNip29GroupList *self)
{
  if (!self->scope || g_hash_table_size(self->sources) == 0)
    return FALSE;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->sources);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (GPOINTER_TO_UINT(value) != SOURCE_EOSE)
      return FALSE;
  return TRUE;
}

static gboolean
any_pending(GhNip29GroupList *self)
{
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->sources);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (GPOINTER_TO_UINT(value) == SOURCE_PENDING)
      return TRUE;
  return FALSE;
}

static GhNip29GroupListState
compute_state(GhNip29GroupList *self)
{
  if (!self->generation)
    return GH_NIP29_GROUP_LIST_INACTIVE;
  if (!self->scope) {
    GhAccountRelaysState relays = gh_account_relays_get_state(self->relays);
    return relays == GH_ACCOUNT_RELAYS_DISCOVERING ? GH_NIP29_GROUP_LIST_WAITING
                                                   : GH_NIP29_GROUP_LIST_INACTIVE;
  }
  if (self->publishing)
    return GH_NIP29_GROUP_LIST_PUBLISHING;
  if (any_pending(self) || self->decoding)
    return GH_NIP29_GROUP_LIST_READING;
  if (self->unreadable)
    return GH_NIP29_GROUP_LIST_UNREADABLE;
  if (!all_answered(self))
    return GH_NIP29_GROUP_LIST_PARTIAL;
  return self->failed ? GH_NIP29_GROUP_LIST_FAILED : GH_NIP29_GROUP_LIST_SYNCED;
}

static void
add_restorable(GhNip29GroupList *self, GPtrArray *tags, GHashTable *seen)
{
  for (guint i = 0; tags && i < tags->len; i++) {
    const gchar *const *tag = g_ptr_array_index(tags, i);
    gchar *relay = NULL;
    g_autofree gchar *key = group_tag_key(tag, &relay);
    if (!key || g_hash_table_contains(seen, key) ||
        removed_since(self, key, self->newest_created_at) ||
        g_hash_table_contains(self->dismissed, key)) {
      g_free(relay);
      continue;
    }
    g_hash_table_add(seen, g_strdup(key));
    g_autoptr(GhNip29Room) room = self->service
      ? gh_nip29_service_lookup_room(self->service, key) : NULL;
    if (room) {
      g_free(relay);
      continue;
    }
    GhNip29ListedGroup *group = g_new0(GhNip29ListedGroup, 1);
    group->relay_url = relay;
    group->group_id = g_strdup(strchr(key, '\x1f') + 1);
    group->name = tag[3] && *tag[3] && g_utf8_validate(tag[3], -1, NULL) ? g_strdup(tag[3]) : NULL;
    g_ptr_array_add(self->restorable, group);
  }
}

static gboolean
restorable_equal(GPtrArray *a, GPtrArray *b)
{
  if (a->len != b->len)
    return FALSE;
  for (guint i = 0; i < a->len; i++) {
    const GhNip29ListedGroup *x = g_ptr_array_index(a, i), *y = g_ptr_array_index(b, i);
    if (g_strcmp0(x->relay_url, y->relay_url) || g_strcmp0(x->group_id, y->group_id) ||
        g_strcmp0(x->name, y->name))
      return FALSE;
  }
  return TRUE;
}

static void maybe_publish(GhNip29GroupList *self);
static void set_decoded(GhNip29GroupList *self, GPtrArray *pub, GPtrArray *priv);

/* Recomputes the state and the offer, emits "changed" when either moved,
 * and publishes when the merge differs and the list is known. */
static void
evaluate(GhNip29GroupList *self)
{
  if (!self->accounts)
    return;
  g_autoptr(GPtrArray) before = g_steal_pointer(&self->restorable);
  self->restorable = g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip29_listed_group_free);
  if (self->generation && self->decoded) {
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    add_restorable(self, self->public_tags, seen);
    add_restorable(self, self->private_tags, seen);
  }
  /* Only over the newest list, read in full, from every source. */
  if (self->generation && self->decoded && !self->decoding && !self->unreadable &&
      all_answered(self))
    maybe_publish(self);
  GhNip29GroupListState state = compute_state(self);
  if (state != self->state || !restorable_equal(before, self->restorable)) {
    self->state = state;
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  }
}

static gboolean
on_idle(gpointer data)
{
  GhNip29GroupList *self = data;
  self->idle = 0;
  evaluate(self);
  return G_SOURCE_REMOVE;
}

static void
schedule(GhNip29GroupList *self)
{
  if (!self->idle && self->accounts)
    self->idle = g_idle_add(on_idle, self);
}

/* ---- publish ------------------------------------------------------------------------ */

static void
publish_end(GhNip29GroupList *self)
{
  self->publishing = FALSE;
  g_clear_pointer(&self->built_on, g_free);
  g_clear_pointer(&self->pending_public, g_ptr_array_unref);
  g_clear_pointer(&self->pending_private, g_ptr_array_unref);
  g_clear_pointer(&self->pending_json, g_free);
  g_clear_pointer(&self->pending_id, g_free);
  self->pending_created_at = 0;
}

static void
adopt_newest(GhNip29GroupList *self, const gchar *json, const gchar *id, gint64 created_at)
{
  g_free(self->newest_json);
  self->newest_json = g_strdup(json);
  g_free(self->newest_id);
  self->newest_id = g_strdup(id);
  self->newest_created_at = created_at;
}

static void
on_publish_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  GhNip29GroupList *self = data;
  if (publish != self->publish || !self->publishing)
    return;
  if (summary->any_accepted) {
    /* The relays now hold this list. Unless a newer one arrived meanwhile
     * (it is being read, and the merge runs again), it is the newest, and
     * its contents are the merge it was signed from. */
    if (!self->newest_id || g_strcmp0(self->newest_id, self->pending_id) == 0 ||
        self->newest_created_at < self->pending_created_at) {
      adopt_newest(self, self->pending_json, self->pending_id, self->pending_created_at);
      self->decode_serial++;
      self->decoding = FALSE;
      set_decoded(self, g_steal_pointer(&self->pending_public),
                  g_steal_pointer(&self->pending_private));
    }
    self->failed = FALSE;
  } else {
    g_message("Groundhog's group list was not accepted by any relay");
    self->failed = TRUE;
  }
  publish_end(self);
  /* Not from inside the publish's own callback. */
  schedule(self);
}

static void
on_publish_update(GhRelayPublish *publish, const GhRelayPublishResult *result, gpointer data)
{
  (void)publish;
  (void)result;
  (void)data;
}

static gboolean
verified_list(GhNip29GroupList *self, const gchar *json, gchar **out_id, gint64 *out_created_at)
{
  NostrEvent *event = json ? nostr_event_new() : NULL;
  gboolean ok = event && nostr_event_deserialize_signed(event, json, NULL) ==
                           NOSTR_EVENT_VALIDATION_OK &&
                nostr_event_get_kind(event) == GH_NIP29_GROUP_LIST_KIND &&
                g_strcmp0(nostr_event_get_pubkey(event), self->pubkey) == 0;
  if (ok) {
    char *id = nostr_event_get_id(event);
    *out_id = g_strdup(id);
    free(id);
    *out_created_at = nostr_event_get_created_at(event);
  }
  if (event)
    nostr_event_free(event);
  return ok;
}

static void
on_signed(GObject *source, GAsyncResult *result, gpointer data)
{
  GhNip29GroupList *self = data; /* a reference held for this call */
  (void)source;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json = gh_account_controller_sign_finish(result, &error);
  if (!self->accounts || !self->publishing || self->publish) {
    g_object_unref(self);
    return;
  }
  g_autofree gchar *id = NULL;
  gint64 created_at = 0;
  if (!signed_json || !verified_list(self, signed_json, &id, &created_at) ||
      !gh_account_controller_is_current(self->accounts, self->generation)) {
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_message("Groundhog's group list was not signed: %s", error->message);
    self->failed = TRUE;
    publish_end(self);
    schedule(self);
    g_object_unref(self);
    return;
  }
  if (g_strcmp0(self->built_on, self->newest_id) != 0) {
    /* A newer list arrived while the signer was asked: merge again. */
    publish_end(self);
    schedule(self);
    g_object_unref(self);
    return;
  }
  self->publish = self->custom_publish
    ? gh_relay_publish_new_with_transport(self->generation, signed_json, &self->publish_transport,
                                          self->publish_data, on_publish_update, on_publish_done,
                                          self, &error)
    : gh_relay_publish_new(self->generation, signed_json, on_publish_update, on_publish_done,
                           self, &error);
  gboolean started = self->publish != NULL;
  if (started) {
    if (self->custom_publish && self->custom_publish_auth)
      gh_relay_publish_set_auth_transport(self->publish, &self->publish_auth);
    if (self->publish_deadline)
      gh_relay_publish_set_deadline(self->publish, self->publish_deadline);
    gh_relay_publish_set_retries(self->publish, PUBLISH_RETRIES, PUBLISH_RETRY_BASE_MS);
    /* Own list publish (charter §4.3): account AUTH, on challenge only. */
    GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(self->accounts);
    for (guint i = 0; started && self->urls[i]; i++)
      started = gh_relay_publish_add_url(self->publish, self->urls[i], &error) &&
                gh_auth_policy_apply_publish(policy, self->publish,
                                             GH_AUTH_PURPOSE_OWN_LIST_PUBLISH, self->urls[i],
                                             &error);
  }
  if (started) {
    /* Our own list comes back on the live REQ: it need not be decrypted. */
    g_free(self->published_id);
    self->published_id = g_strdup(id);
    self->pending_json = g_strdup(signed_json);
    self->pending_id = g_strdup(id);
    self->pending_created_at = created_at;
    started = gh_relay_publish_start(self->publish, &error);
  }
  if (!started) {
    g_message("Groundhog could not publish its group list: %s",
              error ? error->message : "no relay");
    g_clear_pointer(&self->publish, gh_relay_publish_unref);
    self->failed = TRUE;
    publish_end(self);
    schedule(self);
  }
  g_object_unref(self);
}

static gchar *
build_unsigned(GhNip29GroupList *self, GPtrArray *pub, const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  NostrTags *tags = nostr_tags_new(0);
  for (guint i = 0; i < pub->len; i++) {
    const gchar *const *strv = g_ptr_array_index(pub, i);
    NostrTag *tag = nostr_tag_new(strv[0], NULL);
    for (guint j = 1; strv[j]; j++)
      nostr_tag_append(tag, strv[j]);
    nostr_tags_append(tags, tag);
  }
  /* Dated after the list it replaces, so it is the newest everywhere. */
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  nostr_event_set_kind(event, GH_NIP29_GROUP_LIST_KIND);
  nostr_event_set_created_at(event, MAX(now, self->newest_created_at + 1));
  nostr_event_set_content(event, content);
  nostr_event_set_pubkey(event, self->pubkey);
  nostr_event_set_tags(event, tags);
  char *raw = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *json = raw ? g_strdup(raw) : NULL;
  free(raw);
  return json;
}

static void
sign(GhNip29GroupList *self, const gchar *content)
{
  g_autofree gchar *unsigned_json = build_unsigned(self, self->pending_public, content);
  if (!unsigned_json) {
    self->failed = TRUE;
    publish_end(self);
    return;
  }
  gh_account_controller_sign_with_cancellable_async(self->accounts, unsigned_json,
                                                    self->cancellable, on_signed,
                                                    g_object_ref(self));
}

static void
on_encrypted(GObject *source, GAsyncResult *result, gpointer data)
{
  GhNip29GroupList *self = data; /* a reference held for this call */
  (void)source;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *content = gh_account_controller_nip44_finish(result, &error);
  if (!self->accounts || !self->publishing || self->publish) {
    g_object_unref(self);
    return;
  }
  if (!content || !gh_account_controller_is_current(self->accounts, self->generation)) {
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_message("Groundhog's group list was not encrypted: %s", error->message);
    self->failed = TRUE;
    publish_end(self);
    schedule(self);
  } else {
    sign(self, content);
    if (!self->publishing)
      schedule(self);
  }
  g_object_unref(self);
}

static void
maybe_publish(GhNip29GroupList *self)
{
  /* After a refusal (signer or relays), only a change tries again. */
  if (self->publishing || self->failed || !self->urls || !self->urls[0])
    return;
  GPtrArray *pub = NULL, *priv = NULL;
  gboolean changed = merge(self, &pub, &priv);
  if (!changed) {
    g_ptr_array_unref(pub);
    g_ptr_array_unref(priv);
    return;
  }
  g_clear_pointer(&self->publish, gh_relay_publish_unref);
  self->publishing = TRUE;
  self->built_on = g_strdup(self->newest_id);
  self->pending_public = pub;
  self->pending_private = priv;
  if (priv->len == 0) {
    sign(self, "");
    return;
  }
  /* NIP-51 private items: encrypted by the account to itself. */
  g_autofree gchar *plaintext = gh_nip29_group_list_format_private(priv);
  gh_account_controller_nip44_encrypt_with_cancellable_async(self->accounts, plaintext,
                                                             self->pubkey, self->cancellable,
                                                             on_encrypted, g_object_ref(self));
}

/* ---- read ------------------------------------------------------------------------------ */

static void
set_decoded(GhNip29GroupList *self, GPtrArray *pub, GPtrArray *priv)
{
  g_clear_pointer(&self->public_tags, g_ptr_array_unref);
  g_clear_pointer(&self->private_tags, g_ptr_array_unref);
  self->public_tags = pub;
  self->private_tags = priv;
  self->unreadable = priv == NULL;
  self->decoding = FALSE;
  self->decoded = TRUE;
}

typedef struct {
  GhNip29GroupList *self;
  guint serial;
  GPtrArray *pub;
} Decode;

static void
on_decrypted(GObject *source, GAsyncResult *result, gpointer data)
{
  Decode *decode = data;
  GhNip29GroupList *self = decode->self;
  (void)source;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *plaintext = gh_account_controller_nip44_finish(result, &error);
  if (self->accounts && decode->serial == self->decode_serial) {
    GPtrArray *priv = plaintext ? gh_nip29_group_list_parse_private(plaintext, &error) : NULL;
    if (!priv && error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_message("Groundhog cannot read its group list's private items: %s", error->message);
    set_decoded(self, g_steal_pointer(&decode->pub), priv);
    evaluate(self);
  }
  if (decode->pub)
    g_ptr_array_unref(decode->pub);
  g_object_unref(self);
  g_free(decode);
}

static void
decode_newest(GhNip29GroupList *self)
{
  self->decode_serial++;
  NostrEvent *event = nostr_event_new();
  if (!event || nostr_event_deserialize_compact(event, self->newest_json, NULL) != 1) {
    if (event)
      nostr_event_free(event);
    return;
  }
  GPtrArray *pub = tags_of_event(event);
  const gchar *content = nostr_event_get_content(event);
  if (!content || !*content) {
    nostr_event_free(event);
    set_decoded(self, pub, tags_new());
    return;
  }
  self->decoding = TRUE;
  Decode *decode = g_new0(Decode, 1);
  decode->self = g_object_ref(self);
  decode->serial = self->decode_serial;
  decode->pub = pub;
  gh_account_controller_nip44_decrypt_with_cancellable_async(self->accounts, content,
                                                             self->pubkey, self->cancellable,
                                                             on_decrypted, decode);
  nostr_event_free(event);
}

static void
admit_event(GhNip29GroupList *self, const GhRelayUpdate *update)
{
  g_autofree gchar *id = NULL;
  gint64 created_at = 0;
  if (!verified_list(self, update->event_json, &id, &created_at))
    return; /* a relay can answer with anything; only the account's list counts */
  if (self->newest_id &&
      (created_at < self->newest_created_at ||
       (created_at == self->newest_created_at && g_strcmp0(id, self->newest_id) >= 0)))
    return;
  adopt_newest(self, update->event_json, id, created_at);
  if (g_strcmp0(id, self->published_id) == 0)
    return;
  self->failed = FALSE;
  decode_newest(self);
}

static void
on_scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GhNip29GroupList *self = data;
  if (scope != self->scope || !self->accounts ||
      !gh_account_controller_is_current(self->accounts, self->generation))
    return;
  guint status = GPOINTER_TO_UINT(g_hash_table_lookup(self->sources, update->url));
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    admit_event(self, update);
    schedule(self);
    return;
  case GH_RELAY_NOTICE_EOSE:
    status = SOURCE_EOSE;
    break;
  case GH_RELAY_NOTICE_ERROR:
    if (status == SOURCE_PENDING)
      status = SOURCE_FAILED;
    break;
  case GH_RELAY_NOTICE_CLOSED:
    status = SOURCE_FAILED;
    break;
  default:
    return;
  }
  if (g_hash_table_contains(self->sources, update->url)) {
    g_hash_table_insert(self->sources, g_strdup(update->url), GUINT_TO_POINTER(status));
    /* With every source answered and no list anywhere, the list is empty. */
    if (!self->newest_id && !self->decoded && all_answered(self))
      set_decoded(self, tags_new(), tags_new());
    schedule(self);
  }
}

/* The relays to read and write: discovery-relays, then the 10002 write
 * relays; normalized, distinct, at most GH_NIP29_GROUP_LIST_MAX_RELAYS. */
static GStrv
wanted_urls(GhNip29GroupList *self)
{
  g_auto(GStrv) discovery = g_settings_get_strv(self->settings, "discovery-relays");
  const gchar *const *write = gh_account_relays_get_write_relays(self->relays);
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  const gchar *const *lists[] = { (const gchar *const *)discovery, write };
  for (guint l = 0; l < G_N_ELEMENTS(lists); l++)
    for (guint i = 0; lists[l] && lists[l][i]; i++) {
      g_autofree gchar *url = gh_nip29_normalize_relay_url(lists[l][i], NULL);
      if (!url || !gh_relay_url_validate(url, NULL) ||
          g_ptr_array_find_with_equal_func(urls, url, g_str_equal, NULL) ||
          urls->len >= GH_NIP29_GROUP_LIST_MAX_RELAYS)
        continue;
      g_ptr_array_add(urls, g_steal_pointer(&url));
    }
  g_ptr_array_add(urls, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&urls), FALSE);
}

static NostrFilters *
list_filters(const gchar *pubkey)
{
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  const int kinds[] = { GH_NIP29_GROUP_LIST_KIND };
  const char *const authors[] = { pubkey };
  nostr_filter_set_kinds(filter, kinds, G_N_ELEMENTS(kinds));
  nostr_filter_set_authors(filter, authors, G_N_ELEMENTS(authors));
  gboolean added = nostr_filters_add(filters, filter);
  nostr_filter_free(filter); /* contents moved into the vector */
  if (!added) {
    nostr_filters_free(filters);
    return NULL;
  }
  return filters;
}

static void
stop_scope(GhNip29GroupList *self)
{
  if (self->scope) {
    gh_relay_scope_cancel(self->scope);
    g_clear_pointer(&self->scope, gh_relay_scope_unref);
  }
  g_hash_table_remove_all(self->sources);
  g_clear_pointer(&self->urls, g_strfreev);
}

static void
start_scope(GhNip29GroupList *self, GStrv urls)
{
  NostrFilters *filters = list_filters(self->pubkey);
  if (!filters)
    return;
  self->scope = self->custom_scope
    ? gh_relay_scope_new_with_transport(self->generation, filters, &self->scope_transport,
                                        self->scope_data, on_scope_update, self)
    : gh_relay_scope_new(self->generation, filters, on_scope_update, self);
  if (self->custom_scope && self->custom_scope_auth)
    gh_relay_scope_set_auth_transport(self->scope, &self->scope_auth);
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(self->accounts);
  g_autoptr(GPtrArray) added = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; urls[i]; i++) {
    if (!gh_relay_scope_add_url(self->scope, urls[i], NULL))
      continue;
    /* Own list discovery (charter §4.3): a throwaway key, and only if the
     * relay demands AUTH for the REQ; never the account. */
    gh_auth_policy_apply_scope(policy, self->scope, GH_AUTH_PURPOSE_OWN_LIST_DISCOVERY, urls[i],
                               NULL);
    g_hash_table_insert(self->sources, g_strdup(urls[i]), GUINT_TO_POINTER(SOURCE_PENDING));
    g_ptr_array_add(added, g_strdup(urls[i]));
  }
  if (!added->len) {
    stop_scope(self);
    return;
  }
  g_ptr_array_add(added, NULL);
  self->urls = (GStrv)g_ptr_array_free(g_steal_pointer(&added), FALSE);
  gh_relay_scope_start(self->scope);
}

/* ---- binding -------------------------------------------------------------------------- */

static void
unbind(GhNip29GroupList *self)
{
  stop_scope(self);
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }
  if (self->publish) {
    gh_relay_publish_cancel(self->publish);
    g_clear_pointer(&self->publish, gh_relay_publish_unref);
  }
  publish_end(self);
  self->failed = FALSE;
  self->decode_serial++;
  self->decoding = FALSE;
  self->decoded = FALSE;
  self->unreadable = FALSE;
  g_clear_pointer(&self->public_tags, g_ptr_array_unref);
  g_clear_pointer(&self->private_tags, g_ptr_array_unref);
  g_clear_pointer(&self->newest_json, g_free);
  g_clear_pointer(&self->newest_id, g_free);
  g_clear_pointer(&self->published_id, g_free);
  self->newest_created_at = 0;
  g_hash_table_remove_all(self->removals);
  g_clear_pointer(&self->pubkey, g_free);
  self->generation = 0;
}

/* The account, own-list discovery or the relay settings changed. */
static void
rebind(GhNip29GroupList *self)
{
  if (!self->accounts)
    return;
  guint64 generation = 0;
  const gchar *service_account = self->service ? gh_nip29_service_get_account(self->service)
                                               : NULL;
  g_autofree gchar *pubkey = NULL;
  if (gh_account_controller_get_state(self->accounts) == GH_ACCOUNT_STATE_ACTIVE) {
    pubkey = gh_identity_pubkey_hex(gh_account_controller_get_active_npub(self->accounts));
    if (pubkey && g_strcmp0(pubkey, service_account) == 0)
      generation = gh_account_controller_get_generation(self->accounts);
  }
  if (generation != self->generation) {
    unbind(self);
    self->generation = generation;
    if (generation) {
      self->pubkey = g_steal_pointer(&pubkey);
      self->cancellable = g_cancellable_new();
    }
  }
  /* Own-list discovery first: the write relays are where the list lives. */
  if (self->generation &&
      gh_account_relays_get_generation(self->relays) == self->generation &&
      gh_account_relays_get_state(self->relays) != GH_ACCOUNT_RELAYS_DISCOVERING) {
    g_auto(GStrv) urls = wanted_urls(self);
    if (!self->urls || !g_strv_equal((const gchar *const *)urls,
                                     (const gchar *const *)self->urls)) {
      /* A new relay set: ask it again; the newest list seen stays known. */
      stop_scope(self);
      self->failed = FALSE;
      if (urls[0])
        start_scope(self, urls);
    }
  }
  schedule(self);
}

/* This device's changes, as they happen: a group that becomes LEFT,
 * REMOVED, DENIED or CLOSED, or that is forgotten, is removed from the list
 * at that time; joining it again cancels that. */
static void
note_room(GhNip29GroupList *self, GhNip29Room *room)
{
  const gchar *room_id = gh_nip29_room_get_room_id(room);
  LocalVerdict now = verdict_of_state(gh_nip29_room_get_join_state(room));
  gpointer old = NULL;
  gboolean known = g_hash_table_lookup_extended(self->rooms, room_id, NULL, &old);
  if (now == LOCAL_PRESENT)
    g_hash_table_remove(self->removals, room_id);
  else if (now == LOCAL_ABSENT && known && GPOINTER_TO_INT(old) - 1 != LOCAL_ABSENT &&
           self->generation) {
    gint64 *at = g_new(gint64, 1);
    *at = g_get_real_time() / G_USEC_PER_SEC;
    g_hash_table_insert(self->removals, g_strdup(room_id), at);
  }
  g_hash_table_insert(self->rooms, g_strdup(room_id), GINT_TO_POINTER(now + 1));
}

static void
on_room_state(GhNip29GroupList *self, GParamSpec *pspec, GhNip29Room *room)
{
  (void)pspec;
  note_room(self, room);
  self->failed = FALSE;
  schedule(self);
}

static void
watch_rooms(GhNip29GroupList *self)
{
  if (!self->service)
    return;
  GListModel *rooms = G_LIST_MODEL(self->service);
  g_autoptr(GHashTable) now = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint i = 0; i < g_list_model_get_n_items(rooms); i++) {
    g_autoptr(GhNip29Room) room = g_list_model_get_item(rooms, i);
    const gchar *room_id = gh_nip29_room_get_room_id(room);
    g_hash_table_add(now, g_strdup(room_id));
    if (!g_hash_table_contains(self->rooms, room_id))
      g_signal_connect_object(room, "notify::join-state", G_CALLBACK(on_room_state), self,
                              G_CONNECT_SWAPPED);
    note_room(self, room);
  }
  /* A room that went from the service was forgotten (or its creation
   * refused): this device no longer has the group. */
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, self->rooms);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    if (!g_hash_table_contains(now, key)) {
      if (self->generation) {
        gint64 *at = g_new(gint64, 1);
        *at = g_get_real_time() / G_USEC_PER_SEC;
        g_hash_table_insert(self->removals, g_strdup(key), at);
      }
      g_hash_table_iter_remove(&iter);
    }
  self->failed = FALSE;
  schedule(self);
}

static void
on_service_gone(gpointer data, GObject *where)
{
  GhNip29GroupList *self = data;
  (void)where;
  self->service = NULL;
  unbind(self);
  schedule(self);
}

GhNip29GroupList *
gh_nip29_group_list_new(const GhNip29GroupListConfig *config)
{
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(config->accounts), NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_RELAYS(config->account_relays), NULL);
  g_return_val_if_fail(G_IS_SETTINGS(config->settings), NULL);
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(config->service), NULL);
  GhNip29GroupList *self = g_object_new(GH_TYPE_NIP29_GROUP_LIST, NULL);
  self->accounts = g_object_ref(config->accounts);
  self->relays = g_object_ref(config->account_relays);
  self->settings = g_object_ref(config->settings);
  self->service = config->service;
  g_object_weak_ref(G_OBJECT(self->service), on_service_gone, self);
  if (config->scope_transport) {
    self->scope_transport = *config->scope_transport;
    self->scope_data = config->scope_transport_data;
    self->custom_scope = TRUE;
    if (config->scope_auth_transport) {
      self->scope_auth = *config->scope_auth_transport;
      self->custom_scope_auth = TRUE;
    }
  }
  if (config->publish_transport) {
    self->publish_transport = *config->publish_transport;
    self->publish_data = config->publish_transport_data;
    self->custom_publish = TRUE;
    if (config->publish_auth_transport) {
      self->publish_auth = *config->publish_auth_transport;
      self->custom_publish_auth = TRUE;
    }
  }
  self->publish_deadline = config->publish_deadline;
  g_signal_connect_object(self->accounts, "changed", G_CALLBACK(rebind), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->relays, "changed", G_CALLBACK(rebind), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(self->settings, "changed::discovery-relays", G_CALLBACK(rebind), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->service, "items-changed", G_CALLBACK(watch_rooms), self,
                          G_CONNECT_SWAPPED);
  watch_rooms(self);
  rebind(self);
  return self;
}

GhNip29GroupListState
gh_nip29_group_list_get_state(GhNip29GroupList *self)
{
  g_return_val_if_fail(GH_IS_NIP29_GROUP_LIST(self), GH_NIP29_GROUP_LIST_INACTIVE);
  return self->state;
}

GPtrArray *
gh_nip29_group_list_dup_restorable(GhNip29GroupList *self)
{
  g_return_val_if_fail(GH_IS_NIP29_GROUP_LIST(self), NULL);
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip29_listed_group_free);
  for (guint i = 0; self->restorable && i < self->restorable->len; i++) {
    const GhNip29ListedGroup *group = g_ptr_array_index(self->restorable, i);
    GhNip29ListedGroup *copy = g_new0(GhNip29ListedGroup, 1);
    copy->relay_url = g_strdup(group->relay_url);
    copy->group_id = g_strdup(group->group_id);
    copy->name = g_strdup(group->name);
    g_ptr_array_add(out, copy);
  }
  return out;
}

guint
gh_nip29_group_list_restore(GhNip29GroupList *self, GPtrArray *groups, GError **error)
{
  g_return_val_if_fail(GH_IS_NIP29_GROUP_LIST(self), 0);
  g_return_val_if_fail(groups != NULL, 0);
  if (!self->service) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "No message storage is open");
    return 0;
  }
  guint started = 0;
  for (guint i = 0; i < groups->len; i++) {
    const GhNip29ListedGroup *group = g_ptr_array_index(groups, i);
    g_autoptr(GError) local = NULL;
    g_autoptr(GhNip29Room) room = gh_nip29_service_join(self->service, group->relay_url,
                                                        group->group_id, NULL, NULL, &local);
    if (room)
      started++;
    else if (error && !*error)
      *error = g_steal_pointer(&local);
  }
  evaluate(self);
  return started;
}

void
gh_nip29_group_list_dismiss(GhNip29GroupList *self)
{
  g_return_if_fail(GH_IS_NIP29_GROUP_LIST(self));
  for (guint i = 0; self->restorable && i < self->restorable->len; i++) {
    const GhNip29ListedGroup *group = g_ptr_array_index(self->restorable, i);
    g_hash_table_add(self->dismissed,
                     g_strdup_printf("%s\x1f%s", group->relay_url, group->group_id));
  }
  evaluate(self);
}

const gchar *
gh_nip29_group_list_get_event_json(GhNip29GroupList *self)
{
  g_return_val_if_fail(GH_IS_NIP29_GROUP_LIST(self), NULL);
  return self->newest_json;
}

static void
gh_nip29_group_list_dispose(GObject *object)
{
  GhNip29GroupList *self = GH_NIP29_GROUP_LIST(object);
  if (self->accounts) {
    g_clear_handle_id(&self->idle, g_source_remove);
    if (self->service) {
      g_signal_handlers_disconnect_by_data(self->service, self);
      g_object_weak_unref(G_OBJECT(self->service), on_service_gone, self);
      GListModel *rooms = G_LIST_MODEL(self->service);
      for (guint i = 0; i < g_list_model_get_n_items(rooms); i++) {
        g_autoptr(GhNip29Room) room = g_list_model_get_item(rooms, i);
        g_signal_handlers_disconnect_by_data(room, self);
      }
      self->service = NULL;
    }
    g_signal_handlers_disconnect_by_data(self->accounts, self);
    g_signal_handlers_disconnect_by_data(self->relays, self);
    g_signal_handlers_disconnect_by_data(self->settings, self);
    unbind(self);
    g_clear_object(&self->accounts);
    g_clear_object(&self->relays);
    g_clear_object(&self->settings);
  }
  G_OBJECT_CLASS(gh_nip29_group_list_parent_class)->dispose(object);
}

static void
gh_nip29_group_list_finalize(GObject *object)
{
  GhNip29GroupList *self = GH_NIP29_GROUP_LIST(object);
  g_hash_table_unref(self->sources);
  g_hash_table_unref(self->rooms);
  g_hash_table_unref(self->removals);
  g_hash_table_unref(self->dismissed);
  g_clear_pointer(&self->restorable, g_ptr_array_unref);
  G_OBJECT_CLASS(gh_nip29_group_list_parent_class)->finalize(object);
}

static void
gh_nip29_group_list_class_init(GhNip29GroupListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_nip29_group_list_dispose;
  object_class->finalize = gh_nip29_group_list_finalize;
  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gh_nip29_group_list_init(GhNip29GroupList *self)
{
  self->sources = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->rooms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->removals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->dismissed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->restorable = g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip29_listed_group_free);
  self->state = GH_NIP29_GROUP_LIST_INACTIVE;
}
