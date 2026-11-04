#include "page-settings.h"
#include "signer-window.h"
#include "app-resources.h"
#include "approval_dialog.h"
#include "../settings_manager.h"
#include "../session-manager.h"
#include "../i18n.h"
#include <signer_permission_policy.h>

struct _PageSettings {
  AdwPreferencesPage parent_instance;
  AdwPreferencesGroup *security, *management;
  AdwActionRow *remote;
  AdwSpinRow *retention;
  AdwComboRow *lock_timeout;
  AdwSwitchRow *lock_idle, *pause_lock;
  GSettings *settings;
  GrottoHomeModel *model;
};
G_DEFINE_TYPE(PageSettings, page_settings, ADW_TYPE_PREFERENCES_PAGE)
static const gint lock_values[] = {0, 60, 300, 900, 1800, 3600};
static void combo_changed(GObject *object, GParamSpec *pspec, gpointer data) {
  (void)pspec; PageSettings *self = data; guint index = adw_combo_row_get_selected(ADW_COMBO_ROW(object));
  if (object == G_OBJECT(self->lock_timeout) && index < G_N_ELEMENTS(lock_values)) {
    gn_session_manager_set_timeout(gn_session_manager_get_default(), lock_values[index]);
    g_settings_set_int(self->settings, "lock-timeout-sec", lock_values[index]);
  }
}
static void retention_changed(AdwSpinRow *row, GParamSpec *pspec, gpointer data) {
  (void)pspec; PageSettings *self = data; g_settings_set_int(self->settings, "log-retention-days", (gint)adw_spin_row_get_value(row));
}
void page_settings_show_retention(PageSettings *self, GtkWidget *parent) {
  AdwDialog *dialog = adw_preferences_dialog_new(); adw_dialog_set_title(dialog, _("Activity Retention"));
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_description(group, _("Metadata only — no messages, secrets or event content"));
  AdwSpinRow *row = ADW_SPIN_ROW(adw_spin_row_new_with_range(1, 30, 1));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Keep Activity (Days)"));
  adw_spin_row_set_value(row, CLAMP(g_settings_get_int(self->settings, "log-retention-days"), 1, 30));
  g_signal_connect_object(row, "notify::value", G_CALLBACK(retention_changed), self, 0);
  adw_preferences_group_add(group, GTK_WIDGET(row)); adw_preferences_page_add(page, group);
  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), page); adw_dialog_present(dialog, parent);
}
typedef struct { GSettings *settings; AdwDialog *dialog; GtkEntry *custom; GtkLabel *error; GPtrArray *checks; } Policy;
static void policy_free(gpointer data) { Policy *p = data; g_object_unref(p->settings); g_ptr_array_unref(p->checks); g_free(p); }
static void policy_save(GtkButton *button, gpointer data) {
  (void)button; Policy *p = data; g_autoptr(GPtrArray) values = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < p->checks->len; i++) {
    GtkCheckButton *check = p->checks->pdata[i];
    if (gtk_check_button_get_active(check)) g_ptr_array_add(values, g_strdup(g_object_get_data(G_OBJECT(check), "kind")));
  }
  g_auto(GStrv) custom = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(p->custom)), ", \t\n", -1);
  for (guint i = 0; custom[i]; i++) {
    if (!*custom[i]) continue;
    gchar *end = NULL; guint64 kind = g_ascii_strtoull(custom[i], &end, 10);
    if (end == custom[i] || *end || kind > G_MAXUINT16 || *custom[i] == '-') {
      gtk_label_set_text(p->error, _("Use whole event-type numbers from 0 to 65535, separated by commas.")); return;
    }
    g_ptr_array_add(values, g_strdup_printf("%u", (guint)kind));
  }
  g_ptr_array_add(values, NULL);
  if (!g_settings_set_strv(p->settings, "require-confirmation-kinds", (const gchar *const *)values->pdata)) {
    gtk_label_set_text(p->error, _("The ask-by-default policy could not be saved.")); return;
  }
  adw_dialog_close(p->dialog);
}
static void policy_default(GtkButton *button, gpointer data) {
  (void)button; Policy *p = data; g_settings_reset(p->settings, "require-confirmation-kinds"); adw_dialog_close(p->dialog);
}
void page_settings_show_ask_defaults(PageSettings *self, GtkWidget *parent) {
  Policy *p = g_new0(Policy, 1); p->settings = g_object_ref(self->settings); p->checks = g_ptr_array_new();
  p->dialog = adw_dialog_new(); adw_dialog_set_title(p->dialog, _("Ask-by-Default Event Types")); adw_dialog_set_content_width(p->dialog, 400);
  GtkWidget *toolbar = adw_toolbar_view_new(); adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  GtkWidget *scroll = gtk_scrolled_window_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18); gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
  g_autoptr(GVariant) configured = g_settings_get_user_value(self->settings, "require-confirmation-kinds");
  g_auto(GStrv) values = configured ? g_variant_dup_strv(configured, NULL) : NULL;
  GString *others = g_string_new("");
  for (guint i = 0; i < G_N_ELEMENTS(signer_default_ask_by_default_kinds); i++) {
    guint32 kind = signer_default_ask_by_default_kinds[i]; g_autofree gchar *number = g_strdup_printf("%u", kind);
    GtkWidget *check = gtk_check_button_new_with_label(gnostr_approval_kind_name(kind));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), !configured || g_strv_contains((const gchar *const *)values, number));
    g_object_set_data_full(G_OBJECT(check), "kind", g_strdup(number), g_free); gtk_box_append(GTK_BOX(box), check); g_ptr_array_add(p->checks, check);
  }
  for (guint i = 0; values && values[i]; i++) {
    gboolean known = FALSE;
    for (guint j = 0; j < p->checks->len; j++) if (!strcmp(values[i], g_object_get_data(G_OBJECT(p->checks->pdata[j]), "kind"))) known = TRUE;
    if (!known) { if (others->len) g_string_append(others, ", "); g_string_append(others, values[i]); }
  }
  gtk_box_append(GTK_BOX(box), gtk_label_new(_("Additional custom event types (comma-separated)"))); p->custom = GTK_ENTRY(gtk_entry_new());
  gtk_editable_set_text(GTK_EDITABLE(p->custom), others->str); g_string_free(others, TRUE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(p->custom), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Additional custom event types"), -1); gtk_box_append(GTK_BOX(box), GTK_WIDGET(p->custom));
  p->error = GTK_LABEL(gtk_label_new(NULL)); gtk_label_set_wrap(p->error, TRUE); gtk_box_append(GTK_BOX(box), GTK_WIDGET(p->error));
  GtkWidget *save = gtk_button_new_with_label(_("Save Ask-by-Default Policy")); g_signal_connect(save, "clicked", G_CALLBACK(policy_save), p); gtk_box_append(GTK_BOX(box), save);
  GtkWidget *defaults = gtk_button_new_with_label(_("Restore Defaults")); g_signal_connect(defaults, "clicked", G_CALLBACK(policy_default), p); gtk_box_append(GTK_BOX(box), defaults);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box); adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
  g_object_set_data_full(G_OBJECT(p->dialog), "policy", p, policy_free); adw_dialog_set_child(p->dialog, toolbar); adw_dialog_present(p->dialog, parent);
}
void page_settings_add_background_group(PageSettings *self, AdwPreferencesGroup *group) {
  AdwPreferencesGroup *groups[] = {self->security, self->management};
  for (guint i = 0; i < G_N_ELEMENTS(groups); i++) { g_object_ref(groups[i]); adw_preferences_page_remove(ADW_PREFERENCES_PAGE(self), groups[i]); }
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), group);
  for (guint i = 0; i < G_N_ELEMENTS(groups); i++) { adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), groups[i]); g_object_unref(groups[i]); }
}
void page_settings_set_model(PageSettings *self, GrottoHomeModel *model) {
  g_set_object(&self->model, model);
}
static void dispose(GObject *object) {
  PageSettings *self = PAGE_SETTINGS(object); if (self->model) g_signal_handlers_disconnect_by_data(self->model, self);
  g_clear_object(&self->model); g_clear_object(&self->settings); gtk_widget_dispose_template(GTK_WIDGET(self), TYPE_PAGE_SETTINGS);
  G_OBJECT_CLASS(page_settings_parent_class)->dispose(object);
}
static void page_settings_class_init(PageSettingsClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose; GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-settings.ui");
#define BIND(x) gtk_widget_class_bind_template_child(wc, PageSettings, x)
  BIND(security); BIND(management); BIND(remote); BIND(retention); BIND(lock_timeout); BIND(lock_idle); BIND(pause_lock);
#undef BIND
}
static void open_remote(AdwActionRow *row, gpointer data) {
  (void)row; PageSettings *self = data; GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  if (!SIGNER_IS_WINDOW(root)) return;
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(root));
  if (dialog) adw_dialog_close(dialog);
  signer_window_show_page(SIGNER_WINDOW(root), "remote-signing");
}
static void page_settings_init(PageSettings *self) {
  gtk_widget_init_template(GTK_WIDGET(self)); self->settings = g_settings_new("org.nostr.Grotto");
  gint timeout = g_settings_get_int(self->settings, "lock-timeout-sec");
  for (guint i = 0; i < G_N_ELEMENTS(lock_values); i++) if (timeout == lock_values[i]) adw_combo_row_set_selected(self->lock_timeout, i);
  g_settings_bind(self->settings, "session-lock-on-idle", self->lock_idle, "active", G_SETTINGS_BIND_DEFAULT);
  g_settings_bind(self->settings, "pause-on-screen-lock", self->pause_lock, "active", G_SETTINGS_BIND_DEFAULT);
  g_signal_connect(self->lock_timeout, "notify::selected", G_CALLBACK(combo_changed), self);
  g_settings_bind(self->settings, "log-retention-days", self->retention, "value", G_SETTINGS_BIND_DEFAULT);
  g_signal_connect(self->remote, "activated", G_CALLBACK(open_remote), self);
}
PageSettings *page_settings_new(void) { return g_object_new(TYPE_PAGE_SETTINGS, NULL); }
