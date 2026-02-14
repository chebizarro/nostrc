/**
 * GNostr Identity Management Implementation
 *
 * GSettings schema ID is injected via gnostr_identity_init().
 * The library has no opinion about schema names.
 */

#include "gnostr-identity.h"
#include "keystore.h"
#include <nostr-gobject-1.0/nostr_keys.h>
#include <string.h>

/* Constructor-injected GSettings schema ID and key */
static const char *s_identity_schema_id = NULL;
#define SETTINGS_KEY_CURRENT_NPUB "current-npub"

void gnostr_identity_init(const char *schema_id) {
  s_identity_schema_id = schema_id;
}

void gnostr_identity_free(GNostrIdentity *identity) {
  if (!identity) return;
  g_free(identity->npub);
  g_free(identity->label);
  g_free(identity->signer_type);
  g_free(identity);
}

GNostrIdentity *gnostr_identity_copy(const GNostrIdentity *identity) {
  if (!identity) return NULL;
  GNostrIdentity *copy = g_new0(GNostrIdentity, 1);
  copy->npub = g_strdup(identity->npub);
  copy->label = g_strdup(identity->label);
  copy->has_local_key = identity->has_local_key;
  copy->signer_type = g_strdup(identity->signer_type);
  return copy;
}

GNostrIdentity *gnostr_identity_get_current(void) {
  if (!s_identity_schema_id) {
    g_warning("gnostr_identity_get_current: schema not set, call gnostr_identity_init() first");
    return NULL;
  }

  GSettings *settings = g_settings_new(s_identity_schema_id);
  if (!settings) return NULL;

  char *npub = g_settings_get_string(settings, SETTINGS_KEY_CURRENT_NPUB);
  g_object_unref(settings);

  if (!npub || !*npub || !g_str_has_prefix(npub, "npub1")) {
    g_free(npub);
    return NULL;
  }

  GNostrIdentity *identity = g_new0(GNostrIdentity, 1);
  identity->npub = npub;
  identity->has_local_key = gnostr_keystore_has_key(npub);

  if (identity->has_local_key) {
    identity->signer_type = g_strdup("local");
  } else {
    /* Could be NIP-55L or NIP-46, we don't track this yet */
    identity->signer_type = g_strdup("external");
  }

  return identity;
}

void gnostr_identity_set_current(const char *npub) {
  if (!s_identity_schema_id) {
    g_warning("gnostr_identity_set_current: schema not set, call gnostr_identity_init() first");
    return;
  }

  GSettings *settings = g_settings_new(s_identity_schema_id);
  if (!settings) return;

  g_settings_set_string(settings, SETTINGS_KEY_CURRENT_NPUB, npub ? npub : "");
  g_object_unref(settings);
}

GList *gnostr_identity_list_stored(GError **error) {
  GList *keys = gnostr_keystore_list_keys(error);
  if (!keys) return NULL;

  GList *identities = NULL;
  for (GList *l = keys; l != NULL; l = l->next) {
    GnostrKeyInfo *key_info = (GnostrKeyInfo *)l->data;

    GNostrIdentity *identity = g_new0(GNostrIdentity, 1);
    identity->npub = g_strdup(key_info->npub);
    identity->label = g_strdup(key_info->label);
    identity->has_local_key = TRUE;
    identity->signer_type = g_strdup("local");

    identities = g_list_prepend(identities, identity);
  }

  g_list_free_full(keys, (GDestroyNotify)gnostr_key_info_free);

  return g_list_reverse(identities);
}

/* Helper to derive npub from nsec using GNostrKeys */
static char *derive_npub_from_nsec(const char *nsec, GError **error) {
  if (!nsec || !g_str_has_prefix(nsec, "nsec1")) {
    g_set_error(error, GNOSTR_KEYSTORE_ERROR,
                GNOSTR_KEYSTORE_ERROR_INVALID_KEY,
                "Invalid nsec format");
    return NULL;
  }

  GError *key_error = NULL;
  g_autoptr(GNostrKeys) keys = gnostr_keys_new_from_nsec(nsec, &key_error);
  if (!keys) {
    g_set_error(error, GNOSTR_KEYSTORE_ERROR,
                GNOSTR_KEYSTORE_ERROR_INVALID_KEY,
                "Failed to import nsec: %s",
                key_error ? key_error->message : "unknown error");
    g_clear_error(&key_error);
    return NULL;
  }

  gchar *npub = gnostr_keys_get_npub(keys);
  if (!npub) {
    g_set_error(error, GNOSTR_KEYSTORE_ERROR,
                GNOSTR_KEYSTORE_ERROR_FAILED,
                "Failed to encode npub");
    return NULL;
  }

  return npub;
}

char *gnostr_identity_import_nsec(const char *nsec,
                                   const char *label,
                                   GError **error) {
  /* Derive npub from nsec */
  char *npub = derive_npub_from_nsec(nsec, error);
  if (!npub) return NULL;

  /* Store in secure storage */
  if (!gnostr_keystore_store_key(npub, nsec, label, error)) {
    g_free(npub);
    return NULL;
  }

  return npub;
}

/* Async import implementation */
typedef struct {
  char *nsec;
  char *label;
} ImportAsyncData;

static void import_async_data_free(gpointer data) {
  ImportAsyncData *d = data;
  if (d->nsec) {
    memset(d->nsec, 0, strlen(d->nsec));
    g_free(d->nsec);
  }
  g_free(d->label);
  g_free(d);
}

static void import_nsec_thread(GTask *task,
                                gpointer source_object,
                                gpointer task_data,
                                GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;

  ImportAsyncData *data = task_data;
  GError *error = NULL;

  char *npub = gnostr_identity_import_nsec(data->nsec, data->label, &error);

  if (npub) {
    g_task_return_pointer(task, npub, g_free);
  } else {
    g_task_return_error(task, error);
  }
}

void gnostr_identity_import_nsec_async(const char *nsec,
                                        const char *label,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  ImportAsyncData *data = g_new0(ImportAsyncData, 1);
  data->nsec = g_strdup(nsec);
  data->label = g_strdup(label);

  g_task_set_task_data(task, data, import_async_data_free);
  g_task_run_in_thread(task, import_nsec_thread);
  g_object_unref(task);
}

char *gnostr_identity_import_nsec_finish(GAsyncResult *result,
                                          GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

char *gnostr_identity_get_nsec(const char *npub, GError **error) {
  return gnostr_keystore_retrieve_key(npub, error);
}

/* Async get nsec implementation */
typedef struct {
  char *npub;
} GetNsecAsyncData;

static void get_nsec_async_data_free(gpointer data) {
  GetNsecAsyncData *d = data;
  g_free(d->npub);
  g_free(d);
}

static void get_nsec_thread(GTask *task,
                             gpointer source_object,
                             gpointer task_data,
                             GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;

  GetNsecAsyncData *data = task_data;
  GError *error = NULL;

  char *nsec = gnostr_identity_get_nsec(data->npub, &error);

  if (nsec) {
    g_task_return_pointer(task, nsec, g_free);
  } else {
    g_task_return_error(task, error);
  }
}

void gnostr_identity_get_nsec_async(const char *npub,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  GetNsecAsyncData *data = g_new0(GetNsecAsyncData, 1);
  data->npub = g_strdup(npub);

  g_task_set_task_data(task, data, get_nsec_async_data_free);
  g_task_run_in_thread(task, get_nsec_thread);
  g_object_unref(task);
}

char *gnostr_identity_get_nsec_finish(GAsyncResult *result,
                                       GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

gboolean gnostr_identity_delete(const char *npub, GError **error) {
  return gnostr_keystore_delete_key(npub, error);
}

gboolean gnostr_identity_has_local_key(const char *npub) {
  return gnostr_keystore_has_key(npub);
}

gboolean gnostr_identity_secure_storage_available(void) {
  return gnostr_keystore_available();
}

void gnostr_identity_clear_nsec(char *nsec) {
  if (nsec) {
    memset(nsec, 0, strlen(nsec));
    g_free(nsec);
  }
}
