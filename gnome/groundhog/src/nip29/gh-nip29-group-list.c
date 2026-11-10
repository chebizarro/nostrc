#include "gh-nip29-group-list.h"
#include "gh-identity.h"
#include "gh-nip29-group.h"
#include <string.h>
#include <jansson.h>

struct _GhNip29GroupList {
  GObject parent_instance;
  GhAccountController *accounts;
  GhAccountRelays *relays;
  GSettings *settings;
  GhNip29Service *service; /* weak */
  GhSocialLists *social;
  GNostrListService *lists;
  GhListAdapter *adapter; /* owned by lists */
  GhListAdapterNetwork network;
  gboolean owned_lists;
  guint64 generation;
  GStrv urls;
  gboolean waiting;
  GHashTable *rooms, *removals, *dismissed;
  GPtrArray *restorable, *public_tags, *private_tags;
  gchar *newest_json;
  gint64 newest_created_at;
  guint idle;
  GhNip29GroupListState state;
};
enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];
G_DEFINE_FINAL_TYPE(GhNip29GroupList, gh_nip29_group_list, G_TYPE_OBJECT)
static void schedule(GhNip29GroupList *self);
static void evaluate(GhNip29GroupList *self);
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
tags_from_codec(const NostrListDocument *codec, gboolean priv)
{
  GPtrArray *tags = tags_new();
  for (gsize i = 0; i < nostr_nip51_document_tag_count(codec, priv); i++) {
    const NostrListTag *tag = nostr_nip51_document_tag(codec, priv, i);
    GStrv fields = g_new0(gchar *, nostr_nip51_tag_size(tag) + 1);
    for (gsize j = 0; j < nostr_nip51_tag_size(tag); j++) fields[j] = g_strdup(nostr_nip51_tag_field(tag, j));
    g_ptr_array_add(tags, fields);
  }
  return tags;
}
GPtrArray *
gh_nip29_group_list_parse_private(const gchar *plaintext, GError **error)
{
  NostrListDocument *codec = nostr_nip51_document_new(GH_NIP29_GROUP_LIST_KIND, NULL);
  if (!plaintext || nostr_nip51_document_parse_private_tags(codec, plaintext, strlen(plaintext))) {
    nostr_nip51_document_free(codec);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid private group list");
    return NULL;
  }
  for (gsize i = 0; i < nostr_nip51_document_tag_count(codec, TRUE); i++)
    if (!nostr_nip51_tag_size(nostr_nip51_document_tag(codec, TRUE, i))) {
      nostr_nip51_document_free(codec);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid empty private group tag");
      return NULL;
    }
  GPtrArray *tags = tags_from_codec(codec, TRUE);
  nostr_nip51_document_free(codec);
  return tags;
}
gchar *
gh_nip29_group_list_format_private(GPtrArray *tags)
{
  NostrListDocument *codec = nostr_nip51_document_new(GH_NIP29_GROUP_LIST_KIND, NULL);
  for (guint i = 0; tags && i < tags->len; i++) {
    const char *const *fields = tags->pdata[i];
    NostrListTag *tag = nostr_nip51_tag_new(fields, g_strv_length((gchar **)fields));
    int rc = nostr_nip51_document_append(codec, TRUE, tag);
    nostr_nip51_tag_free(tag);
    if (rc) { nostr_nip51_document_free(codec); return NULL; }
  }
  char *raw = nostr_nip51_document_serialize_private_tags(codec);
  gchar *result = g_strdup(raw);
  free(raw);
  nostr_nip51_document_free(codec);
  return result;
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

static void
project(GhNip29GroupList *self, GNostrListDocument *snapshot)
{
  g_clear_pointer(&self->public_tags, g_ptr_array_unref);
  g_clear_pointer(&self->private_tags, g_ptr_array_unref);
  const NostrListDocument *codec = gnostr_list_document_get_codec(snapshot);
  self->public_tags = tags_from_codec(codec, FALSE);
  self->private_tags = tags_from_codec(codec, TRUE);
  self->newest_created_at = gnostr_list_document_get_created_at(snapshot);
  g_free(self->newest_json);
  self->newest_json = g_strdup(gnostr_list_document_get_source_json(snapshot));
}
static void
add_delta(GPtrArray *ops, GPtrArray *before, GPtrArray *after, gboolean priv)
{
  for (guint i = 0; i < before->len; i++) {
    const char *const *fields = before->pdata[i];
    gboolean found = FALSE;
    for (guint j = 0; j < after->len; j++)
      if (g_strv_equal(fields, after->pdata[j])) { found = TRUE; break; }
    if (!found) g_ptr_array_add(ops, gnostr_list_operation_new(GNOSTR_LIST_EDIT_REMOVE, priv,
      fields, g_strv_length((gchar **)fields), G_MAXUINT, 0));
  }
  for (guint i = 0; i < after->len; i++) {
    const char *const *fields = after->pdata[i];
    gboolean found = FALSE;
    for (guint j = 0; j < before->len; j++)
      if (g_strv_equal(fields, before->pdata[j])) { found = TRUE; break; }
    if (!found) g_ptr_array_add(ops, gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, priv,
      fields, g_strv_length((gchar **)fields), 0, 0));
  }
}
static GPtrArray *
derive_operations(gpointer data, GNostrListDocument *snapshot, GError **error)
{
  (void)error;
  GhNip29GroupList *self = data;
  if (gnostr_list_document_get_kind(snapshot) != GH_NIP29_GROUP_LIST_KIND) return NULL;
  project(self, snapshot);
  g_autoptr(GPtrArray) pub = NULL, priv = NULL;
  GPtrArray *ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  if (merge(self, &pub, &priv)) {
    add_delta(ops, self->public_tags, pub, FALSE);
    add_delta(ops, self->private_tags, priv, TRUE);
  }
  return ops;
}
static GhNip29GroupListState
compute_state(GhNip29GroupList *self)
{
  if (!self->generation || !self->lists) return GH_NIP29_GROUP_LIST_INACTIVE;
  switch (gnostr_list_service_get_state(self->lists, GH_NIP29_GROUP_LIST_KIND, "")) {
  case GNOSTR_LIST_WAITING_FOR_DEPENDENCY: return GH_NIP29_GROUP_LIST_WAITING;
  case GNOSTR_LIST_READING: return GH_NIP29_GROUP_LIST_READING;
  case GNOSTR_LIST_PARTIAL: {
    g_autoptr(GNostrListDocument) snapshot = gnostr_list_service_lookup(self->lists, GH_NIP29_GROUP_LIST_KIND, "");
    json_t *sources = json_loads(gnostr_list_document_get_source_outcomes(snapshot), 0, NULL);
    guint answered = json_object_size(sources);
    json_decref(sources);
    return self->urls && answered < g_strv_length(self->urls) ? GH_NIP29_GROUP_LIST_READING : GH_NIP29_GROUP_LIST_PARTIAL;
  }
  case GNOSTR_LIST_UNREADABLE: return GH_NIP29_GROUP_LIST_UNREADABLE;
  case GNOSTR_LIST_PUBLISHING: return GH_NIP29_GROUP_LIST_PUBLISHING;
  case GNOSTR_LIST_FAILED: case GNOSTR_LIST_WAITING_FOR_SIGNER: return GH_NIP29_GROUP_LIST_FAILED;
  case GNOSTR_LIST_INACTIVE: return GH_NIP29_GROUP_LIST_INACTIVE;
  default: return self->urls && self->urls[0] ? GH_NIP29_GROUP_LIST_SYNCED : GH_NIP29_GROUP_LIST_INACTIVE;
  }
}
static void
evaluate(GhNip29GroupList *self)
{
  if (!self->accounts) return;
  g_autoptr(GPtrArray) before = g_steal_pointer(&self->restorable);
  self->restorable = g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip29_listed_group_free);
  if (self->lists) {
    g_autoptr(GNostrListDocument) snapshot = gnostr_list_service_lookup(self->lists, GH_NIP29_GROUP_LIST_KIND, "");
    if (snapshot) {
      project(self, snapshot);
      g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      add_restorable(self, self->public_tags, seen);
      add_restorable(self, self->private_tags, seen);
    }
  }
  GhNip29GroupListState state = compute_state(self);
  if (state != self->state || !restorable_equal(before, self->restorable)) {
    self->state = state;
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  }
}
static gboolean on_idle(gpointer data)
{
  GhNip29GroupList *self = data;
  self->idle = 0;
  evaluate(self);
  return G_SOURCE_REMOVE;
}
static void schedule(GhNip29GroupList *self)
{
  if (!self->idle && self->accounts) self->idle = g_idle_add(on_idle, self);
}
static void
unbind(GhNip29GroupList *self)
{
  if (self->lists) {
    g_signal_handlers_disconnect_by_data(self->lists, self);
    gh_list_adapter_set_derive(self->adapter, NULL, NULL);
    if (self->owned_lists) gnostr_list_service_close(self->lists);
    else gh_social_lists_configure_groups(self->social, NULL, TRUE, NULL, NULL);
    g_clear_object(&self->lists);
  }
  self->adapter = NULL;
  self->generation = 0;
  g_clear_pointer(&self->urls, g_strfreev);
  g_clear_pointer(&self->newest_json, g_free);
  self->newest_created_at = 0;
  g_clear_pointer(&self->public_tags, g_ptr_array_unref);
  g_clear_pointer(&self->private_tags, g_ptr_array_unref);
  g_hash_table_remove_all(self->removals);
  g_hash_table_remove_all(self->dismissed);
}
static void
rebind(GhNip29GroupList *self)
{
  if (!self->accounts) return;
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(gh_account_controller_get_active_npub(self->accounts));
  guint64 generation = gh_account_controller_get_state(self->accounts) == GH_ACCOUNT_STATE_ACTIVE &&
    self->service && !g_strcmp0(pubkey, gh_nip29_service_get_account(self->service))
    ? gh_account_controller_get_generation(self->accounts) : 0;
  if (self->social && gh_social_lists_get_generation(self->social) != generation) generation = 0;
  if (self->generation != generation) {
    unbind(self);
    self->generation = generation;
    if (generation) {
      self->owned_lists = self->social == NULL;
      if (self->social) {
        self->lists = g_object_ref(gh_social_lists_get_service(self->social));
        self->adapter = gh_social_lists_get_adapter(self->social);
      } else {
        self->adapter = gh_list_adapter_new_volatile(pubkey, generation);
        gh_list_adapter_set_network(self->adapter, &self->network);
        self->lists = gnostr_list_service_new(pubkey, generation, gh_list_adapter_get_callbacks(),
          self->adapter, (GDestroyNotify)gh_list_adapter_free);
      }
      gh_list_adapter_set_derive(self->adapter, derive_operations, self);
      g_signal_connect_swapped(self->lists, "state-changed", G_CALLBACK(schedule), self);
    }
  }
  if (generation) {
    gboolean waiting = gh_account_relays_get_generation(self->relays) != generation ||
      gh_account_relays_get_state(self->relays) == GH_ACCOUNT_RELAYS_DISCOVERING;
    g_auto(GStrv) urls = wanted_urls(self);
    if (!self->urls || self->waiting != waiting || !g_strv_equal((const gchar *const *)urls, (const gchar *const *)self->urls)) {
      g_clear_pointer(&self->urls, g_strfreev);
      self->urls = g_strdupv(urls); self->waiting = waiting;
      if (self->social) gh_social_lists_configure_groups(self->social, (const gchar *const *)urls, waiting, derive_operations, self);
      else {
        GNostrListReadPlanEntry plan = { .kind = GH_NIP29_GROUP_LIST_KIND, .identifier = "",
          .sources = (const gchar *const *)urls, .targets = (const gchar *const *)urls,
          .read_purpose = "own-list-discovery", .publish_purpose = "own-list-publish",
          .publish_policy = GNOSTR_LIST_AUTO_ALWAYS, .required_sources = GNOSTR_LIST_SOURCES_ALL,
          .waiting_for_dependency = waiting };
        gnostr_list_service_update_read_plan(self->lists, &plan, 1, NULL);
      }
    }
  }
  schedule(self);
}
static void
room_changes(GhNip29GroupList *self)
{
  if (self->lists) {
    g_autoptr(GNostrListDocument) snapshot = gnostr_list_service_lookup(self->lists, GH_NIP29_GROUP_LIST_KIND, "");
    if (snapshot) {
      g_autoptr(GPtrArray) ops = derive_operations(self, snapshot, NULL);
      if (ops && ops->len) {
        g_autoptr(GNostrListDocument) saved = gnostr_list_service_apply(self->lists,
          GH_NIP29_GROUP_LIST_KIND, "", gnostr_list_document_get_local_revision(snapshot), ops, NULL);
      }
    }
    gnostr_list_service_retry(self->lists, GH_NIP29_GROUP_LIST_KIND, "");
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
  room_changes(self);
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
  room_changes(self);
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
  g_return_val_if_fail(config && GH_IS_ACCOUNT_CONTROLLER(config->accounts) &&
    GH_IS_ACCOUNT_RELAYS(config->account_relays) && G_IS_SETTINGS(config->settings) &&
    GH_IS_NIP29_SERVICE(config->service), NULL);
  GhNip29GroupList *self = g_object_new(GH_TYPE_NIP29_GROUP_LIST, NULL);
  self->accounts = g_object_ref(config->accounts);
  self->relays = g_object_ref(config->account_relays);
  self->settings = g_object_ref(config->settings);
  self->social = config->social_lists ? g_object_ref(config->social_lists) : NULL;
  GhListAdapterNetwork network = { .accounts = self->accounts,
    .scope_transport = config->scope_transport, .scope_auth_transport = config->scope_auth_transport,
    .scope_data = config->scope_transport_data, .publish_transport = config->publish_transport,
    .publish_auth_transport = config->publish_auth_transport, .publish_data = config->publish_transport_data,
    .publish_deadline = config->publish_deadline };
  gh_list_adapter_network_copy(&self->network, &network);
  self->service = config->service;
  g_object_weak_ref(G_OBJECT(self->service), on_service_gone, self);
  g_signal_connect_object(self->accounts, "changed", G_CALLBACK(rebind), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(self->relays, "changed", G_CALLBACK(rebind), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(self->settings, "changed::discovery-relays", G_CALLBACK(rebind), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(self->service, "items-changed", G_CALLBACK(watch_rooms), self, G_CONNECT_SWAPPED);
  if (self->social) g_signal_connect_object(self->social, "changed", G_CALLBACK(rebind), self, G_CONNECT_SWAPPED);
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
      for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(self->service)); i++) {
        g_autoptr(GhNip29Room) room = g_list_model_get_item(G_LIST_MODEL(self->service), i);
        g_signal_handlers_disconnect_by_data(room, self);
      }
      self->service = NULL;
    }
    if (self->social) g_signal_handlers_disconnect_by_data(self->social, self);
    g_signal_handlers_disconnect_by_data(self->accounts, self);
    g_signal_handlers_disconnect_by_data(self->relays, self);
    g_signal_handlers_disconnect_by_data(self->settings, self);
    unbind(self);
    g_clear_object(&self->social);
    gh_list_adapter_network_clear(&self->network);
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
  g_hash_table_unref(self->rooms);
  g_hash_table_unref(self->removals);
  g_hash_table_unref(self->dismissed);
  g_ptr_array_unref(self->restorable);
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
  self->rooms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->removals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->dismissed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->restorable = g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip29_listed_group_free);
}
