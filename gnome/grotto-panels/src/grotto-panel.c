/* SPDX-License-Identifier: MIT */
#include "grotto-panels.h"

typedef struct {
  GrottoPanelsService *service;
  GCancellable *cancellable;
  GrottoPanelsHost host;
  gpointer host_data;
  GDestroyNotify host_destroy;
  gulong capabilities_id, availability_id;
  gboolean disposing;
} GrottoPanelPrivate;
G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(GrottoPanel, grotto_panel, ADW_TYPE_NAVIGATION_PAGE)
enum { PROP_0, PROP_SERVICE, N_PROPS };
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
static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec) {
  if (id == PROP_SERVICE) grotto_panel_set_service(GROTTO_PANEL(object), g_value_get_object(value));
  else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec) {
  if (id == PROP_SERVICE) g_value_set_object(value, grotto_panel_get_service(GROTTO_PANEL(object)));
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
static void grotto_panel_class_init(GrottoPanelClass *klass) {
  GObjectClass *object = G_OBJECT_CLASS(klass);
  object->set_property = set_property;
  object->get_property = get_property;
  object->dispose = dispose;
  properties[PROP_SERVICE] = g_param_spec_object("service", NULL, NULL, GROTTO_TYPE_PANELS_SERVICE,
    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object, N_PROPS, properties);
  changed_signal = g_signal_new("service-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void grotto_panel_init(GrottoPanel *self) {
  GrottoPanelPrivate *p = grotto_panel_get_instance_private(self);
  p->cancellable = g_cancellable_new();
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
