/**
 * Secure Key Storage - Linux libsecret Implementation
 *
 * Uses libsecret to store keys in GNOME Keyring or KDE Wallet.
 * This file is only compiled when HAVE_LIBSECRET is defined.
 */

#ifdef HAVE_LIBSECRET

#include "keystore.h"
#include <libsecret/secret.h>
#include <string.h>

/* Schema for storing Nostr private keys */
static const SecretSchema *
get_nostr_key_schema(void) {
  static SecretSchema schema = {
    .name = "org.gnostr.NostrKey",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = {
      { "npub", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { "application", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { NULL, 0 }
    }
  };
  return &schema;
}

#define GNOSTR_APP_ID "org.gnostr.Client"

/* Error quark */
G_DEFINE_QUARK(gnostr-keystore-error-quark, gnostr_keystore_error)

/* Key info helpers */
void gnostr_key_info_free(GnostrKeyInfo *info) {
  if (!info) return;
  g_free(info->npub);
  g_free(info->label);
  g_free(info);
}

GnostrKeyInfo *gnostr_key_info_copy(const GnostrKeyInfo *info) {
  if (!info) return NULL;
  GnostrKeyInfo *copy = g_new0(GnostrKeyInfo, 1);
  copy->npub = g_strdup(info->npub);
  copy->label = g_strdup(info->label);
  copy->created_at = info->created_at;
  return copy;
}

gboolean gnostr_keystore_available(void) {
  /* Check if we can get the default collection */
  GError *error = NULL;
  SecretService *service = secret_service_get_sync(
      SECRET_SERVICE_LOAD_COLLECTIONS, NULL, &error);

  if (!service) {
    g_clear_error(&error);
    return FALSE;
  }

  g_object_unref(service);
  return TRUE;
}

/* Validate npub format (basic check) */
static gboolean validate_npub(const char *npub, GError **error) {
  if (!npub || !g_str_has_prefix(npub, "npub1") || strlen(npub) != 63) {
    g_set_error(error,
                GNOSTR_KEYSTORE_ERROR,
                GNOSTR_KEYSTORE_ERROR_INVALID_KEY,
                "Invalid npub format: expected npub1... with 63 characters");
    return FALSE;
  }
  return TRUE;
}

/* Validate nsec format (basic check) */
static gboolean validate_nsec(const char *nsec, GError **error) {
  if (!nsec || !g_str_has_prefix(nsec, "nsec1") || strlen(nsec) != 63) {
    g_set_error(error,
                GNOSTR_KEYSTORE_ERROR,
                GNOSTR_KEYSTORE_ERROR_INVALID_KEY,
                "Invalid nsec format: expected nsec1... with 63 characters");
    return FALSE;
  }
  return TRUE;
}

gboolean gnostr_keystore_store_key(const char *npub,
                                    const char *nsec,
                                    const char *label,
                                    GError **error) {
  if (!validate_npub(npub, error)) return FALSE;
  if (!validate_nsec(nsec, error)) return FALSE;

  char *display_label = g_strdup_printf("GNostr: %s",
                                         label ? label : npub);

  gboolean result = secret_password_store_sync(
      get_nostr_key_schema(),
      SECRET_COLLECTION_DEFAULT,
      display_label,
      nsec,
      NULL, /* GCancellable */
      error,
      "npub", npub,
      "application", GNOSTR_APP_ID,
      NULL);

  g_free(display_label);

  if (!result && error && *error) {
    /* Map libsecret errors to our error codes */
    if (g_error_matches(*error, SECRET_ERROR, SECRET_ERROR_IS_LOCKED)) {
      GError *new_error = g_error_new(GNOSTR_KEYSTORE_ERROR,
                                       GNOSTR_KEYSTORE_ERROR_ACCESS_DENIED,
                                       "Keyring is locked: %s",
                                       (*error)->message);
      g_clear_error(error);
      *error = new_error;
    }
  }

  return result;
}

/* Async store implementation */
typedef struct {
  char *npub;
  char *nsec;
  char *label;
} StoreAsyncData;

static void store_async_data_free(gpointer data) {
  StoreAsyncData *d = data;
  if (d->nsec) {
    /* Securely clear nsec from memory */
    memset(d->nsec, 0, strlen(d->nsec));
    g_free(d->nsec);
  }
  g_free(d->npub);
  g_free(d->label);
  g_free(d);
}

static void store_key_thread(GTask *task,
                              gpointer source_object,
                              gpointer task_data,
                              GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;

  StoreAsyncData *data = task_data;
  GError *error = NULL;

  gboolean result = gnostr_keystore_store_key(data->npub,
                                               data->nsec,
                                               data->label,
                                               &error);

  if (result) {
    g_task_return_boolean(task, TRUE);
  } else {
    g_task_return_error(task, error);
  }
}

void gnostr_keystore_store_key_async(const char *npub,
                                      const char *nsec,
                                      const char *label,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  StoreAsyncData *data = g_new0(StoreAsyncData, 1);
  data->npub = g_strdup(npub);
  data->nsec = g_strdup(nsec);
  data->label = g_strdup(label);

  g_task_set_task_data(task, data, store_async_data_free);
  g_task_run_in_thread(task, store_key_thread);
  g_object_unref(task);
}

gboolean gnostr_keystore_store_key_finish(GAsyncResult *result,
                                           GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

char *gnostr_keystore_retrieve_key(const char *npub, GError **error) {
  if (!validate_npub(npub, error)) return NULL;

  char *nsec = secret_password_lookup_sync(
      get_nostr_key_schema(),
      NULL, /* GCancellable */
      error,
      "npub", npub,
      "application", GNOSTR_APP_ID,
      NULL);

  if (!nsec && (!error || !*error)) {
    g_set_error(error,
                GNOSTR_KEYSTORE_ERROR,
                GNOSTR_KEYSTORE_ERROR_NOT_FOUND,
                "Key not found for npub: %s", npub);
  }

  return nsec;
}

/* Async retrieve implementation */
typedef struct {
  char *npub;
} RetrieveAsyncData;

static void retrieve_async_data_free(gpointer data) {
  RetrieveAsyncData *d = data;
  g_free(d->npub);
  g_free(d);
}

static void retrieve_key_thread(GTask *task,
                                 gpointer source_object,
                                 gpointer task_data,
                                 GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;

  RetrieveAsyncData *data = task_data;
  GError *error = NULL;

  char *nsec = gnostr_keystore_retrieve_key(data->npub, &error);

  if (nsec) {
    g_task_return_pointer(task, nsec, g_free);
  } else {
    g_task_return_error(task, error);
  }
}

void gnostr_keystore_retrieve_key_async(const char *npub,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  RetrieveAsyncData *data = g_new0(RetrieveAsyncData, 1);
  data->npub = g_strdup(npub);

  g_task_set_task_data(task, data, retrieve_async_data_free);
  g_task_run_in_thread(task, retrieve_key_thread);
  g_object_unref(task);
}

char *gnostr_keystore_retrieve_key_finish(GAsyncResult *result,
                                           GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

gboolean gnostr_keystore_delete_key(const char *npub, GError **error) {
  if (!validate_npub(npub, error)) return FALSE;

  gboolean result = secret_password_clear_sync(
      get_nostr_key_schema(),
      NULL, /* GCancellable */
      error,
      "npub", npub,
      "application", GNOSTR_APP_ID,
      NULL);

  /* secret_password_clear_sync returns FALSE if nothing was deleted,
   * but doesn't set an error. We want to report this as NOT_FOUND. */
  if (!result && (!error || !*error)) {
    g_set_error(error,
                GNOSTR_KEYSTORE_ERROR,
                GNOSTR_KEYSTORE_ERROR_NOT_FOUND,
                "Key not found for npub: %s", npub);
  }

  return result;
}

/* Async delete implementation */
static void delete_key_thread(GTask *task,
                               gpointer source_object,
                               gpointer task_data,
                               GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;

  RetrieveAsyncData *data = task_data; /* Reuse same struct */
  GError *error = NULL;

  gboolean result = gnostr_keystore_delete_key(data->npub, &error);

  if (result) {
    g_task_return_boolean(task, TRUE);
  } else {
    g_task_return_error(task, error);
  }
}

void gnostr_keystore_delete_key_async(const char *npub,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  RetrieveAsyncData *data = g_new0(RetrieveAsyncData, 1);
  data->npub = g_strdup(npub);

  g_task_set_task_data(task, data, retrieve_async_data_free);
  g_task_run_in_thread(task, delete_key_thread);
  g_object_unref(task);
}

gboolean gnostr_keystore_delete_key_finish(GAsyncResult *result,
                                            GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

GList *gnostr_keystore_list_keys(GError **error) {
  GList *result = NULL;

  /* Search for all items matching our schema and application */
  GHashTable *attrs = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(attrs, "application", GNOSTR_APP_ID);

  GList *items = secret_password_search_sync(
      get_nostr_key_schema(),
      SECRET_SEARCH_ALL,
      NULL, /* GCancellable */
      error,
      "application", GNOSTR_APP_ID,
      NULL);

  g_hash_table_unref(attrs);

  if (!items) {
    /* No items found is not an error */
    if (error && *error) {
      return NULL;
    }
    return NULL;
  }

  for (GList *l = items; l != NULL; l = l->next) {
    SecretRetrievable *item = SECRET_RETRIEVABLE(l->data);
    GHashTable *item_attrs = secret_retrievable_get_attributes(item);

    const char *npub = g_hash_table_lookup(item_attrs, "npub");
    if (npub) {
      GnostrKeyInfo *info = g_new0(GnostrKeyInfo, 1);
      info->npub = g_strdup(npub);
      info->label = g_strdup(secret_retrievable_get_label(item));
      info->created_at = (gint64)secret_retrievable_get_created(item);

      result = g_list_prepend(result, info);
    }

    g_hash_table_unref(item_attrs);
  }

  g_list_free_full(items, g_object_unref);

  return g_list_reverse(result);
}

/* Async list implementation */
static void list_keys_thread(GTask *task,
                              gpointer source_object,
                              gpointer task_data,
                              GCancellable *cancellable) {
  (void)source_object;
  (void)task_data;
  (void)cancellable;

  GError *error = NULL;
  GList *keys = gnostr_keystore_list_keys(&error);

  if (error) {
    g_task_return_error(task, error);
  } else {
    g_task_return_pointer(task, keys, NULL);
  }
}

void gnostr_keystore_list_keys_async(GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_run_in_thread(task, list_keys_thread);
  g_object_unref(task);
}

GList *gnostr_keystore_list_keys_finish(GAsyncResult *result,
                                         GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

gboolean gnostr_keystore_has_key(const char *npub) {
  if (!npub || !g_str_has_prefix(npub, "npub1")) return FALSE;

  char *nsec = secret_password_lookup_sync(
      get_nostr_key_schema(),
      NULL,
      NULL,
      "npub", npub,
      "application", GNOSTR_APP_ID,
      NULL);

  if (nsec) {
    /* Securely clear from memory */
    memset(nsec, 0, strlen(nsec));
    secret_password_free(nsec);
    return TRUE;
  }

  return FALSE;
}

#endif /* HAVE_LIBSECRET */
