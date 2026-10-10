#include "gh-nip46-credentials-private.h"
#include <libsecret/secret.h>

GBytes *gh_nip46_credentials_secret_bytes_new(const void *data, gsize len);

static const SecretSchema schema = {
  .name = GH_NIP46_CREDENTIAL_SCHEMA,
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "account", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "version", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 },
  },
};

typedef struct { GhNip46CredentialBackend base; } SecretBackend;

static GError *
map_error(GError *source, gboolean connecting)
{
  if (g_error_matches(source, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return source;
  gboolean unavailable = connecting ||
    g_error_matches(source, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
    g_error_matches(source, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
    g_error_matches(source, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY) ||
    g_error_matches(source, G_DBUS_ERROR, G_DBUS_ERROR_TIMED_OUT) ||
    g_error_matches(source, G_DBUS_ERROR, G_DBUS_ERROR_DISCONNECTED) ||
    g_error_matches(source, G_IO_ERROR, G_IO_ERROR_CLOSED) ||
    g_error_matches(source, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED);
  GhNip46CredentialError code = unavailable ? GH_NIP46_CREDENTIAL_ERROR_UNAVAILABLE :
    g_error_matches(source, SECRET_ERROR, SECRET_ERROR_IS_LOCKED) ?
      GH_NIP46_CREDENTIAL_ERROR_LOCKED : GH_NIP46_CREDENTIAL_ERROR_FAILED;
  GError *error = g_error_new_literal(GH_NIP46_CREDENTIAL_ERROR, code,
    code == GH_NIP46_CREDENTIAL_ERROR_LOCKED ? "The keyring is locked" :
    code == GH_NIP46_CREDENTIAL_ERROR_UNAVAILABLE ? "Secret Service unavailable" :
    "Secret Service operation failed");
  g_error_free(source);
  return error;
}

static void
on_session(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  *(GAsyncResult **)user_data = g_object_ref(result);
}

/* The shared Secret Service with its session open, or NULL with *error set.
 * The session is opened through libsecret's asynchronous path, on a private
 * main context, and never its synchronous one (nostrc-ep54q): in libsecret
 * 0.21.8 _secret_session_open_sync() dereferences a NULL GError when the AES
 * session is refused and it falls back to "plain" (as
 * secret_service_search_sync(..., SECRET_SEARCH_LOAD_SECRETS, ...) passes
 * one), and on any failure it releases its session with g_clear_object(),
 * which is a CRITICAL and leaks the session and its key. The service keeps
 * the session, so the synchronous calls below never open one themselves. */
static SecretService *
open_service(GCancellable *cancellable, GError **error)
{
  GError *local = NULL;
  SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, cancellable, &local);
  if (service) {
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GAsyncResult *result = NULL;
    secret_service_ensure_session(service, cancellable, on_session, &result);
    while (!result) g_main_context_iteration(context, TRUE);
    if (!secret_service_ensure_session_finish(service, result, &local)) g_clear_object(&service);
    g_object_unref(result);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
  }
  if (!service) *error = map_error(local, TRUE);
  return service;
}

static GHashTable *
attributes(const gchar *account, gboolean version)
{
  GHashTable *attrs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (account) g_hash_table_insert(attrs, g_strdup("account"), g_strdup(account));
  if (version) g_hash_table_insert(attrs, g_strdup("version"), g_strdup(GH_NIP46_CREDENTIAL_VERSION));
  return attrs;
}

static gboolean
check_empty_default(SecretService *service, GCancellable *cancellable, GError **error)
{
  GError *local = NULL;
  SecretCollection *collection = secret_collection_for_alias_sync(service, SECRET_COLLECTION_DEFAULT,
    SECRET_COLLECTION_NONE, cancellable, &local);
  if (local) { *error = map_error(local, FALSE); return FALSE; }
  gboolean locked = collection && secret_collection_get_locked(collection);
  g_clear_object(&collection);
  if (locked) {
    g_set_error_literal(error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_LOCKED,
                        "The keyring is locked");
    return FALSE;
  }
  return TRUE;
}

static GPtrArray *
ss_search(GhNip46CredentialBackend *backend, const gchar *account,
          gboolean interactive, gboolean load_secrets, GCancellable *cancellable,
          GError **error)
{
  (void)backend;
  SecretService *service = open_service(cancellable, error);
  if (!service) return NULL;
  GHashTable *attrs = attributes(account, FALSE);
  /* Listing never loads secrets or unlocks: a locked keyring would otherwise
   * wait on an unlock prompt and stall account discovery. */
  SecretSearchFlags flags = SECRET_SEARCH_ALL;
  if (load_secrets) flags |= SECRET_SEARCH_LOAD_SECRETS;
  if (interactive && load_secrets) flags |= SECRET_SEARCH_UNLOCK;
  GError *local = NULL;
  GList *found = secret_service_search_sync(service, &schema, attrs, flags, cancellable, &local);
  g_hash_table_unref(attrs);
  if (local) {
    *error = map_error(local, FALSE);
    g_object_unref(service);
    return NULL;
  }
  if (!found && load_secrets && !check_empty_default(service, cancellable, error)) {
    g_object_unref(service);
    return NULL;
  }
  GPtrArray *items = g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip46_credential_item_free);
  for (GList *l = found; l; l = l->next) {
    SecretItem *raw = l->data;
    GhNip46CredentialItem *item = g_new0(GhNip46CredentialItem, 1);
    GHashTable *ia = secret_item_get_attributes(raw);
    item->account = g_strdup(g_hash_table_lookup(ia, "account"));
    item->version = g_strdup(g_hash_table_lookup(ia, "version"));
    item->label = secret_item_get_label(raw);
    item->locked = secret_item_get_locked(raw);
    item->attributes_valid = TRUE;
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, ia);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
      const gchar *name = key;
      if (!g_str_equal(name, "account") && !g_str_equal(name, "version") &&
          !g_str_equal(name, "xdg:schema")) item->attributes_valid = FALSE;
    }
    if (load_secrets && !item->locked) {
      SecretValue *value = secret_item_get_secret(raw);
      if (value) {
        gsize len = 0;
        const gchar *data = secret_value_get(value, &len);
        if (len <= 8192) item->secret = gh_nip46_credentials_secret_bytes_new(data, len);
        secret_value_unref(value);
      }
    }
    g_hash_table_unref(ia);
    g_ptr_array_add(items, item);
  }
  g_list_free_full(found, g_object_unref);
  g_object_unref(service);
  return items;
}

static gboolean
ss_write(GhNip46CredentialBackend *backend, const gchar *account, GBytes *secret,
         gboolean interactive, GError **error)
{
  (void)backend;
  SecretService *service = open_service(NULL, error);
  if (!service) return FALSE;
  gsize len = 0;
  const gchar *data = g_bytes_get_data(secret, &len);
  SecretValue *value = secret_value_new(data, (gssize)len, "application/json");
  GHashTable *attrs = attributes(account, TRUE);
  GError *local = NULL;
  gboolean ok = FALSE;
  if (interactive) {
    ok = secret_service_store_sync(service, &schema, attrs, SECRET_COLLECTION_DEFAULT,
                                   GH_NIP46_CREDENTIAL_LABEL, value, NULL, &local);
  } else {
    SecretCollection *collection = secret_collection_for_alias_sync(service, SECRET_COLLECTION_DEFAULT,
      SECRET_COLLECTION_NONE, NULL, &local);
    if (!local && (!collection || secret_collection_get_locked(collection))) {
      g_set_error_literal(&local, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_LOCKED,
                          "The keyring is locked");
    }
    if (!local) {
      SecretItem *item = secret_item_create_sync(collection, &schema, attrs,
        GH_NIP46_CREDENTIAL_LABEL, value, SECRET_ITEM_CREATE_REPLACE, NULL, &local);
      ok = item != NULL;
      g_clear_object(&item);
    }
    g_clear_object(&collection);
  }
  secret_value_unref(value);
  g_hash_table_unref(attrs);
  g_object_unref(service);
  if (local) {
    if (local->domain == GH_NIP46_CREDENTIAL_ERROR) *error = local;
    else *error = map_error(local, FALSE);
  }
  return ok;
}

static gboolean
ss_remove(GhNip46CredentialBackend *backend, const gchar *account,
          gboolean interactive, GError **error)
{
  (void)backend;
  SecretService *service = open_service(NULL, error);
  if (!service) return FALSE;
  GHashTable *attrs = attributes(account, FALSE);
  SecretSearchFlags flags = SECRET_SEARCH_ALL;
  if (interactive) flags |= SECRET_SEARCH_UNLOCK;
  GError *local = NULL;
  GList *found = secret_service_search_sync(service, &schema, attrs, flags, NULL, &local);
  g_hash_table_unref(attrs);
  if (!local && !found) check_empty_default(service, NULL, &local);
  if (!local) {
    for (GList *l = found; l; l = l->next) {
      SecretItem *item = l->data;
      if (secret_item_get_locked(item)) {
        g_set_error_literal(&local, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_LOCKED,
                            "The keyring is locked");
        break;
      }
      if (!secret_item_delete_sync(item, NULL, &local)) break;
    }
  }
  g_list_free_full(found, g_object_unref);
  g_object_unref(service);
  if (local) {
    if (local->domain == GH_NIP46_CREDENTIAL_ERROR) *error = local;
    else *error = map_error(local, FALSE);
    return FALSE;
  }
  return TRUE;
}

static void ss_free(GhNip46CredentialBackend *backend) { g_free(backend); }
GhNip46CredentialBackend *
gh_nip46_credentials_secret_service_new(void)
{
  SecretBackend *backend = g_new0(SecretBackend, 1);
  backend->base.search = ss_search;
  backend->base.write = ss_write;
  backend->base.remove = ss_remove;
  backend->base.free = ss_free;
  return &backend->base;
}
