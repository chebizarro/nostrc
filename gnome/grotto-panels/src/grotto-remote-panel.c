/* SPDX-License-Identifier: MIT */
#include "grotto-panels.h"
#include <glib/gi18n-lib.h>

struct _GrottoRemotePanel {
  GrottoPanel parent_instance;
  AdwBanner *feedback;
  AdwSwitchRow *enabled;
  AdwActionRow *status, *reset, *handoff, *pairing, *connection_link;
  AdwPreferencesGroup *connection_group;
  GtkButton *copy_link;
  gchar *connection_uri;
  GdkContentProvider *clipboard_content;
  AdwEntryRow *new_relay, *methods;
  GtkListBox *relays_list;
  GListModel *model;
  GrottoPanelsItem *snapshot;
  gboolean updating, busy, adding_relay;
};
G_DEFINE_TYPE(GrottoRemotePanel, grotto_remote_panel, GROTTO_TYPE_PANEL)
static void render(GrottoRemotePanel *self);
static const char *state_label(const char *state) {
  const char *values[] = {"DISABLED", "STARTING", "LISTENING", "DEGRADED", "OFFLINE", "STOPPING", "ERROR"};
  const char *labels[] = {N_("Disabled"), N_("Starting"), N_("Ready"), N_("Connection interrupted"), N_("Offline"), N_("Stopping"), N_("Error")};
  for (guint i = 0; i < G_N_ELEMENTS(values); i++) if (!g_strcmp0(values[i], state)) return _(labels[i]);
  return _("Unavailable");
}
static void finished(GObject *source, GAsyncResult *result, gpointer data) {
  GrottoRemotePanel *self = data; g_autoptr(GError) error = NULL;
  gboolean ok = grotto_panel_command_finish(GROTTO_PANEL(source), result, &error);
  self->busy = FALSE;
  if (ok && self->adding_relay) gtk_editable_set_text(GTK_EDITABLE(self->new_relay), "");
  self->adding_relay = FALSE;
  render(self);
  if (!ok) {
    adw_banner_set_title(self->feedback, _("Remote signing wasn't changed. Check the configuration and retry."));
    adw_banner_set_revealed(self->feedback, TRUE);
  }
  g_object_unref(self);
}
static void configure(GrottoRemotePanel *self, gboolean enabled, const char *const *relays, const char *const *methods) {
  if (self->busy || !self->snapshot) return;
  GrottoPanelCommandArgs args = {.remote_enabled = enabled, .relays = relays, .methods = methods,
    .expected_generation = grotto_panels_item_get_metadata(self->snapshot)->owner_generation};
  self->busy = TRUE;
  grotto_panel_command_async(GROTTO_PANEL(self), GROTTO_PANEL_CONFIGURE_REMOTE, &args, finished, g_object_ref(self));
  render(self);
}
static void enabled_changed(GObject *object, GParamSpec *pspec, gpointer data) {
  (void)object; (void)pspec; GrottoRemotePanel *self = data;
  if (self->updating || !self->snapshot) return;
  const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(self->snapshot);
  configure(self, adw_switch_row_get_active(self->enabled), m->relays, m->methods);
}
static void remove_relay(GtkButton *button, gpointer data) {
  GrottoRemotePanel *self = data; if (!self->snapshot) return;
  const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(self->snapshot);
  const char *remove = g_object_get_data(G_OBJECT(button), "relay");
  g_autoptr(GPtrArray) relays = g_ptr_array_new();
  for (guint i = 0; m->relays && m->relays[i]; i++) if (g_strcmp0(remove, m->relays[i])) g_ptr_array_add(relays, (gpointer)m->relays[i]);
  g_ptr_array_add(relays, NULL);
  configure(self, m->remote_enabled, (const char *const *)relays->pdata, m->methods);
}
static void add_relay(AdwEntryRow *row, gpointer data) {
  GrottoRemotePanel *self = data; if (!self->snapshot) return;
  const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(self->snapshot);
  g_autofree char *url = g_strdup(gtk_editable_get_text(GTK_EDITABLE(row)));
  g_strstrip(url);
  const char *candidate[] = {url, NULL};
  GrottoPanelCommandArgs check = {.relays = candidate};
  if (!grotto_panel_command_args_validate(GROTTO_PANEL_CONFIGURE_REMOTE, &check, NULL)) {
    adw_banner_set_title(self->feedback, _("Use wss://, or ws:// on loopback. Remove credentials, query parameters, fragments and .onion hosts."));
    adw_banner_set_revealed(self->feedback, TRUE);
    return;
  }
  g_autoptr(GPtrArray) relays = g_ptr_array_new();
  for (guint i = 0; m->relays && m->relays[i]; i++) {
    if (!strcmp(url, m->relays[i])) return;
    g_ptr_array_add(relays, (gpointer)m->relays[i]);
  }
  g_ptr_array_add(relays, (gpointer)url); g_ptr_array_add(relays, NULL);
  self->adding_relay = TRUE;
  configure(self, m->remote_enabled, (const char *const *)relays->pdata, m->methods);
}
static void methods_apply(AdwEntryRow *row, gpointer data) {
  GrottoRemotePanel *self = data; if (!self->snapshot) return;
  const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(self->snapshot);
  g_auto(GStrv) input = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(row)), ", ", -1);
  g_autoptr(GPtrArray) methods = g_ptr_array_new();
  for (guint i = 0; input[i]; i++) if (*input[i]) g_ptr_array_add(methods, input[i]);
  g_ptr_array_add(methods, NULL);
  configure(self, m->remote_enabled, m->relays, (const char *const *)methods->pdata);
}
static void reset_confirmed(GObject *source, GAsyncResult *result, gpointer data) {
  GrottoRemotePanel *self = data;
  if (!strcmp(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result), "reset")) {
    self->busy = TRUE;
    grotto_panel_command_async(GROTTO_PANEL(self), GROTTO_PANEL_RESET_REMOTE, NULL, finished, g_object_ref(self));
    render(self);
  }
  g_object_unref(self);
}
static void reset(AdwActionRow *row, gpointer data) {
  (void)row; GrottoRemotePanel *self = data;
  AdwDialog *dialog = adw_alert_dialog_new(_("Reset Remote Connections?"), _("Remote pairings and links will stop working. Your identities and local app permissions are kept."));
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"), "reset", _("Reset Connections"), NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "reset", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(self), grotto_panel_get_cancellable(GROTTO_PANEL(self)), reset_confirmed, g_object_ref(self));
}
static void handoff(AdwActionRow *row, gpointer data) {
  (void)row; grotto_panel_host_invoke(data, GROTTO_PANEL_HOST_OPEN_SIGNER, "remote-signing", NULL);
}
static void pairing(AdwActionRow *row, gpointer data) {
  (void)row; g_signal_emit_by_name(data, "show-connection-link");
}
static void clear_clipboard(GrottoRemotePanel *self) {
  if (!self->clipboard_content) return;
  GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
  if (gdk_clipboard_get_content(clipboard) == self->clipboard_content) gdk_clipboard_set_content(clipboard, NULL);
  g_clear_object(&self->clipboard_content);
}
void grotto_remote_panel_set_connection_link(GrottoRemotePanel *self, const char *uri) {
  g_return_if_fail(GROTTO_IS_REMOTE_PANEL(self));
  if (uri && (!g_str_has_prefix(uri, "bunker://") || strlen(uri) > 65536 || !g_utf8_validate(uri, -1, NULL))) uri = NULL;
  if (g_strcmp0(self->connection_uri, uri)) clear_clipboard(self);
  g_free(self->connection_uri); self->connection_uri = g_strdup(uri);
  adw_action_row_set_subtitle(self->connection_link, uri ? uri : _("Open Show QR Code to choose an identity or check the connection"));
  gtk_widget_set_sensitive(GTK_WIDGET(self->copy_link), uri != NULL);
}
static void copy_link(GtkButton *button, gpointer data) {
  (void)button; GrottoRemotePanel *self = data;
  if (!self->connection_uri) return;
  clear_clipboard(self);
  self->clipboard_content = gdk_content_provider_new_typed(G_TYPE_STRING, self->connection_uri);
  gdk_clipboard_set_content(gtk_widget_get_clipboard(GTK_WIDGET(self)), self->clipboard_content);
}
static void request_connection_link(GtkWidget *widget, gpointer data) {
  (void)data; GrottoRemotePanel *self = GROTTO_REMOTE_PANEL(widget);
  const GrottoPanelsMetadata *m = self->snapshot ? grotto_panels_item_get_metadata(self->snapshot) : NULL;
  if (m && m->remote_enabled && !g_strcmp0(m->state, "LISTENING") &&
      grotto_panel_can_command(GROTTO_PANEL(self), GROTTO_PANEL_CONFIGURE_REMOTE))
    g_signal_emit_by_name(self, "connection-link-requested");
}
static void unmap_connection(GtkWidget *widget, gpointer data) {
  (void)data; grotto_remote_panel_set_connection_link(GROTTO_REMOTE_PANEL(widget), NULL);
}
static void render(GrottoRemotePanel *self) {
  self->updating = TRUE;
  g_clear_object(&self->snapshot);
  if (self->model && g_list_model_get_n_items(self->model)) self->snapshot = g_list_model_get_item(self->model, 0);
  const GrottoPanelsMetadata *m = self->snapshot ? grotto_panels_item_get_metadata(self->snapshot) : NULL;
  gboolean can = m && !self->busy && grotto_panel_can_command(GROTTO_PANEL(self), GROTTO_PANEL_CONFIGURE_REMOTE);
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
  gboolean editing_methods = focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(self->methods));
  gboolean move_focus = focus && (gtk_widget_is_ancestor(focus, GTK_WIDGET(self->relays_list)) ||
    (!can && (editing_methods || gtk_widget_is_ancestor(focus, GTK_WIDGET(self->new_relay)))));
  if (move_focus) gtk_root_set_focus(root, NULL);
  adw_banner_set_title(self->feedback, _("Signer Unavailable"));
  adw_banner_set_revealed(self->feedback, !m);
  adw_action_row_set_subtitle(self->status, m && m->reason && *m->reason ? m->reason : state_label(m ? m->state : NULL));
  gboolean ready = m && m->remote_enabled && !g_strcmp0(m->state, "LISTENING");
  if (!ready) grotto_remote_panel_set_connection_link(self, NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->connection_link), m && m->remote_enabled);
  gtk_widget_set_visible(GTK_WIDGET(self->pairing), m && m->remote_enabled);
  gtk_widget_set_sensitive(GTK_WIDGET(self->pairing), can && g_signal_has_handler_pending(self,
    g_signal_lookup("show-connection-link", GROTTO_TYPE_REMOTE_PANEL), 0, TRUE));
  adw_preferences_group_set_description(self->connection_group, ready ?
    _("Paste or scan this link in a remote app; single-use links are replaced after an app connects.") :
    m && m->remote_enabled ? _("The connection link will appear when remote signing is ready.") :
    _("Enable remote signing to produce a link that an app can paste or scan to connect."));
  adw_switch_row_set_active(self->enabled, m && m->remote_enabled);
  gtk_widget_set_sensitive(GTK_WIDGET(self->enabled), can);
  gtk_widget_set_sensitive(GTK_WIDGET(self->new_relay), can);
  gtk_widget_set_sensitive(GTK_WIDGET(self->methods), can);
  gtk_widget_set_sensitive(GTK_WIDGET(self->reset), m && !self->busy && grotto_panel_can_command(GROTTO_PANEL(self), GROTTO_PANEL_RESET_REMOTE));
  gtk_widget_set_visible(GTK_WIDGET(self->handoff), !can && !self->busy && grotto_panel_host_can(GROTTO_PANEL(self), GROTTO_PANEL_HOST_OPEN_SIGNER));
  GtkWidget *child; while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->relays_list)))) gtk_list_box_remove(self->relays_list, child);
  for (guint i = 0; m && m->relays && m->relays[i]; i++) {
    GtkWidget *row = adw_action_row_new(); adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), m->relays[i]);
    GtkWidget *button = gtk_button_new_from_icon_name("edit-delete-symbolic");
    gtk_widget_set_tooltip_text(button, _("Remove Relay")); gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(button, "flat"); gtk_widget_set_sensitive(button, can);
    g_object_set_data_full(G_OBJECT(button), "relay", g_strdup(m->relays[i]), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(remove_relay), self);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), button); gtk_list_box_append(self->relays_list, row);
  }
  if (!editing_methods) {
    g_autofree char *methods = m && m->methods ? g_strjoinv(", ", (char **)m->methods) : g_strdup("");
    gtk_editable_set_text(GTK_EDITABLE(self->methods), methods);
  }
  self->updating = FALSE;
  if (move_focus) gtk_widget_child_focus(GTK_WIDGET(self), GTK_DIR_TAB_FORWARD);
  if (gtk_widget_get_mapped(GTK_WIDGET(self))) request_connection_link(GTK_WIDGET(self), NULL);
}
static void changed(GListModel *model, guint p, guint r, guint a, gpointer data) { (void)model; (void)p; (void)r; (void)a; render(data); }
static void attach(GrottoPanel *panel, gpointer data) {
  (void)data; GrottoRemotePanel *self = GROTTO_REMOTE_PANEL(panel);
  GrottoPanelsService *service = grotto_panel_get_service(panel);
  GListModel *model = service ? grotto_panels_service_get_model(service, GROTTO_PANELS_STATUS) : NULL;
  if (self->model != model) {
    if (self->model) g_signal_handlers_disconnect_by_data(self->model, self);
    g_set_object(&self->model, model);
    if (model) g_signal_connect(model, "items-changed", G_CALLBACK(changed), self);
  }
  render(self);
}
static void dispose(GObject *object) {
  GrottoRemotePanel *self = GROTTO_REMOTE_PANEL(object);
  if (self->model) g_signal_handlers_disconnect_by_data(self->model, self);
  g_clear_object(&self->model); g_clear_object(&self->snapshot);
  clear_clipboard(self); g_clear_pointer(&self->connection_uri, g_free);
  adw_navigation_page_set_child(ADW_NAVIGATION_PAGE(self), NULL);
  gtk_widget_dispose_template(GTK_WIDGET(self), GROTTO_TYPE_REMOTE_PANEL);
  G_OBJECT_CLASS(grotto_remote_panel_parent_class)->dispose(object);
}
static void grotto_remote_panel_class_init(GrottoRemotePanelClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose; GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  g_signal_new("connection-link-requested", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  g_signal_new("show-connection-link", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  gtk_widget_class_set_template_from_resource(wc, "/org/nostr/GrottoPanels/remote-panel.ui");
#define BIND(x) gtk_widget_class_bind_template_child(wc, GrottoRemotePanel, x)
  BIND(connection_group); BIND(connection_link); BIND(copy_link);
  BIND(feedback); BIND(enabled); BIND(status); BIND(pairing); BIND(reset); BIND(handoff); BIND(new_relay); BIND(methods); BIND(relays_list);
#undef BIND
}
static void grotto_remote_panel_init(GrottoRemotePanel *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  g_signal_connect(self, "map", G_CALLBACK(request_connection_link), NULL);
  g_signal_connect(self, "unmap", G_CALLBACK(unmap_connection), NULL);
  g_signal_connect(self->copy_link, "clicked", G_CALLBACK(copy_link), self);
  grotto_remote_panel_set_connection_link(self, NULL);
  g_signal_connect(self, "service-changed", G_CALLBACK(attach), NULL);
  g_signal_connect(self->enabled, "notify::active", G_CALLBACK(enabled_changed), self);
  g_signal_connect(self->new_relay, "apply", G_CALLBACK(add_relay), self);
  g_signal_connect(self->methods, "apply", G_CALLBACK(methods_apply), self);
  g_signal_connect(self->reset, "activated", G_CALLBACK(reset), self);
  g_signal_connect(self->handoff, "activated", G_CALLBACK(handoff), self);
  g_signal_connect(self->pairing, "activated", G_CALLBACK(pairing), self);
}
GtkWidget *grotto_remote_panel_new(GrottoPanelsService *service) { return g_object_new(GROTTO_TYPE_REMOTE_PANEL, "service", service, NULL); }
