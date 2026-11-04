/* SPDX-License-Identifier: MIT */
#include "fake-service.h"
struct _GrottoPanelsFakeService {
  GObject parent_instance;
  GListStore *models[GROTTO_PANELS_N_MODELS];
  gboolean available, caps[GROTTO_PANEL_N_COMMANDS], deferred;
  gint failures[GROTTO_PANEL_N_COMMANDS];
  GQueue pending;
  guint invocations;
  GrottoPanelCommand last_command;
  GrottoPanelCommandArgs last_args;
};
static void iface_init(GrottoPanelsServiceInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GrottoPanelsFakeService, grotto_panels_fake_service, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GROTTO_TYPE_PANELS_SERVICE, iface_init))
typedef struct { gint failure; gulong cancel_id; } Pending;
static GListModel *get_model(GrottoPanelsService *service, GrottoPanelsModel model) {
  return G_LIST_MODEL(GROTTO_PANELS_FAKE_SERVICE(service)->models[model]);
}
static gboolean available(GrottoPanelsService *service) { return GROTTO_PANELS_FAKE_SERVICE(service)->available; }
static gboolean can(GrottoPanelsService *service, GrottoPanelCommand command) {
  return GROTTO_PANELS_FAKE_SERVICE(service)->caps[command];
}
static void clear_record(GrottoPanelsFakeService *self) {
  GrottoPanelCommandArgs *a = &self->last_args;
  g_free((char *)a->principal); g_free((char *)a->label); g_free((char *)a->public_key); g_free((char *)a->trust);
  g_strfreev((char **)a->relays); g_strfreev((char **)a->methods);
  for (gsize i = 0; i < a->n_permissions; i++) {
    g_free((char *)a->permissions[i].operation); g_free((char *)a->permissions[i].decision);
    g_free((char *)a->permissions[i].relay);
  }
  g_free((gpointer)a->permissions);
  *a = (GrottoPanelCommandArgs){0};
}
static void record(GrottoPanelsFakeService *self, GrottoPanelCommand command, const GrottoPanelCommandArgs *a) {
  clear_record(self);
  self->last_command = command; self->invocations++;
  self->last_args = *a;
  self->last_args.principal = g_strdup(a->principal); self->last_args.label = g_strdup(a->label);
  self->last_args.public_key = g_strdup(a->public_key); self->last_args.trust = g_strdup(a->trust);
  self->last_args.relays = (const char *const *)g_strdupv((char **)a->relays);
  self->last_args.methods = (const char *const *)g_strdupv((char **)a->methods);
  GrottoPanelPermission *permissions = g_new0(GrottoPanelPermission, a->n_permissions);
  self->last_args.permissions = permissions;
  for (gsize i = 0; i < a->n_permissions; i++) {
    permissions[i] = a->permissions[i];
    permissions[i].operation = g_strdup(a->permissions[i].operation);
    permissions[i].decision = g_strdup(a->permissions[i].decision);
    permissions[i].relay = g_strdup(a->permissions[i].relay);
  }
}
static void finish_task(GTask *task) {
  Pending *p = g_task_get_task_data(task);
  GCancellable *cancel = g_task_get_cancellable(task);
  if (p->cancel_id) { g_signal_handler_disconnect(cancel, p->cancel_id); p->cancel_id = 0; }
  if (!g_task_return_error_if_cancelled(task)) {
    if (p->failure >= 0) g_task_return_new_error(task, G_IO_ERROR, p->failure, "Panel command failed");
    else g_task_return_boolean(task, TRUE);
  }
  g_object_unref(task);
}
static void cancelled(GCancellable *cancel, GTask *task) {
  (void)cancel;
  GrottoPanelsFakeService *self = g_task_get_source_object(task);
  if (g_queue_remove(&self->pending, task)) finish_task(task);
}
static void invoke_async(GrottoPanelsService *service, GrottoPanelCommand command, const GrottoPanelCommandArgs *args,
  GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
  GrottoPanelsFakeService *self = GROTTO_PANELS_FAKE_SERVICE(service);
  record(self, command, args);
  GTask *task = g_task_new(self, cancel, callback, data);
  Pending *p = g_new0(Pending, 1); p->failure = self->failures[command];
  g_task_set_task_data(task, p, g_free);
  if (!self->deferred || g_cancellable_is_cancelled(cancel)) { finish_task(task); return; }
  g_queue_push_tail(&self->pending, task);
  if (cancel) p->cancel_id = g_signal_connect(cancel, "cancelled", G_CALLBACK(cancelled), task);
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
  g_assert_true(g_queue_is_empty(&self->pending));
  clear_record(self);
  for (guint i = 0; i < GROTTO_PANELS_N_MODELS; i++) g_clear_object(&self->models[i]);
  G_OBJECT_CLASS(grotto_panels_fake_service_parent_class)->dispose(object);
}
static void grotto_panels_fake_service_class_init(GrottoPanelsFakeServiceClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose;
}
static void grotto_panels_fake_service_init(GrottoPanelsFakeService *self) {
  for (guint i = 0; i < GROTTO_PANELS_N_MODELS; i++) self->models[i] = g_list_store_new(GROTTO_TYPE_PANELS_ITEM);
  for (guint i = 0; i < GROTTO_PANEL_N_COMMANDS; i++) self->failures[i] = -1;
}
GrottoPanelsFakeService *grotto_panels_fake_service_new(void) { return g_object_new(GROTTO_TYPE_PANELS_FAKE_SERVICE, NULL); }
void grotto_panels_fake_service_set_available(GrottoPanelsFakeService *self, gboolean value) {
  self->available = value;
  if (!value) {
    while (!g_queue_is_empty(&self->pending)) {
      GTask *task = g_queue_pop_head(&self->pending);
      Pending *p = g_task_get_task_data(task); p->failure = G_IO_ERROR_CANCELLED;
      finish_task(task);
    }
    for (guint i = 0; i < GROTTO_PANELS_N_MODELS; i++) g_list_store_remove_all(self->models[i]);
    for (guint i = 0; i < GROTTO_PANEL_N_COMMANDS; i++) self->caps[i] = FALSE;
    g_signal_emit_by_name(self, "capabilities-changed");
  }
  g_signal_emit_by_name(self, "availability-changed");
}
void grotto_panels_fake_service_set_can(GrottoPanelsFakeService *self, GrottoPanelCommand command, gboolean value) {
  g_return_if_fail(command >= 0 && command < GROTTO_PANEL_N_COMMANDS);
  self->caps[command] = value;
  g_signal_emit_by_name(self, "capabilities-changed");
}
gboolean grotto_panels_fake_service_replace(GrottoPanelsFakeService *self, GrottoPanelsModel model,
  const GrottoPanelsMetadata *metadata, GError **error) {
  g_return_val_if_fail(model >= 0 && model < GROTTO_PANELS_N_MODELS, FALSE);
  g_autoptr(GrottoPanelsItem) item = metadata ? grotto_panels_item_new(metadata, error) : NULL;
  if (metadata && !item) return FALSE;
  gpointer items[] = {item};
  g_list_store_splice(self->models[model], 0, g_list_model_get_n_items(G_LIST_MODEL(self->models[model])), items, item ? 1 : 0);
  return TRUE;
}
void grotto_panels_fake_service_set_failure(GrottoPanelsFakeService *self, GrottoPanelCommand command, gint code) {
  g_return_if_fail(command >= 0 && command < GROTTO_PANEL_N_COMMANDS);
  self->failures[command] = code;
}
void grotto_panels_fake_service_set_deferred(GrottoPanelsFakeService *self, gboolean deferred) { self->deferred = deferred; }
gboolean grotto_panels_fake_service_complete_next(GrottoPanelsFakeService *self) {
  GTask *task = g_queue_pop_head(&self->pending);
  if (!task) return FALSE;
  finish_task(task); return TRUE;
}
guint grotto_panels_fake_service_get_pending(GrottoPanelsFakeService *self) { return self->pending.length; }
guint grotto_panels_fake_service_get_invocations(GrottoPanelsFakeService *self) { return self->invocations; }
GrottoPanelCommand grotto_panels_fake_service_get_last_command(GrottoPanelsFakeService *self) { return self->last_command; }
const GrottoPanelCommandArgs *grotto_panels_fake_service_get_last_args(GrottoPanelsFakeService *self) { return &self->last_args; }
