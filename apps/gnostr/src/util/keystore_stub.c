/**
 * Secure Key Storage - Stub Implementation
 *
 * Provides stub functions when no platform-native secure storage is available.
 * All operations return errors indicating the feature is not available.
 */

#if !defined(HAVE_LIBSECRET) && !defined(HAVE_MACOS_KEYCHAIN)

#include "keystore.h"

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

static void set_not_available_error(GError **error) {
  g_set_error(error,
              GNOSTR_KEYSTORE_ERROR,
              GNOSTR_KEYSTORE_ERROR_NOT_AVAILABLE,
              "Secure key storage is not available on this platform. "
              "Install libsecret (Linux) or build on macOS to enable.");
}

gboolean gnostr_keystore_available(void) {
  return FALSE;
}

gboolean gnostr_keystore_store_key(const char *npub,
                                    const char *nsec,
                                    const char *label,
                                    GError **error) {
  (void)npub;
  (void)nsec;
  (void)label;
  set_not_available_error(error);
  return FALSE;
}

void gnostr_keystore_store_key_async(const char *npub,
                                      const char *nsec,
                                      const char *label,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data) {
  (void)npub;
  (void)nsec;
  (void)label;

  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  GError *error = NULL;
  set_not_available_error(&error);
  g_task_return_error(task, error);
  g_object_unref(task);
}

gboolean gnostr_keystore_store_key_finish(GAsyncResult *result,
                                           GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

char *gnostr_keystore_retrieve_key(const char *npub, GError **error) {
  (void)npub;
  set_not_available_error(error);
  return NULL;
}

void gnostr_keystore_retrieve_key_async(const char *npub,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data) {
  (void)npub;

  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  GError *error = NULL;
  set_not_available_error(&error);
  g_task_return_error(task, error);
  g_object_unref(task);
}

char *gnostr_keystore_retrieve_key_finish(GAsyncResult *result,
                                           GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

gboolean gnostr_keystore_delete_key(const char *npub, GError **error) {
  (void)npub;
  set_not_available_error(error);
  return FALSE;
}

void gnostr_keystore_delete_key_async(const char *npub,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data) {
  (void)npub;

  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  GError *error = NULL;
  set_not_available_error(&error);
  g_task_return_error(task, error);
  g_object_unref(task);
}

gboolean gnostr_keystore_delete_key_finish(GAsyncResult *result,
                                            GError **error) {
  return g_task_propagate_boolean(G_TASK(result), error);
}

GList *gnostr_keystore_list_keys(GError **error) {
  set_not_available_error(error);
  return NULL;
}

void gnostr_keystore_list_keys_async(GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  GError *error = NULL;
  set_not_available_error(&error);
  g_task_return_error(task, error);
  g_object_unref(task);
}

GList *gnostr_keystore_list_keys_finish(GAsyncResult *result,
                                         GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

gboolean gnostr_keystore_has_key(const char *npub) {
  (void)npub;
  return FALSE;
}

#endif /* !HAVE_LIBSECRET && !HAVE_MACOS_KEYCHAIN */
