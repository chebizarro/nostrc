#include <gnostr-plugin-api.h>

GPtrArray *gnostr_plugin_context_query_events(
    GnostrPluginContext *context, const char *filter_json, GError **error) {
  (void)context;
  (void)filter_json;
  (void)error;
  return g_ptr_array_new_with_free_func(g_free);
}
char *gnostr_plugin_context_get_event_by_id(
    GnostrPluginContext *context, const char *event_id_hex, GError **error) {
  (void)context;
  (void)event_id_hex;
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
              "Offline test backend has no event");
  return NULL;
}
guint64 gnostr_plugin_context_subscribe_events(
    GnostrPluginContext *context, const char *filter_json,
    GCallback callback, gpointer user_data, GDestroyNotify destroy_notify) {
  (void)context;
  (void)filter_json;
  (void)callback;
  (void)user_data;
  (void)destroy_notify;
  return 0;
}
void gnostr_plugin_context_unsubscribe_events(
    GnostrPluginContext *context, guint64 subscription_id) {
  (void)context;
  (void)subscription_id;
}
const char *gnostr_plugin_context_get_user_pubkey(
    GnostrPluginContext *context) {
  (void)context;
  return NULL;
}
void gnostr_plugin_context_request_sign_event(
    GnostrPluginContext *context, const char *unsigned_event_json,
    GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data) {
  (void)context;
  (void)unsigned_event_json;
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                          "Offline test backend cannot sign");
  g_object_unref(task);
}
char *gnostr_plugin_context_request_sign_event_finish(
    GnostrPluginContext *context, GAsyncResult *result, GError **error) {
  (void)context;
  return g_task_propagate_pointer(G_TASK(result), error);
}
void gnostr_plugin_context_publish_event_async(
    GnostrPluginContext *context, const char *event_json,
    GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data) {
  (void)context;
  (void)event_json;
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                          "Offline test backend cannot publish");
  g_object_unref(task);
}
gboolean gnostr_plugin_context_publish_event_finish(
    GnostrPluginContext *context, GAsyncResult *result, GError **error) {
  (void)context;
  return g_task_propagate_boolean(G_TASK(result), error);
}
