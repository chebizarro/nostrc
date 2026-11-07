/* SPDX-License-Identifier: MIT */
#include "grotto-panels.h"
#include "grotto-app-detail.h"
#include <glib/gi18n-lib.h>

struct _GrottoAppsPanel {
  GrottoPanel parent_instance;
  AdwNavigationView *navigation;
  AdwPreferencesPage *preferences;
  AdwPreferencesGroup *pending_group, *apps_group, *controls, *advanced, *add_group;
  AdwBanner *unavailable;
  AdwSwitchRow *pause_row;
  AdwActionRow *ask_defaults;
  GtkListBox *apps_list, *pending_list;
  GtkButton *add_app;
  GListModel *apps, *requests, *status;
  GtkWidget *host_pending;
  char *identity, *return_principal;
  gboolean updating, busy;
};
G_DEFINE_TYPE(GrottoAppsPanel, grotto_apps_panel, GROTTO_TYPE_PANEL)

const GrottoPanelDescriptor *grotto_panels_get_descriptors(gsize *length) {
  static const GrottoPanelDescriptor panels[] = {
    {"identity", N_("Identity"), "avatar-default-symbolic"},
    {"apps", N_("Connected Apps"), "application-x-executable-symbolic"},
    {"activity", N_("Activity"), "document-open-recent-symbolic"},
    {"remote-signing", N_("Remote Signing"), "network-server-symbolic"}
  };
  if (length) *length = G_N_ELEMENTS(panels);
  return panels;
}
static void clear_list(GtkListBox *list) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(list)))) gtk_list_box_remove(list, child);
}
static void restore_app_focus(AdwNavigationPage *page, gpointer data) {
  (void)page; GrottoAppsPanel *self = data;
  /* List snapshots can replace the originating widget while detail is open. */
  if (!self->return_principal) return;
  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(self->apps_list)); row; row = gtk_widget_get_next_sibling(row)) {
    if (!g_strcmp0(g_object_get_data(G_OBJECT(row), "principal"), self->return_principal)) {
      gtk_widget_grab_focus(row); break;
    }
  }
}
static void open_app(AdwActionRow *row, gpointer data) {
  grotto_apps_panel_show_app(data, g_object_get_data(G_OBJECT(row), "principal"));
}
static void review(AdwActionRow *row, gpointer data) {
  grotto_panel_host_invoke(data, GROTTO_PANEL_HOST_REVIEW, g_object_get_data(G_OBJECT(row), "request"), NULL);
}
static void render(GrottoAppsPanel *self) {
  GrottoPanel *panel = GROTTO_PANEL(self);
  GrottoPanelsService *service = grotto_panel_get_service(panel);
  self->updating = TRUE;
  adw_banner_set_revealed(self->unavailable, !grotto_panels_service_is_available(service));
  clear_list(self->apps_list); clear_list(self->pending_list);
  for (guint i = 0; self->apps && i < g_list_model_get_n_items(self->apps); i++) {
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(self->apps, i);
    const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(item);
    if (self->identity && g_strcmp0(self->identity, m->public_key)) continue;
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    g_autoptr(GIcon) icon = NULL;
    g_autofree char *name = grotto_panels_app_display_info(m->id, m->label, &icon);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_gicon(icon));
    const char *transport = !strcmp(m->transport, "NIP46") ? _("Remote") : _("Local App");
    const char *access = m->blocked ? _("Blocked") : !strcmp(m->access_policy, "Previous access policy") ? _("Previous access policy") :
      !strcmp(m->trust, "full") ? _("Routine Operations") : !strcmp(m->trust, "medium") ? _("Ask for Sensitive Operations") : _("Ask Unless Permitted");
    g_autoptr(GDateTime) date = m->last_used_at ? g_date_time_new_from_unix_local(m->last_used_at) : NULL;
    g_autofree char *when = date ? g_date_time_format(date, "%x") : NULL;
    g_autofree char *used = when ? g_strdup_printf(_("Used %s"), when) : g_strdup(_("Not Used Yet"));
    g_autofree char *connection = !strcmp(m->transport, "NIP46") && m->relay && *m->relay ?
      g_strdup_printf("%s · %s", transport, m->relay) : g_strdup(transport);
    g_autofree char *subtitle = g_strdup_printf(_("%s\n%s · %s · %s"), m->id, connection, access, used);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), !strcmp(m->transport, "NIP46") ? 0 : 2);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data_full(G_OBJECT(row), "principal", g_strdup(m->id), g_free);
    g_signal_connect(row, "activated", G_CALLBACK(open_app), self);
    gtk_list_box_append(self->apps_list, row);
  }
  for (guint i = 0; !self->host_pending && self->requests && i < g_list_model_get_n_items(self->requests); i++) {
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(self->requests, i);
    const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(item);
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    g_autoptr(GIcon) icon = NULL;
    g_autofree char *name = grotto_panels_app_display_info(m->principal, m->label, &icon);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_gicon(icon));
    g_autofree char *title = g_strdup_printf("%s · %s", name, m->operation);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    gint64 seconds = MAX((gint64)0, (m->deadline_usec - g_get_monotonic_time()) / G_USEC_PER_SEC);
    g_autofree char *remaining = g_strdup_printf(_("%" G_GINT64_FORMAT " seconds remaining"), seconds);
    g_autofree char *subtitle = g_strdup_printf(_("%s\n%s"), m->principal, remaining);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), grotto_panel_host_can(panel, GROTTO_PANEL_HOST_REVIEW));
    g_object_set_data_full(G_OBJECT(row), "request", g_strdup(m->id), g_free);
    g_signal_connect(row, "activated", G_CALLBACK(review), self); gtk_list_box_append(self->pending_list, row);
  }
  gtk_widget_set_visible(GTK_WIDGET(self->pending_group), !self->host_pending && self->requests && g_list_model_get_n_items(self->requests));
  g_autoptr(GrottoPanelsItem) status = self->status && g_list_model_get_n_items(self->status) ? g_list_model_get_item(self->status, 0) : NULL;
  gboolean paused = status && grotto_panels_item_get_metadata(status)->paused;
  adw_switch_row_set_active(self->pause_row, paused);
  gtk_widget_set_sensitive(GTK_WIDGET(self->pause_row), status && !self->busy &&
    grotto_panel_can_command(panel, paused ? GROTTO_PANEL_RESUME : GROTTO_PANEL_PAUSE));
  gtk_widget_set_visible(GTK_WIDGET(self->add_app), grotto_panel_host_can(panel, GROTTO_PANEL_HOST_ADD_APP));
  gtk_widget_set_visible(GTK_WIDGET(self->advanced), grotto_panel_host_can(panel, GROTTO_PANEL_HOST_OPEN_SIGNER));
  self->updating = FALSE;
}
static void items_changed(GListModel *model, guint position, guint removed, guint added, gpointer data) {
  (void)model; (void)position; (void)removed; (void)added; render(data);
}
static void attach(GrottoPanel *panel, gpointer data) {
  (void)data; GrottoAppsPanel *self = GROTTO_APPS_PANEL(panel);
  GListModel **slots[] = {&self->apps, &self->requests, &self->status};
  GrottoPanelsModel kinds[] = {GROTTO_PANELS_APPS, GROTTO_PANELS_REQUESTS, GROTTO_PANELS_STATUS};
  GrottoPanelsService *service = grotto_panel_get_service(panel);
  for (guint i = 0; i < G_N_ELEMENTS(slots); i++) {
    GListModel *model = service ? grotto_panels_service_get_model(service, kinds[i]) : NULL;
    if (*slots[i] == model) continue;
    if (*slots[i]) g_signal_handlers_disconnect_by_data(*slots[i], self);
    g_set_object(slots[i], model);
    if (model) g_signal_connect(model, "items-changed", G_CALLBACK(items_changed), self);
  }
  render(self);
}
static void pause_done(GObject *source, GAsyncResult *result, gpointer data) {
  GrottoAppsPanel *self = data;
  g_autoptr(GError) error = NULL;
  gboolean ok = grotto_panel_command_finish(GROTTO_PANEL(source), result, &error);
  self->busy = FALSE; render(self);
  if (!ok) {
    adw_banner_set_title(self->unavailable, _("Signing state wasn't changed. Review the current state and retry."));
    adw_banner_set_revealed(self->unavailable, TRUE);
  }
  g_object_unref(self);
}
static void resume_chosen(GObject *source, GAsyncResult *result, gpointer data) {
  GrottoAppsPanel *self = data;
  if (!strcmp(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result), "resume"))
    grotto_panel_command_async(GROTTO_PANEL(self), GROTTO_PANEL_RESUME, NULL, pause_done, g_object_ref(self));
  else { self->busy = FALSE; render(self); }
  g_object_unref(self);
}
static void pause_changed(GObject *object, GParamSpec *pspec, gpointer data) {
  (void)object; (void)pspec; GrottoAppsPanel *self = data;
  if (self->updating || self->busy) return;
  self->busy = TRUE;
  if (adw_switch_row_get_active(self->pause_row))
    grotto_panel_command_async(GROTTO_PANEL(self), GROTTO_PANEL_PAUSE, NULL, pause_done, g_object_ref(self));
  else {
    AdwDialog *dialog = adw_alert_dialog_new(_("Resume Signing?"), _("Previously authorized apps can sign again. Saved permissions still apply."));
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"), "resume", _("Resume Signing"), NULL);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(self), grotto_panel_get_cancellable(GROTTO_PANEL(self)), resume_chosen, g_object_ref(self));
  }
  render(self);
}
static void add_app(GtkButton *button, gpointer data) {
  (void)button; grotto_panel_host_invoke(data, GROTTO_PANEL_HOST_ADD_APP, NULL, NULL);
}
static void ask_defaults(AdwActionRow *row, gpointer data) {
  (void)row; grotto_panel_host_invoke(data, GROTTO_PANEL_HOST_OPEN_SIGNER, "ask-defaults", NULL);
}
static void dispose(GObject *object) {
  GrottoAppsPanel *self = GROTTO_APPS_PANEL(object);
  GListModel **models[] = {&self->apps, &self->requests, &self->status};
  for (guint i = 0; i < G_N_ELEMENTS(models); i++) {
    if (*models[i]) g_signal_handlers_disconnect_by_data(*models[i], self);
    g_clear_object(models[i]);
  }
  adw_navigation_page_set_child(ADW_NAVIGATION_PAGE(self), NULL);
  gtk_widget_dispose_template(GTK_WIDGET(self), GROTTO_TYPE_APPS_PANEL);
  G_OBJECT_CLASS(grotto_apps_panel_parent_class)->dispose(object);
}
static void finalize(GObject *object) {
  g_free(GROTTO_APPS_PANEL(object)->identity);
  g_free(GROTTO_APPS_PANEL(object)->return_principal);
  G_OBJECT_CLASS(grotto_apps_panel_parent_class)->finalize(object);
}
static void grotto_apps_panel_class_init(GrottoAppsPanelClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass); oc->dispose = dispose; oc->finalize = finalize;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, "/org/nostr/GrottoPanels/apps-panel.ui");
#define BIND(x) gtk_widget_class_bind_template_child(wc, GrottoAppsPanel, x)
  BIND(navigation); BIND(preferences); BIND(pending_group); BIND(apps_group); BIND(controls); BIND(advanced); BIND(add_group);
  BIND(unavailable); BIND(pause_row); BIND(ask_defaults); BIND(apps_list); BIND(pending_list); BIND(add_app);
#undef BIND
}
static void grotto_apps_panel_init(GrottoAppsPanel *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  g_signal_connect(adw_navigation_view_get_visible_page(self->navigation), "shown", G_CALLBACK(restore_app_focus), self);
  g_signal_connect(self, "service-changed", G_CALLBACK(attach), NULL);
  g_signal_connect(self->pause_row, "notify::active", G_CALLBACK(pause_changed), self);
  g_signal_connect(self->add_app, "clicked", G_CALLBACK(add_app), self);
  g_signal_connect(self->ask_defaults, "activated", G_CALLBACK(ask_defaults), self);
}
GtkWidget *grotto_apps_panel_new(GrottoPanelsService *service) {
  return g_object_new(GROTTO_TYPE_APPS_PANEL, "service", service, NULL);
}
void grotto_apps_panel_show_app(GrottoAppsPanel *self, const char *principal) {
  g_return_if_fail(GROTTO_IS_APPS_PANEL(self));
  if (!principal || !*principal) {
    adw_navigation_view_pop_to_tag(self->navigation, "apps-root");
    g_clear_pointer(&self->identity, g_free); render(self);
    return;
  }
  if (!grotto_panels_public_text_validate(principal, NULL)) return;
  g_free(self->return_principal); self->return_principal = g_strdup(principal);
  adw_navigation_view_push(self->navigation, ADW_NAVIGATION_PAGE(grotto_app_detail_new(GROTTO_PANEL(self), principal)));
}
AdwNavigationView *grotto_apps_panel_get_navigation(GrottoAppsPanel *self) { return self->navigation; }
void grotto_apps_panel_filter_identity(GrottoAppsPanel *self, const char *key) {
  if (!grotto_panels_public_text_validate(key, NULL)) return;
  g_free(self->identity); self->identity = key && *key ? g_strdup(key) : NULL; render(self);
}
void grotto_apps_panel_set_pending_group(GrottoAppsPanel *self, GtkWidget *group) {
  g_return_if_fail(ADW_IS_PREFERENCES_GROUP(group));
  if (self->host_pending) adw_preferences_page_remove(self->preferences, ADW_PREFERENCES_GROUP(self->host_pending));
  self->host_pending = group;
  /* PreferencesPage has no insert API at the floor: keep pending first. */
  GList *groups = NULL;
  GtkWidget *children[] = {GTK_WIDGET(self->pending_group), GTK_WIDGET(self->controls), GTK_WIDGET(self->apps_group), GTK_WIDGET(self->add_group), GTK_WIDGET(self->advanced)};
  for (guint i = 0; i < G_N_ELEMENTS(children); i++) {
    g_object_ref(children[i]); adw_preferences_page_remove(self->preferences, ADW_PREFERENCES_GROUP(children[i]));
    groups = g_list_append(groups, children[i]);
  }
  adw_preferences_page_add(self->preferences, ADW_PREFERENCES_GROUP(group));
  for (GList *l = groups; l; l = l->next) {
    adw_preferences_page_add(self->preferences, ADW_PREFERENCES_GROUP(l->data)); g_object_unref(l->data);
  }
  g_list_free(groups); render(self);
}
