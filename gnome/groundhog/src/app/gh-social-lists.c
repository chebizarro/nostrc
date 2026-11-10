#include "gh-social-lists.h"
#include "gh-list-adapter.h"
#include <jansson.h>
#include <string.h>
#include <nostr-kinds.h>

struct _GhSocialLists {
  GObject parent_instance;
  GhAccountStore *account_store;
  GhStore *store;
  GhListAdapter *adapter; /* owned by service; revoked before service release */
  GNostrListService *service;
  guint64 generation;
  gboolean sync_enabled;
  gchar *error;
};
G_DEFINE_TYPE(GhSocialLists, gh_social_lists, G_TYPE_OBJECT)
enum { CHANGED, DOCUMENT_CHANGED, SYNC_STATE_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

void
gh_social_lists_close(GhSocialLists *self)
{
  if (self->adapter)
    gh_list_adapter_revoke(self->adapter);
  if (self->service) {
    g_signal_handlers_disconnect_by_data(self->service, self);
    gnostr_list_service_close(self->service);
    g_clear_object(&self->service);
  }
  self->adapter = NULL;
  self->store = NULL;
  self->generation = 0;
  self->sync_enabled = FALSE;
  g_clear_pointer(&self->error, g_free);
  g_signal_emit(self, signals[CHANGED], 0);
  g_signal_emit(self, signals[SYNC_STATE_CHANGED], 0);
}

static void
document_changed(GNostrListService *service, GNostrListDocument *snapshot, GhSocialLists *self)
{
  (void)service;
  g_signal_emit(self, signals[DOCUMENT_CHANGED], 0, snapshot);
  g_signal_emit(self, signals[CHANGED], 0);
}

static GNostrListReadPlanEntry
local_plan(gint kind, const gchar *identifier)
{
  const NostrListSchema *schema = nostr_nip51_schema_lookup(kind);
  return (GNostrListReadPlanEntry){
    .kind = kind, .identifier = identifier,
    .publish_policy = nostr_nip51_schema_read_only(schema) ? GNOSTR_LIST_READ_ONLY : GNOSTR_LIST_EXPLICIT_ONLY,
    .required_sources = kind == NOSTR_KIND_SIMPLE_GROUP_LIST ? GNOSTR_LIST_SOURCES_ALL : GNOSTR_LIST_SOURCES_MAJORITY,
  };
}

gboolean
gh_social_lists_bind_store(GhSocialLists *self, GhStore *store, guint64 generation, GError **error)
{
  gh_social_lists_close(self);
  g_autoptr(GPtrArray) identities = gh_store_social_list(store, error);
  GhStoreSocialPreferences preferences;
  if (!identities || !gh_store_social_get_preferences(store, &preferences, error))
    goto fail;
  if (identities->len > 256) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Too many account list documents");
    goto fail;
  }
  self->store = store;
  self->generation = generation;
  self->sync_enabled = preferences.sync_enabled;
  self->adapter = gh_list_adapter_new(store, generation);
  self->service = gnostr_list_service_new(gh_store_get_account_pubkey(store), generation,
    gh_list_adapter_get_callbacks(), self->adapter, (GDestroyNotify)gh_list_adapter_free);
  GNostrListReadPlanEntry entries[256];
  for (guint i = 0; i < identities->len; i++) {
    GhStoreSocialIdentity *id = identities->pdata[i];
    entries[i] = local_plan(id->kind, id->identifier);
  }
  if (!gnostr_list_service_start(self->service, entries, identities->len, error)) {
    gh_social_lists_close(self);
    goto fail;
  }
  g_signal_connect(self->service, "document-changed", G_CALLBACK(document_changed), self);
  g_signal_emit(self, signals[CHANGED], 0);
  g_signal_emit(self, signals[SYNC_STATE_CHANGED], 0);
  return TRUE;
fail:
  self->error = g_strdup("Account lists could not be loaded");
  g_signal_emit(self, signals[CHANGED], 0);
  return FALSE;
}

static void
store_bound(GhAccountStore *store, GhSocialLists *self)
{
  g_autoptr(GError) error = NULL;
  gh_social_lists_bind_store(self, gh_account_store_get_store(store),
                             gh_account_store_get_generation(store), &error);
}

static void
store_closing(GhAccountStore *store, GhSocialLists *self)
{
  (void)store;
  gh_social_lists_close(self);
}

static void
gh_social_lists_dispose(GObject *object)
{
  GhSocialLists *self = GH_SOCIAL_LISTS(object);
  if (self->account_store)
    g_signal_handlers_disconnect_by_data(self->account_store, self);
  gh_social_lists_close(self);
  g_clear_object(&self->account_store);
  G_OBJECT_CLASS(gh_social_lists_parent_class)->dispose(object);
}

static void
gh_social_lists_class_init(GhSocialListsClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_social_lists_dispose;
  signals[CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[DOCUMENT_CHANGED] = g_signal_new("document-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 1, GNOSTR_TYPE_LIST_DOCUMENT);
  signals[SYNC_STATE_CHANGED] = g_signal_new("sync-state-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void gh_social_lists_init(GhSocialLists *self) { (void)self; }

GhSocialLists *
gh_social_lists_new(GhAccountStore *account_store)
{
  GhSocialLists *self = g_object_new(GH_TYPE_SOCIAL_LISTS, NULL);
  if (account_store) {
    self->account_store = g_object_ref(account_store);
    g_signal_connect(account_store, "store-bound", G_CALLBACK(store_bound), self);
    g_signal_connect(account_store, "store-closing", G_CALLBACK(store_closing), self);
    if (gh_account_store_get_store(account_store))
      store_bound(account_store, self);
  }
  return self;
}

GNostrListService *gh_social_lists_get_service(GhSocialLists *self) { return self->service; }
guint64 gh_social_lists_get_generation(GhSocialLists *self) { return self->generation; }
const gchar *gh_social_lists_get_error(GhSocialLists *self) { return self->error; }
gboolean gh_social_lists_get_sync_enabled(GhSocialLists *self) { return self->sync_enabled; }

gboolean
gh_social_lists_set_sync_enabled(GhSocialLists *self, gboolean enabled, GError **error)
{
  if (enabled) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Synchronization is not available in this version yet");
    return FALSE;
  }
  if (!self->store) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account storage is not open");
    return FALSE;
  }
  if (!gh_store_social_set_sync_enabled(self->store, FALSE, error))
    return FALSE;
  self->sync_enabled = FALSE;
  gnostr_list_service_set_private_sync_enabled(self->service, FALSE);
  g_signal_emit(self, signals[SYNC_STATE_CHANGED], 0);
  return TRUE;
}

GNostrListDocument *
gh_social_lists_ensure(GhSocialLists *self, gint kind, const gchar *identifier, GError **error)
{
  if (!self->service) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account list storage is not open");
    return NULL;
  }
  GNostrListDocument *existing = gnostr_list_service_lookup(self->service, kind, identifier);
  if (existing)
    return existing;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->service));
  if (n >= 256) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Too many account list documents");
    return NULL;
  }
  GNostrListReadPlanEntry entries[256];
  GPtrArray *snapshots = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < n; i++) {
    GNostrListDocument *snapshot = g_list_model_get_item(G_LIST_MODEL(self->service), i);
    g_ptr_array_add(snapshots, snapshot);
    entries[i] = local_plan(gnostr_list_document_get_kind(snapshot), gnostr_list_document_get_identifier(snapshot));
  }
  entries[n] = local_plan(kind, identifier);
  gboolean ok = gnostr_list_service_update_read_plan(self->service, entries, n + 1, error);
  g_ptr_array_unref(snapshots);
  return ok ? gnostr_list_service_lookup(self->service, kind, identifier) : NULL;
}

GNostrListDocument *
gh_social_lists_apply(GhSocialLists *self, gint kind, const gchar *identifier, guint64 revision,
                      GPtrArray *operations, GError **error)
{
  if (!self->service) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account list storage is not open");
    return NULL;
  }
  return gnostr_list_service_apply(self->service, kind, identifier, revision, operations, error);
}

gchar *
gh_social_lists_export(GNostrListDocument *snapshot)
{
  const NostrListDocument *codec = gnostr_list_document_get_codec(snapshot);
  json_t *root = json_pack("{s:i,s:s}", "kind", gnostr_list_document_get_kind(snapshot),
                          "identifier", gnostr_list_document_get_identifier(snapshot));
  for (guint p = 0; p < 2; p++) {
    json_t *tags = json_array();
    for (gsize i = 0; i < nostr_nip51_document_tag_count(codec, p); i++) {
      const NostrListTag *tag = nostr_nip51_document_tag(codec, p, i);
      json_t *fields = json_array();
      for (gsize f = 0; f < nostr_nip51_tag_size(tag); f++)
        json_array_append_new(fields, json_string(nostr_nip51_tag_field(tag, f)));
      json_array_append_new(tags, fields);
    }
    json_object_set_new(root, p ? "private" : "public", tags);
  }
  gchar *result = json_dumps(root, JSON_INDENT(2));
  json_decref(root);
  return result;
}

GNostrListDocument *
gh_social_lists_import(GhSocialLists *self, const gchar *wire, GError **error)
{
  if (!wire || strlen(wire) > NOSTR_LIST_MAX_FOLLOW_BYTES) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "List import exceeds the document limit");
    return NULL;
  }
  json_t *root = json_loads(wire, JSON_REJECT_DUPLICATES, NULL);
  json_t *k = json_object_get(root, "kind"), *id = json_object_get(root, "identifier");
  json_int_t kind_value = json_integer_value(k);
  gint kind = kind_value >= 0 && kind_value <= 65535 ? (gint)kind_value : -1;
  const gchar *identifier = json_string_value(id);
  GNostrListDocument *result = NULL;
  GPtrArray *ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  gboolean valid = json_is_integer(k) && kind >= 0 && kind <= 65535 && identifier &&
    strlen(identifier) == json_string_length(id);
  for (guint p = 0; valid && p < 2; p++) {
    json_t *tags = json_object_get(root, p ? "private" : "public");
    valid = json_is_array(tags) && json_array_size(tags) <= NOSTR_LIST_MAX_TAGS;
    for (gsize i = 0; valid && i < json_array_size(tags); i++) {
      json_t *tag = json_array_get(tags, i);
      gsize n = json_array_size(tag);
      const gchar *fields[NOSTR_LIST_MAX_FIELDS];
      valid = json_is_array(tag) && n > 0 && n <= NOSTR_LIST_MAX_FIELDS;
      for (gsize f = 0; valid && f < n; f++) {
        json_t *field = json_array_get(tag, f);
        fields[f] = json_string_value(field);
        valid = fields[f] && strlen(fields[f]) == json_string_length(field);
      }
      if (!valid)
        break;
      if (!strcmp(fields[0], "d")) {
        valid = !p && n == 2 && !strcmp(fields[1], identifier);
        continue;
      }
      GNostrListOperation *op = gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, p, fields, n, 0, 0);
      valid = op != NULL;
      if (op)
        g_ptr_array_add(ops, op);
    }
  }
  if (!valid) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid local list import");
  } else {
    g_autoptr(GNostrListDocument) snapshot = gh_social_lists_ensure(self, kind, identifier, error);
    if (snapshot) {
      if (gnostr_list_document_get_local_revision(snapshot) > 0 || gnostr_list_document_get_event_id(snapshot)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "A list with this address already exists; choose a new identifier");
      } else {
        result = gh_social_lists_apply(self, kind, identifier, 0, ops, error);
      }
    }
  }
  g_ptr_array_unref(ops);
  json_decref(root);
  return result;
}
