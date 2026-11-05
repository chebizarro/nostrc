/* SPDX-License-Identifier: MIT */
#include "grotto-panels.h"
#include "grotto-profile-view.h"
#include <glib/gi18n-lib.h>
#include <string.h>

struct _GrottoIdentityPanel {
  GrottoPanel parent_instance;
  AdwNavigationView *navigation;
  AdwHeaderBar *header;
  GtkStack *content;
  AdwStatusPage *empty;
  AdwBanner *banner;
  GtkBox *profile_box;
  GrottoProfileView *profile;
  AdwEntryRow *name_row;
  AdwActionRow *key_row, *backup_row, *apps_row, *default_row, *details_row;
  AdwPreferencesGroup *other_group, *add_group, *advanced_group;
  GtkListBox *other_list;
  GtkButton *copy_key, *show_qr, *set_default, *add_identity, *empty_add, *remove_identity;
  GtkImage *default_check;
  GtkLabel *identity_count;
  GListModel *model;
  gulong model_changed;
  char *pinned, *identity;
  gboolean busy, disposed;
  guint generation;
};
G_DEFINE_TYPE(GrottoIdentityPanel, grotto_identity_panel, GROTTO_TYPE_PANEL)
static void render(GrottoIdentityPanel *self);
static void host(GrottoIdentityPanel *self, GrottoPanelHostAction action, const char *value) {
  grotto_panel_host_invoke(GROTTO_PANEL(self), action, value, NULL);
}
static void add(GtkButton *button, gpointer data) { (void)button; host(data, GROTTO_PANEL_HOST_ADD_IDENTITY, NULL); }
static void backup(GtkWidget *widget, gpointer data) {
  (void)widget; GrottoIdentityPanel *self = data;
  if (self->identity) host(self, GROTTO_PANEL_HOST_BACKUP, self->identity);
}
static void remove_identity(GtkButton *button, gpointer data) {
  (void)button; GrottoIdentityPanel *self = data;
  if (self->identity) host(self, GROTTO_PANEL_HOST_REMOVE, self->identity);
}
static void copy_key(GtkButton *button, gpointer data) {
  (void)button; GrottoIdentityPanel *self = data;
  if (self->identity) gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), self->identity);
}
static void open_target(GrottoIdentityPanel *self, const char *prefix) {
  if (!self->identity) return;
  g_autofree char *target = g_strconcat(prefix, self->identity, NULL);
  host(self, GROTTO_PANEL_HOST_OPEN_SIGNER, target);
}
static void qr(GtkButton *button, gpointer data) { (void)button; open_target(data, "public-key:"); }
static void apps(AdwActionRow *row, gpointer data) { (void)row; open_target(data, "apps:"); }

/* A child forwards through a weak parent, never a raw borrowed callback table. */
typedef struct { GWeakRef panel; } Forward;
static void forward_free(gpointer data) { Forward *f = data; g_weak_ref_clear(&f->panel); g_free(f); }
static void forward(Forward *f, GrottoPanelHostAction action, const char *value) {
  g_autoptr(GrottoPanel) panel = g_weak_ref_get(&f->panel);
  if (panel) grotto_panel_host_invoke(panel, action, value, NULL);
}
static void forward_toast(gpointer f, const char *v) { forward(f, GROTTO_PANEL_HOST_TOAST, v); }
static void forward_backup(gpointer f, const char *v) { forward(f, GROTTO_PANEL_HOST_BACKUP, v); }
static void forward_remove(gpointer f, const char *v) { forward(f, GROTTO_PANEL_HOST_REMOVE, v); }
static void forward_open(gpointer f, const char *v) { forward(f, GROTTO_PANEL_HOST_OPEN_SIGNER, v); }
static void forward_profile(gpointer data, GrottoProfileAction action, const char *key,
  const GrottoProfileInfo *info, GCancellable *cancel, GAsyncReadyCallback callback, gpointer user_data) {
  Forward *f = data; g_autoptr(GrottoPanel) panel = g_weak_ref_get(&f->panel);
  if (panel) grotto_panel_profile_async(panel, action, key, info, cancel, callback, user_data);
  else { GTask *task = g_task_new(NULL, cancel, callback, user_data);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Host closed"); g_object_unref(task); }
}
static void push_identity(GrottoIdentityPanel *self, const char *id) {
  GrottoPanel *child = GROTTO_PANEL(grotto_identity_panel_new(grotto_panel_get_service(GROTTO_PANEL(self)), id));
  gboolean controls;
  g_object_get(self, "show-window-controls", &controls, NULL);
  g_object_set(child, "show-window-controls", controls, NULL);
  GrottoPanelsHost callbacks = {0};
  if (grotto_panel_host_can(GROTTO_PANEL(self), GROTTO_PANEL_HOST_TOAST)) callbacks.show_toast = forward_toast;
  if (grotto_panel_host_can(GROTTO_PANEL(self), GROTTO_PANEL_HOST_BACKUP)) callbacks.open_backup = forward_backup;
  if (grotto_panel_host_can(GROTTO_PANEL(self), GROTTO_PANEL_HOST_REMOVE)) callbacks.remove_identity = forward_remove;
  if (grotto_panel_host_can(GROTTO_PANEL(self), GROTTO_PANEL_HOST_OPEN_SIGNER)) callbacks.open_in_signer = forward_open;
  if (grotto_panel_has_profile_host(GROTTO_PANEL(self))) callbacks.profile_async = forward_profile;
  Forward *f = g_new0(Forward, 1); g_weak_ref_init(&f->panel, self);
  grotto_panel_set_host(child, &callbacks, f, forward_free);
  adw_navigation_view_push(self->navigation, ADW_NAVIGATION_PAGE(child));
}
static void other_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
  (void)box; push_identity(data, g_object_get_data(G_OBJECT(row), "identity"));
}
static void details(AdwActionRow *row, gpointer data) {
  (void)row; GrottoIdentityPanel *self = data; if (self->identity) push_identity(self, self->identity);
}
typedef struct { GWeakRef panel; guint generation; GrottoPanelsService *service; } Edit;
static void edited(GObject *source, GAsyncResult *result, gpointer data) {
  Edit *edit = data; g_autoptr(GrottoIdentityPanel) self = g_weak_ref_get(&edit->panel);
  g_autoptr(GError) error = NULL;
  gboolean ok = grotto_panels_service_invoke_finish(GROTTO_PANELS_SERVICE(source), result, &error);
  if (self && !self->disposed && edit->generation == self->generation &&
      edit->service == grotto_panel_get_service(GROTTO_PANEL(self))) {
    self->busy = FALSE; render(self);
    host(self, GROTTO_PANEL_HOST_TOAST, ok ? _("Identity updated") : _("Identity wasn't updated. Try again"));
  }
  g_object_unref(edit->service); g_weak_ref_clear(&edit->panel); g_free(edit);
}
static void invoke(GrottoIdentityPanel *self, GrottoPanelCommand command) {
  GrottoPanelsService *service = grotto_panel_get_service(GROTTO_PANEL(self));
  if (self->busy || !self->identity || !grotto_panels_service_can(service, command)) return;
  GrottoPanelCommandArgs args = {.public_key = self->identity};
  if (command == GROTTO_PANEL_SET_IDENTITY_LABEL) args.label = gtk_editable_get_text(GTK_EDITABLE(self->name_row));
  Edit *edit = g_new0(Edit, 1); g_weak_ref_init(&edit->panel, self); edit->generation = self->generation; edit->service = g_object_ref(service);
  self->busy = TRUE;
  /* Finish the entry's focus transition before making its subtree insensitive
   * (GTK 4.14 otherwise loses GtkText's focus-out notification). */
  gtk_widget_grab_focus(GTK_WIDGET(self->copy_key));
  grotto_panels_service_invoke_async(service, command, &args, grotto_panel_get_cancellable(GROTTO_PANEL(self)), edited, edit);
  gtk_widget_set_sensitive(GTK_WIDGET(self->name_row), FALSE); gtk_widget_set_sensitive(GTK_WIDGET(self->set_default), FALSE);
}
static void label_apply(AdwEntryRow *row, gpointer data) { (void)row; invoke(data, GROTTO_PANEL_SET_IDENTITY_LABEL); }
static void make_default(GtkButton *button, gpointer data) { (void)button; invoke(data, GROTTO_PANEL_SET_DEFAULT_IDENTITY); }
static void render(GrottoIdentityPanel *self) {
  GrottoPanel *panel = GROTTO_PANEL(self);
  GrottoPanelsService *service = grotto_panel_get_service(panel);
  gboolean available = grotto_panels_service_is_available(service);
  g_autoptr(GrottoPanelsItem) selected = NULL;
  guint n = self->model ? g_list_model_get_n_items(self->model) : 0;
  for (guint i = 0; i < n; i++) {
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(self->model, i);
    const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(item);
    if ((self->pinned && !strcmp(self->pinned, m->public_key)) || (!self->pinned && (!selected || m->is_default)))
      g_set_object(&selected, item);
  }
  const GrottoPanelsMetadata *m = selected ? grotto_panels_item_get_metadata(selected) : NULL;
  if (g_strcmp0(self->identity, m ? m->public_key : NULL)) { self->generation++; self->busy = FALSE; }
  g_free(self->identity); self->identity = m ? g_strdup(m->public_key) : NULL;
  gtk_stack_set_visible_child_name(self->content, m ? "identity" : "empty");
  adw_status_page_set_title(self->empty, !available ? _("Signer Unavailable") : self->pinned ? _("Identity Unavailable") : _("No Identity Yet"));
  adw_status_page_set_description(self->empty, !available ? _("Open Grotto and try again") : self->pinned ? _("This identity is no longer on this device") : _("Create or import an identity to start signing"));
  gtk_widget_set_visible(GTK_WIDGET(self->empty_add), available && !self->pinned && grotto_panel_host_can(panel, GROTTO_PANEL_HOST_ADD_IDENTITY));
  adw_banner_set_title(self->banner, available ? _("This identity isn't backed up") : _("Signer unavailable"));
  adw_banner_set_revealed(self->banner, m && (!available || (!m->watch_only && !m->backup_acknowledged)));
  adw_banner_set_button_label(self->banner, available && m && m->can_sign && grotto_panel_host_can(panel, GROTTO_PANEL_HOST_BACKUP) ? _("Back Up…") : "");
  g_autofree char *identity_count = g_strdup_printf(g_dngettext(GETTEXT_PACKAGE,
      "%u identity on this device", "%u identities on this device", n), n);
  gtk_label_set_text(self->identity_count, identity_count);
  gtk_widget_set_visible(GTK_WIDGET(self->identity_count), !self->pinned);
  if (!m) {
    grotto_profile_view_set_identity(self->profile, NULL, NULL, FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->backup_row), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->advanced_group), FALSE);
    return;
  }
  g_autofree char *fallback = g_strdup_printf(_("Identity %.12s…%.6s"), m->public_key, m->public_key + MAX(0, (gint)strlen(m->public_key) - 6));
  grotto_profile_view_set_identity(self->profile, self->identity, *m->label ? m->label : fallback,
    available && m->can_sign && !m->watch_only);
  if (!self->busy && !(gtk_widget_get_state_flags(GTK_WIDGET(self->name_row)) & GTK_STATE_FLAG_FOCUS_WITHIN)) gtk_editable_set_text(GTK_EDITABLE(self->name_row), m->label);
  gboolean editable = !self->busy && grotto_panels_service_can(service, GROTTO_PANEL_SET_IDENTITY_LABEL);
  if (!editable && (gtk_widget_get_state_flags(GTK_WIDGET(self->name_row)) & GTK_STATE_FLAG_FOCUS_WITHIN))
    gtk_widget_grab_focus(GTK_WIDGET(self->copy_key));
  gtk_widget_set_sensitive(GTK_WIDGET(self->name_row), editable);
  adw_action_row_set_subtitle(self->key_row, m->public_key);
  gtk_widget_set_visible(GTK_WIDGET(self->show_qr), grotto_panel_host_can(panel, GROTTO_PANEL_HOST_OPEN_SIGNER));
  gtk_widget_set_visible(GTK_WIDGET(self->backup_row), !m->watch_only && m->can_sign && available && grotto_panel_host_can(panel, GROTTO_PANEL_HOST_BACKUP));
  adw_action_row_set_subtitle(self->backup_row, m->backup_acknowledged ? _("Backup acknowledged") : _("Not backed up"));
  gtk_widget_set_visible(GTK_WIDGET(self->apps_row), grotto_panel_host_can(panel, GROTTO_PANEL_HOST_OPEN_SIGNER));
  g_autofree char *count = g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "%u app uses this identity", "%u apps use this identity", m->app_count), m->app_count);
  adw_action_row_set_subtitle(self->apps_row, count);
  gtk_widget_set_visible(GTK_WIDGET(self->default_row), !m->watch_only && m->can_sign);
  adw_action_row_set_subtitle(self->default_row, m->is_default ? _("Apps that don't choose an identity use this one") : _("Used by apps that don't choose an identity"));
  gtk_widget_set_visible(GTK_WIDGET(self->default_check), m->is_default);
  gtk_widget_set_visible(GTK_WIDGET(self->set_default), !m->is_default);
  gtk_widget_set_sensitive(GTK_WIDGET(self->set_default), !self->busy && grotto_panels_service_can(service, GROTTO_PANEL_SET_DEFAULT_IDENTITY));
  gtk_widget_set_visible(GTK_WIDGET(self->details_row), self->pinned == NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->advanced_group), self->pinned && m->can_sign && available && grotto_panel_host_can(panel, GROTTO_PANEL_HOST_REMOVE));
  gtk_widget_set_visible(GTK_WIDGET(self->add_group), !self->pinned && grotto_panel_host_can(panel, GROTTO_PANEL_HOST_ADD_IDENTITY));
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->other_list)))) gtk_list_box_remove(self->other_list, child);
  for (guint i = 0; !self->pinned && i < n; i++) {
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(self->model, i);
    const GrottoPanelsMetadata *other = grotto_panels_item_get_metadata(item);
    if (!strcmp(other->public_key, m->public_key)) continue;
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), *other->label ? other->label : _("Identity"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), other->watch_only ? _("Watch only — cannot sign") : other->public_key);
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 1);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), adw_avatar_new(40, other->label, TRUE));
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
    g_object_set_data_full(G_OBJECT(row), "identity", g_strdup(other->public_key), g_free);
    gtk_list_box_append(self->other_list, row);
  }
  gtk_widget_set_visible(GTK_WIDGET(self->other_group), !self->pinned && n > 1);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->key_row), m->watch_only ? _("Public Key (Watch Only — Cannot Sign)") : _("Public Key"));
}
static void items_changed(GListModel *model, guint position, guint removed, guint added, gpointer data) {
  (void)model; (void)position; (void)removed; (void)added; render(data);
}
static void attach(GrottoPanel *panel, gpointer data) {
  (void)data; GrottoIdentityPanel *self = GROTTO_IDENTITY_PANEL(panel);
  if (self->disposed) return;
  GrottoPanelsService *service = grotto_panel_get_service(panel);
  GListModel *model = service ? grotto_panels_service_get_model(service, GROTTO_PANELS_IDENTITIES) : NULL;
  if (model != self->model) {
    if (self->model) g_signal_handler_disconnect(self->model, self->model_changed);
    g_set_object(&self->model, model); self->generation++; self->busy = FALSE;
    if (model) self->model_changed = g_signal_connect(model, "items-changed", G_CALLBACK(items_changed), self);
  }
  render(self);
}
static void dispose(GObject *object) {
  GrottoIdentityPanel *self = GROTTO_IDENTITY_PANEL(object);
  self->disposed = TRUE; self->generation++;
  if (self->model) g_signal_handler_disconnect(self->model, self->model_changed);
  g_clear_object(&self->model); g_clear_pointer(&self->identity, g_free); g_clear_pointer(&self->pinned, g_free);
  /* The inherited AdwNavigationPage owns child too. Detach it before template
   * refs are dropped, so the 1.5 parent never unparents a finalized child. */
  adw_navigation_page_set_child(ADW_NAVIGATION_PAGE(self), NULL);
  gtk_widget_dispose_template(GTK_WIDGET(self), GROTTO_TYPE_IDENTITY_PANEL);
  G_OBJECT_CLASS(grotto_identity_panel_parent_class)->dispose(object);
}
static void grotto_identity_panel_class_init(GrottoIdentityPanelClass *klass) {
  bindtextdomain(GETTEXT_PACKAGE, GROTTO_PANELS_LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  G_OBJECT_CLASS(klass)->dispose = dispose;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, "/org/nostr/GrottoPanels/identity-panel.ui");
#define BIND(name) gtk_widget_class_bind_template_child(wc, GrottoIdentityPanel, name)
  BIND(header); BIND(navigation); BIND(content); BIND(empty); BIND(banner); BIND(profile_box); BIND(identity_count); BIND(name_row); BIND(key_row);
  BIND(backup_row); BIND(apps_row); BIND(default_row); BIND(details_row); BIND(other_group); BIND(other_list);
  BIND(add_group); BIND(advanced_group); BIND(copy_key); BIND(show_qr); BIND(set_default); BIND(default_check);
  BIND(add_identity); BIND(empty_add); BIND(remove_identity);
#undef BIND
}
static void grotto_identity_panel_init(GrottoIdentityPanel *self) {
  gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(gdk_display_get_default()), "/org/nostr/GrottoPanels/icons");
  gtk_widget_init_template(GTK_WIDGET(self));
  self->profile = GROTTO_PROFILE_VIEW(grotto_profile_view_new(GROTTO_PANEL(self)));
  gtk_box_append(self->profile_box, GTK_WIDGET(self->profile));
  g_object_bind_property(self, "show-window-controls", self->header, "show-start-title-buttons", G_BINDING_SYNC_CREATE);
  g_object_bind_property(self, "show-window-controls", self->header, "show-end-title-buttons", G_BINDING_SYNC_CREATE);
#define CONNECT(widget, signal, cb) g_signal_connect(self->widget, signal, G_CALLBACK(cb), self)
  CONNECT(add_identity, "clicked", add); CONNECT(empty_add, "clicked", add); CONNECT(banner, "button-clicked", backup);
  CONNECT(backup_row, "activated", backup); CONNECT(remove_identity, "clicked", remove_identity);
  CONNECT(copy_key, "clicked", copy_key); CONNECT(show_qr, "clicked", qr); CONNECT(apps_row, "activated", apps);
  CONNECT(other_list, "row-activated", other_activated); CONNECT(details_row, "activated", details);
  CONNECT(name_row, "apply", label_apply); CONNECT(set_default, "clicked", make_default);
#undef CONNECT
  g_signal_connect(self, "service-changed", G_CALLBACK(attach), NULL);
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(self->key_row), FALSE);
  attach(GROTTO_PANEL(self), NULL);
}
GtkWidget *grotto_identity_panel_new(GrottoPanelsService *service, const char *public_key) {
  g_return_val_if_fail(grotto_panels_public_text_validate(public_key, NULL), NULL);
  GrottoIdentityPanel *self = g_object_new(GROTTO_TYPE_IDENTITY_PANEL, NULL);
  self->pinned = g_strdup(public_key);
  grotto_panel_set_service(GROTTO_PANEL(self), service);
  return GTK_WIDGET(self);
}
AdwNavigationView *grotto_identity_panel_get_navigation(GrottoIdentityPanel *self) { return self->navigation; }
