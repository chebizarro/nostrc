/* SPDX-License-Identifier: MIT */
#include "grotto-panels.h"

struct _GrottoPanelsItem {
  GObject parent_instance;
  GrottoPanelsMetadata metadata;
};
G_DEFINE_TYPE(GrottoPanelsItem, grotto_panels_item, G_TYPE_OBJECT)

static void item_finalize(GObject *object) {
  GrottoPanelsMetadata *m = &GROTTO_PANELS_ITEM(object)->metadata;
  g_free((char *)m->id);
  g_free((char *)m->label);
  g_free((char *)m->public_key);
  g_free((char *)m->state);
  g_free((char *)m->reason);
  g_free((char *)m->access_policy);
  G_OBJECT_CLASS(grotto_panels_item_parent_class)->finalize(object);
}
static void grotto_panels_item_class_init(GrottoPanelsItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = item_finalize;
}
static void grotto_panels_item_init(GrottoPanelsItem *self) { (void)self; }
GrottoPanelsItem *grotto_panels_item_new(const GrottoPanelsMetadata *metadata) {
  g_return_val_if_fail(metadata != NULL, NULL);
  GrottoPanelsItem *self = g_object_new(GROTTO_TYPE_PANELS_ITEM, NULL);
  self->metadata = *metadata;
#define COPY(field) self->metadata.field = g_strdup(metadata->field ? metadata->field : "")
  COPY(id); COPY(label); COPY(public_key); COPY(state); COPY(reason); COPY(access_policy);
#undef COPY
  return self;
}
const GrottoPanelsMetadata *grotto_panels_item_get_metadata(GrottoPanelsItem *self) {
  g_return_val_if_fail(GROTTO_IS_PANELS_ITEM(self), NULL);
  return &self->metadata;
}

const GVariantType *grotto_panel_command_get_args_type(GrottoPanelCommand command) {
  static const char *const types[] = {
    "(ss)", "(s)", "(ss)", "(ssaa{sv}t)", "(s)", "()", "()", "()", "(basas)", "()"
  };
  return command >= 0 && command < GROTTO_PANEL_N_COMMANDS ? G_VARIANT_TYPE(types[command]) : NULL;
}
GrottoPanelsAccess grotto_panel_command_get_access(GrottoPanelCommand command) {
  if (command == GROTTO_PANEL_PAUSE) return GROTTO_PANELS_ACCESS_SESSION;
  if (command == GROTTO_PANEL_SET_CLIENT_LABEL) return GROTTO_PANELS_ACCESS_MANAGER;
  return GROTTO_PANELS_ACCESS_APPROVER;
}

G_DEFINE_INTERFACE(GrottoPanelsService, grotto_panels_service, G_TYPE_OBJECT)
static void grotto_panels_service_default_init(GrottoPanelsServiceInterface *iface) {
  g_signal_new("capabilities-changed", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST,
               0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  g_signal_new("availability-changed", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST,
               0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
GListModel *grotto_panels_service_get_model(GrottoPanelsService *self, GrottoPanelsModel model) {
  g_return_val_if_fail(GROTTO_PANELS_IS_SERVICE(self), NULL);
  g_return_val_if_fail(model >= 0 && model < GROTTO_PANELS_N_MODELS, NULL);
  GrottoPanelsServiceInterface *iface = GROTTO_PANELS_SERVICE_GET_IFACE(self);
  return iface->get_model ? iface->get_model(self, model) : NULL;
}
gboolean grotto_panels_service_is_available(GrottoPanelsService *self) {
  if (!self) return FALSE;
  g_return_val_if_fail(GROTTO_PANELS_IS_SERVICE(self), FALSE);
  GrottoPanelsServiceInterface *iface = GROTTO_PANELS_SERVICE_GET_IFACE(self);
  return iface->is_available && iface->is_available(self);
}
gboolean grotto_panels_service_can(GrottoPanelsService *self, GrottoPanelCommand command) {
  if (!grotto_panel_command_get_args_type(command) || !grotto_panels_service_is_available(self)) return FALSE;
  GrottoPanelsServiceInterface *iface = GROTTO_PANELS_SERVICE_GET_IFACE(self);
  return iface->can && iface->can(self, command);
}
void grotto_panels_service_invoke_async(GrottoPanelsService *self, GrottoPanelCommand command,
  GVariant *args, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
  g_return_if_fail(self == NULL || GROTTO_PANELS_IS_SERVICE(self));
  GrottoPanelsServiceInterface *iface = self ? GROTTO_PANELS_SERVICE_GET_IFACE(self) : NULL;
  const GVariantType *type = grotto_panel_command_get_args_type(command);
  GIOErrorEnum code = G_IO_ERROR_NOT_SUPPORTED;
  const char *message = "Unsupported panel command";
  if (type) {
    if (!args || !g_variant_is_of_type(args, type)) {
      code = G_IO_ERROR_INVALID_ARGUMENT;
      message = "Invalid panel command arguments";
    } else if (!grotto_panels_service_is_available(self)) {
      code = G_IO_ERROR_NOT_INITIALIZED;
      message = "Signer unavailable";
    } else if (!grotto_panels_service_can(self, command)) {
      code = G_IO_ERROR_PERMISSION_DENIED;
      message = "Open in Grotto";
    } else if (iface->invoke_async && iface->invoke_finish) {
      iface->invoke_async(self, command, args, cancellable, callback, user_data);
      return;
    }
  }
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, grotto_panels_service_invoke_async);
  g_task_return_new_error(task, G_IO_ERROR, code, "%s", message);
  g_object_unref(task);
}
gboolean grotto_panels_service_invoke_finish(GrottoPanelsService *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(self == NULL || GROTTO_PANELS_IS_SERVICE(self), FALSE);
  if (g_async_result_is_tagged(result, grotto_panels_service_invoke_async))
    return g_task_propagate_boolean(G_TASK(result), error);
  g_return_val_if_fail(self != NULL, FALSE);
  GrottoPanelsServiceInterface *iface = GROTTO_PANELS_SERVICE_GET_IFACE(self);
  g_return_val_if_fail(iface->invoke_finish != NULL, FALSE);
  return iface->invoke_finish(self, result, error);
}
