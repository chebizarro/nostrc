/* SPDX-License-Identifier: MIT */
#include "fake-service.h"
struct _GrottoPanelsFakeService {
  GObject parent_instance;
  GListStore *models[GROTTO_PANELS_N_MODELS];
  gboolean available, caps[GROTTO_PANEL_N_COMMANDS];
};
static void iface_init(GrottoPanelsServiceInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GrottoPanelsFakeService, grotto_panels_fake_service, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GROTTO_TYPE_PANELS_SERVICE, iface_init))
static GListModel *get_model(GrottoPanelsService *service, GrottoPanelsModel model) {
  return G_LIST_MODEL(GROTTO_PANELS_FAKE_SERVICE(service)->models[model]);
}
static gboolean available(GrottoPanelsService *service) { return GROTTO_PANELS_FAKE_SERVICE(service)->available; }
static gboolean can(GrottoPanelsService *service, GrottoPanelCommand command) {
  return GROTTO_PANELS_FAKE_SERVICE(service)->caps[command];
}
static void invoke_async(GrottoPanelsService *self, GrottoPanelCommand command, GVariant *args,
  GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
  (void)command; (void)args;
  GTask *task = g_task_new(self, cancel, callback, data);
  g_task_return_boolean(task, TRUE);
  g_object_unref(task);
}
static gboolean invoke_finish(GrottoPanelsService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
static void iface_init(GrottoPanelsServiceInterface *iface) {
  iface->get_model = get_model; iface->can = can; iface->is_available = available;
  iface->invoke_async = invoke_async; iface->invoke_finish = invoke_finish;
}
static void dispose(GObject *object) {
  GrottoPanelsFakeService *self = GROTTO_PANELS_FAKE_SERVICE(object);
  for (guint i = 0; i < GROTTO_PANELS_N_MODELS; i++) g_clear_object(&self->models[i]);
  G_OBJECT_CLASS(grotto_panels_fake_service_parent_class)->dispose(object);
}
static void grotto_panels_fake_service_class_init(GrottoPanelsFakeServiceClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose;
}
static void grotto_panels_fake_service_init(GrottoPanelsFakeService *self) {
  for (guint i = 0; i < GROTTO_PANELS_N_MODELS; i++) self->models[i] = g_list_store_new(GROTTO_TYPE_PANELS_ITEM);
}
GrottoPanelsFakeService *grotto_panels_fake_service_new(void) { return g_object_new(GROTTO_TYPE_PANELS_FAKE_SERVICE, NULL); }
void grotto_panels_fake_service_set_available(GrottoPanelsFakeService *self, gboolean value) {
  self->available = value;
  g_signal_emit_by_name(self, "availability-changed");
}
void grotto_panels_fake_service_set_can(GrottoPanelsFakeService *self, GrottoPanelCommand command, gboolean value) {
  g_return_if_fail(command >= 0 && command < GROTTO_PANEL_N_COMMANDS);
  self->caps[command] = value;
  g_signal_emit_by_name(self, "capabilities-changed");
}
void grotto_panels_fake_service_replace(GrottoPanelsFakeService *self, GrottoPanelsModel model, const GrottoPanelsMetadata *metadata) {
  g_return_if_fail(model >= 0 && model < GROTTO_PANELS_N_MODELS);
  g_autoptr(GrottoPanelsItem) item = metadata ? grotto_panels_item_new(metadata) : NULL;
  gpointer items[] = {item};
  g_list_store_splice(self->models[model], 0, g_list_model_get_n_items(G_LIST_MODEL(self->models[model])), items, item ? 1 : 0);
}
