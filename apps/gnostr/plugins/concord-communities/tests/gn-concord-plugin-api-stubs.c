/* Offline host backend for the Concord plugin's service tests: no relay, no
 * storage, no signer. The service must stay fully exercisable without them —
 * everything Concord needs to *read* a plane is pure derivation. */

#include <gnostr-plugin-api.h>
#include <nostr-event.h>

#include <stdlib.h>
#include <string.h>

/* Test hooks. With no signer key set the backend refuses to sign, which is
 * the deactivated-host case; setting one lets a test drive the whole mint
 * path and feed the resulting wrap back through the reader. */
const char *gn_concord_test_signer_sk = NULL;
const char *gn_concord_test_user_pubkey = NULL;
char *gn_concord_test_published_json = NULL;

GPtrArray *gnostr_plugin_context_query_events(GnostrPluginContext *context,
                                              const char *filter_json,
                                              GError **error) {
  (void)context;
  (void)filter_json;
  (void)error;
  return g_ptr_array_new_with_free_func(g_free);
}

char *gnostr_plugin_context_get_event_by_id(GnostrPluginContext *context,
                                            const char *event_id_hex,
                                            GError **error) {
  (void)context;
  (void)event_id_hex;
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
              "Offline test backend has no event");
  return NULL;
}

guint64 gnostr_plugin_context_subscribe_events(GnostrPluginContext *context,
                                               const char *filter_json,
                                               GCallback callback,
                                               gpointer user_data,
                                               GDestroyNotify destroy_notify) {
  (void)context;
  (void)filter_json;
  (void)callback;
  /* Matches the host: a failed subscribe (0) leaves @user_data with the
   * caller and never runs @destroy_notify — that only fires on unsubscribe
   * after a successful subscription. */
  (void)user_data;
  (void)destroy_notify;
  return 0;
}

void gnostr_plugin_context_unsubscribe_events(GnostrPluginContext *context,
                                              guint64 subscription_id) {
  (void)context;
  (void)subscription_id;
}

const char *gnostr_plugin_context_get_user_pubkey(
    GnostrPluginContext *context) {
  (void)context;
  return gn_concord_test_user_pubkey;
}

void gnostr_plugin_context_request_sign_event(GnostrPluginContext *context,
                                              const char *unsigned_event_json,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data) {
  (void)context;
  (void)unsigned_event_json;
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  if (!gn_concord_test_signer_sk) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Offline test backend cannot sign");
    g_object_unref(task);
    return;
  }
  NostrEvent *event = nostr_event_new();
  if (!event ||
      !nostr_event_deserialize_compact(event, unsigned_event_json, NULL) ||
      nostr_event_sign(event, gn_concord_test_signer_sk) != 0) {
    nostr_event_free(event);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Offline test backend failed to sign");
    g_object_unref(task);
    return;
  }
  char *signed_json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_task_return_pointer(task, signed_json, free);
  g_object_unref(task);
}

char *gnostr_plugin_context_request_sign_event_finish(
    GnostrPluginContext *context, GAsyncResult *result, GError **error) {
  (void)context;
  return g_task_propagate_pointer(G_TASK(result), error);
}

void gnostr_plugin_context_publish_event_async(GnostrPluginContext *context,
                                               const char *event_json,
                                               GCancellable *cancellable,
                                               GAsyncReadyCallback callback,
                                               gpointer user_data) {
  (void)context;
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  if (!gn_concord_test_signer_sk) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Offline test backend cannot publish");
    g_object_unref(task);
    return;
  }
  free(gn_concord_test_published_json);
  gn_concord_test_published_json =
    event_json ? strdup(event_json) : NULL;
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}

gboolean gnostr_plugin_context_publish_event_finish(
    GnostrPluginContext *context, GAsyncResult *result, GError **error) {
  (void)context;
  return g_task_propagate_boolean(G_TASK(result), error);
}

gboolean gnostr_plugin_context_store_data(GnostrPluginContext *context,
                                          const char *key, GBytes *data,
                                          GError **error) {
  (void)context;
  (void)key;
  (void)data;
  (void)error;
  return TRUE;
}

GBytes *gnostr_plugin_context_load_data(GnostrPluginContext *context,
                                        const char *key, GError **error) {
  (void)context;
  (void)key;
  (void)error;
  return NULL;
}

gboolean gnostr_plugin_context_delete_data(GnostrPluginContext *context,
                                           const char *key) {
  (void)context;
  (void)key;
  return FALSE;
}
