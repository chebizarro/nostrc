/* SPDX-License-Identifier: MIT */
#include "grotto-panel-private.h"

typedef struct {
  GrottoPanelsService *service;
  GCancellable *cancellable;
  GrottoPanelsHost host;
  gpointer host_data;
  GDestroyNotify host_destroy;
  gulong capabilities_id, availability_id;
  gboolean disposing;
  gboolean show_window_controls;
} GrottoPanelPrivate;
G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(GrottoPanel, grotto_panel, ADW_TYPE_NAVIGATION_PAGE)
enum { PROP_0, PROP_SERVICE, PROP_SHOW_WINDOW_CONTROLS, N_PROPS };
static GParamSpec *properties[N_PROPS];
static guint changed_signal;

static void service_changed(GrottoPanelsService *service, GrottoPanel *self) {
  (void)service;
  g_signal_emit(self, changed_signal, 0);
}
void grotto_panel_set_service(GrottoPanel *self, GrottoPanelsService *service) {
  g_return_if_fail(GROTTO_IS_PANEL(self));
  g_return_if_fail(service == NULL || GROTTO_PANELS_IS_SERVICE(service));
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  if (p->service == service) return;
  if (p->service) {
    g_signal_handler_disconnect(p->service, p->capabilities_id);
    g_signal_handler_disconnect(p->service, p->availability_id);
  }
  g_cancellable_cancel(p->cancellable);
  g_clear_object(&p->cancellable);
  p->cancellable = g_cancellable_new();
  g_set_object(&p->service, service);
  if (service) {
    p->capabilities_id = g_signal_connect(service, "capabilities-changed", G_CALLBACK(service_changed), self);
    p->availability_id = g_signal_connect(service, "availability-changed", G_CALLBACK(service_changed), self);
  }
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SERVICE]);
  g_signal_emit(self, changed_signal, 0);
}
GrottoPanelsService *grotto_panel_get_service(GrottoPanel *self) {
  g_return_val_if_fail(GROTTO_IS_PANEL(self), NULL);
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  return p->service;
}
GCancellable *grotto_panel_get_cancellable(GrottoPanel *self) {
  g_return_val_if_fail(GROTTO_IS_PANEL(self), NULL);
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  return p->cancellable;
}
void grotto_panel_set_host(GrottoPanel *self, const GrottoPanelsHost *host,
                           gpointer data, GDestroyNotify destroy) {
  g_return_if_fail(GROTTO_IS_PANEL(self));
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  if (p->host_destroy) p->host_destroy(p->host_data);
  p->host = host ? *host : (GrottoPanelsHost){0};
  p->host_data = data;
  p->host_destroy = destroy;
  if (!p->disposing) g_signal_emit(self, changed_signal, 0);
}
gboolean grotto_panel_host_can(GrottoPanel *self, GrottoPanelHostAction action) {
  g_return_val_if_fail(GROTTO_IS_PANEL(self), FALSE);
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  switch (action) {
    case GROTTO_PANEL_HOST_TOAST: return p->host.show_toast != NULL;
    case GROTTO_PANEL_HOST_REVIEW: return p->host.review_request != NULL;
    case GROTTO_PANEL_HOST_ADD_IDENTITY: return p->host.add_identity != NULL;
    case GROTTO_PANEL_HOST_ADD_APP: return p->host.add_app != NULL;
    case GROTTO_PANEL_HOST_BACKUP: return p->host.open_backup != NULL;
    case GROTTO_PANEL_HOST_REMOVE: return p->host.remove_identity != NULL;
    case GROTTO_PANEL_HOST_REPLACE: return p->host.replace_identity != NULL;
    case GROTTO_PANEL_HOST_OPEN_SIGNER: return p->host.open_in_signer != NULL;
    default: return FALSE;
  }
}
gboolean grotto_panel_host_invoke(GrottoPanel *self, GrottoPanelHostAction action,
  const char *value, GError **error) {
  g_return_val_if_fail(GROTTO_IS_PANEL(self), FALSE);
  if (!grotto_panels_public_text_validate(value, error)) return FALSE;
  gboolean empty = action == GROTTO_PANEL_HOST_ADD_IDENTITY || action == GROTTO_PANEL_HOST_ADD_APP;
  if ((empty && value) || (!empty && (!value || !*value))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid public host target");
    return FALSE;
  }
  if (!grotto_panel_host_can(self, action)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Host action unavailable");
    return FALSE;
  }
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  switch (action) {
    case GROTTO_PANEL_HOST_TOAST: p->host.show_toast(p->host_data, value); break;
    case GROTTO_PANEL_HOST_REVIEW: p->host.review_request(p->host_data, value); break;
    case GROTTO_PANEL_HOST_ADD_IDENTITY: p->host.add_identity(p->host_data); break;
    case GROTTO_PANEL_HOST_ADD_APP: p->host.add_app(p->host_data); break;
    case GROTTO_PANEL_HOST_BACKUP: p->host.open_backup(p->host_data, value); break;
    case GROTTO_PANEL_HOST_REMOVE: p->host.remove_identity(p->host_data, value); break;
    case GROTTO_PANEL_HOST_REPLACE: p->host.replace_identity(p->host_data, value); break;
    case GROTTO_PANEL_HOST_OPEN_SIGNER: p->host.open_in_signer(p->host_data, value); break;
    default: g_assert_not_reached();
  }
  return TRUE;
}
gboolean grotto_panel_can_command(GrottoPanel *self, GrottoPanelCommand command) {
  g_return_val_if_fail(GROTTO_IS_PANEL(self), FALSE);
  if (command < 0 || command >= GROTTO_PANEL_N_COMMANDS) return FALSE;
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  if (!grotto_panels_service_is_available(p->service)) return FALSE;
  if (grotto_panel_command_get_access(command) == GROTTO_PANELS_ACCESS_MANAGER)
    return grotto_panels_service_can(p->service, command);
  return p->host.can_command && p->host.command_async && p->host.can_command(p->host_data, command);
}
static void command_done(GObject *source, GAsyncResult *result, gpointer data) {
  GTask *task = data; GrottoPanel *self = g_task_get_source_object(task);
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  g_autoptr(GError) error = NULL;
  gboolean ok = GROTTO_PANELS_IS_SERVICE(source) ?
    grotto_panels_service_invoke_finish(GROTTO_PANELS_SERVICE(source), result, &error) :
    G_IS_TASK(result) ? g_task_propagate_boolean(G_TASK(result), &error) : FALSE;
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  if (p->service != g_task_get_task_data(task))
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Panel detached");
  else if (error) g_task_return_error(task, g_steal_pointer(&error));
  else if (ok) g_task_return_boolean(task, TRUE);
  else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Command failed");
  g_object_unref(task);
}
void grotto_panel_forward_command_async(GrottoPanel *self, GrottoPanelCommand command,
  const GrottoPanelCommandArgs *args, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data) {
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  GTask *task = g_task_new(self, cancellable, callback, data);
  g_autoptr(GError) error = NULL;
  const GrottoPanelCommandArgs empty = {0}; if (!args) args = &empty;
  if (!grotto_panel_command_args_validate(command, args, &error)) g_task_return_error(task, g_steal_pointer(&error));
  else if (!grotto_panel_can_command(self, command))
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Open in Grotto");
  else {
    g_task_set_task_data(task, g_object_ref(p->service), g_object_unref);
    if (grotto_panel_command_get_access(command) == GROTTO_PANELS_ACCESS_MANAGER)
      grotto_panels_service_invoke_async(p->service, command, args, cancellable, command_done, task);
    else p->host.command_async(p->host_data, command, args, cancellable, command_done, task);
    return;
  }
  g_object_unref(task);
}
void grotto_panel_command_async(GrottoPanel *self, GrottoPanelCommand command,
  const GrottoPanelCommandArgs *args, GAsyncReadyCallback callback, gpointer data) {
  g_return_if_fail(GROTTO_IS_PANEL(self));
  grotto_panel_forward_command_async(self, command, args, grotto_panel_get_cancellable(self), callback, data);
}
gboolean grotto_panel_command_finish(GrottoPanel *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
gboolean grotto_panel_has_profile_host(GrottoPanel *self) {
  g_return_val_if_fail(GROTTO_IS_PANEL(self), FALSE);
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  return p->host.profile_async != NULL;
}
static void profile_done(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source;
  GTask *task = data;
  GrottoPanel *self = g_task_get_source_object(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GrottoProfileInfo) info = G_IS_TASK(result) ? g_task_propagate_pointer(G_TASK(result), &error) : NULL;
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  if (grotto_panel_get_service(self) != g_task_get_task_data(task))
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Panel detached");
  else if (error) g_task_return_error(task, g_steal_pointer(&error));
  else if (!info || !grotto_profile_info_validate(info, &error))
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid public profile");
  else {
    for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) if (!info->fields[i]) info->fields[i] = g_strdup("");
    g_task_return_pointer(task, g_steal_pointer(&info), (GDestroyNotify)grotto_profile_info_free);
  }
  g_object_unref(task);
}
void grotto_panel_profile_async(GrottoPanel *self, GrottoProfileAction action,
  const char *key, const GrottoProfileInfo *info, GCancellable *cancel,
  GAsyncReadyCallback callback, gpointer data) {
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  GTask *task = g_task_new(self, cancel, callback, data);
  g_autoptr(GError) error = NULL;
  if (action < GROTTO_PROFILE_FETCH || action > GROTTO_PROFILE_VERIFY_NIP05 ||
      (action != GROTTO_PROFILE_FETCH && !info) || !key || !*key ||
      !grotto_panels_public_text_validate(key, &error) || !grotto_profile_info_validate(info, &error))
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid profile request");
  else if (!p->host.profile_async)
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Open in Grotto");
  else {
    if (p->service) g_task_set_task_data(task, g_object_ref(p->service), g_object_unref);
    p->host.profile_async(p->host_data, action, key, info, cancel, profile_done, task);
    return;
  }
  g_object_unref(task);
}
GrottoProfileInfo *grotto_panel_profile_finish(GrottoPanel *self, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec) {
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(GROTTO_PANEL(object));
  if (id == PROP_SERVICE) grotto_panel_set_service(GROTTO_PANEL(object), g_value_get_object(value));
  else if (id == PROP_SHOW_WINDOW_CONTROLS) p->show_window_controls = g_value_get_boolean(value);
  else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec) {
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(GROTTO_PANEL(object));
  if (id == PROP_SERVICE) g_value_set_object(value, grotto_panel_get_service(GROTTO_PANEL(object)));
  else if (id == PROP_SHOW_WINDOW_CONTROLS) g_value_set_boolean(value, p->show_window_controls);
  else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void dispose(GObject *object) {
  GrottoPanel *self = GROTTO_PANEL(object);
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  p->disposing = TRUE;
  if (p->service) {
    g_signal_handler_disconnect(p->service, p->capabilities_id);
    g_signal_handler_disconnect(p->service, p->availability_id);
    g_clear_object(&p->service);
  }
  if (p->cancellable) g_cancellable_cancel(p->cancellable);
  g_clear_object(&p->cancellable);
  grotto_panel_set_host(self, NULL, NULL, NULL);
  G_OBJECT_CLASS(grotto_panel_parent_class)->dispose(object);
}
static void panel_unmap(GtkWidget *widget) {
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(GROTTO_PANEL(widget));
  if (p->cancellable) g_cancellable_cancel(p->cancellable);
  GTK_WIDGET_CLASS(grotto_panel_parent_class)->unmap(widget);
}
static void panel_map(GtkWidget *widget) {
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(GROTTO_PANEL(widget));
  if (p->cancellable && g_cancellable_is_cancelled(p->cancellable)) {
    g_clear_object(&p->cancellable); p->cancellable = g_cancellable_new();
  }
  GTK_WIDGET_CLASS(grotto_panel_parent_class)->map(widget);
}
void grotto_panel_bind_header_bar(GrottoPanel *self, AdwHeaderBar *header) {
  g_object_bind_property(self, "show-window-controls", header, "show-start-title-buttons", G_BINDING_SYNC_CREATE);
  g_object_bind_property(self, "show-window-controls", header, "show-end-title-buttons", G_BINDING_SYNC_CREATE);
}
static void bind_headers(GrottoPanel *self, GtkWidget *widget) {
  if (widget != GTK_WIDGET(self) && GROTTO_IS_PANEL(widget)) return;
  if (ADW_IS_HEADER_BAR(widget)) grotto_panel_bind_header_bar(self, ADW_HEADER_BAR(widget));
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    bind_headers(self, child);
}
static void constructed(GObject *object) {
  G_OBJECT_CLASS(grotto_panel_parent_class)->constructed(object);
  bind_headers(GROTTO_PANEL(object), GTK_WIDGET(object));
}
static void grotto_panel_class_init(GrottoPanelClass *klass) {
  GTK_WIDGET_CLASS(klass)->unmap = panel_unmap;
  GTK_WIDGET_CLASS(klass)->map = panel_map;
  GObjectClass *object = G_OBJECT_CLASS(klass);
  object->set_property = set_property;
  object->get_property = get_property;
  object->dispose = dispose;
  object->constructed = constructed;
  properties[PROP_SERVICE] = g_param_spec_object("service", NULL, NULL, GROTTO_TYPE_PANELS_SERVICE,
    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  properties[PROP_SHOW_WINDOW_CONTROLS] = g_param_spec_boolean("show-window-controls", NULL, NULL, TRUE,
    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object, N_PROPS, properties);
  changed_signal = g_signal_new("service-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void grotto_panel_init(GrottoPanel *self) {
  grotto_panels_app_info_prepare();
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  p->cancellable = g_cancellable_new();
  p->show_window_controls = TRUE;
}
GtkWidget *grotto_panel_create_metadata_row(GrottoPanelsItem *item) {
  const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(item);
  g_return_val_if_fail(m != NULL, NULL);
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), m->label);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), m->reason);
  return row;
}
