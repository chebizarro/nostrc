#include "page-settings.h"
#include "signer-window.h"
#include "app-resources.h"
#include "dialog-actions.h"
#include "approval_dialog.h"
#include "../settings_manager.h"
#include "../session-manager.h"
#include "../i18n.h"
#include <signer_permission_policy.h>

struct _PageSettings {
  AdwPreferencesPage parent_instance;
  AdwPreferencesGroup *security, *management;
  AdwSpinRow *retention;
  AdwComboRow *lock_timeout;
  AdwSwitchRow *lock_idle, *pause_lock;
  AdwSwitchRow *bootstrap_enabled;
  GtkTextView *bootstrap_relays;
  GtkLabel *bootstrap_error;
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
static void retention_save(GtkButton *button, gpointer data) {
  PageSettings *self = data;
  AdwSpinRow *row = g_object_get_data(G_OBJECT(button), "retention-row");
  if (g_settings_set_int(self->settings, "log-retention-days", (gint)adw_spin_row_get_value(row)))
    adw_dialog_close(ADW_DIALOG(gtk_widget_get_ancestor(GTK_WIDGET(button), ADW_TYPE_DIALOG)));
}
void page_settings_show_retention(PageSettings *self, GtkWidget *parent) {
  AdwDialog *dialog = adw_dialog_new(); adw_dialog_set_title(dialog, _("Activity Retention"));
  adw_dialog_set_content_width(dialog, 400);
  GtkWidget *save = gtk_button_new_with_mnemonic(_("_Save"));
  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), grotto_dialog_header(dialog, save));
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_description(group, _("Metadata only — no messages, secrets or event content"));
  AdwSpinRow *row = ADW_SPIN_ROW(adw_spin_row_new_with_range(1, 30, 1));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Keep Activity (Days)"));
  adw_spin_row_set_value(row, CLAMP(g_settings_get_int(self->settings, "log-retention-days"), 1, 30));
  g_object_set_data(G_OBJECT(save), "retention-row", row);
  g_signal_connect_object(save, "clicked", G_CALLBACK(retention_save), self, 0);
  adw_preferences_group_add(group, GTK_WIDGET(row)); adw_preferences_page_add(page, group);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(page));
  adw_dialog_set_child(dialog, toolbar); adw_dialog_present(dialog, parent);
}
typedef struct { GSettings *settings; AdwDialog *dialog; GtkEntry *custom; GtkLabel *error; GtkWidget *save; GPtrArray *checks; } Policy;
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
static void policy_validate(GtkEditable *editable, gpointer data) {
  Policy *p = data; gboolean valid = TRUE;
  g_auto(GStrv) custom = g_strsplit_set(gtk_editable_get_text(editable), ", \t\n", -1);
  for (guint i = 0; custom[i]; i++) {
    if (!*custom[i]) continue;
    gchar *end = NULL; guint64 kind = g_ascii_strtoull(custom[i], &end, 10);
    if (end == custom[i] || *end || kind > G_MAXUINT16 || *custom[i] == '-') { valid = FALSE; break; }
  }
  gtk_widget_set_sensitive(p->save, valid);
  gtk_label_set_text(p->error, valid ? "" : _("Use whole event-type numbers from 0 to 65535, separated by commas."));
}
static void policy_default(AdwActionRow *row, gpointer data) {
  (void)row; Policy *p = data;
  for (guint i = 0; i < p->checks->len; i++) gtk_check_button_set_active(p->checks->pdata[i], TRUE);
  gtk_editable_set_text(GTK_EDITABLE(p->custom), "");
}
void page_settings_show_ask_defaults(PageSettings *self, GtkWidget *parent) {
  Policy *p = g_new0(Policy, 1); p->settings = g_object_ref(self->settings); p->checks = g_ptr_array_new();
  p->dialog = adw_dialog_new(); adw_dialog_set_title(p->dialog, _("Ask-by-Default Event Types")); adw_dialog_set_content_width(p->dialog, 400);
  p->save = gtk_button_new_with_mnemonic(_("_Save"));
  g_signal_connect(p->save, "clicked", G_CALLBACK(policy_save), p);
  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), grotto_dialog_header(p->dialog, p->save));
  GtkWidget *scroll = gtk_scrolled_window_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_margin_start(box, 18); gtk_widget_set_margin_end(box, 18); gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
  g_autoptr(GVariant) configured = g_settings_get_user_value(self->settings, "require-confirmation-kinds");
  g_auto(GStrv) values = configured ? g_variant_dup_strv(configured, NULL) : NULL;
  GtkWidget *kinds = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(kinds), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(kinds, "boxed-list");
  gtk_box_append(GTK_BOX(box), kinds);
  GString *others = g_string_new("");
  for (guint i = 0; i < G_N_ELEMENTS(signer_default_ask_by_default_kinds); i++) {
    guint32 kind = signer_default_ask_by_default_kinds[i]; g_autofree gchar *number = g_strdup_printf("%u", kind);
    GtkWidget *row = adw_action_row_new(), *check = gtk_check_button_new();
    const gchar *name = gnostr_approval_kind_name(kind);
    g_autofree gchar *subtitle = g_strdup_printf(_("Kind %u"), kind);
    g_autofree gchar *accessible_name = g_strdup_printf(_("%s, kind %u"), name, kind);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(check), GTK_ACCESSIBLE_PROPERTY_LABEL, accessible_name, -1);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), check);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), !configured || g_strv_contains((const gchar *const *)values, number));
    g_object_set_data_full(G_OBJECT(check), "kind", g_strdup(number), g_free);
    gtk_list_box_append(GTK_LIST_BOX(kinds), row); g_ptr_array_add(p->checks, check);
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
  g_signal_connect(p->custom, "changed", G_CALLBACK(policy_validate), p); policy_validate(GTK_EDITABLE(p->custom), p);
  GtkWidget *group = adw_preferences_group_new(), *defaults = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(defaults), _("_Restore Defaults"));
  adw_preferences_row_set_use_underline(ADW_PREFERENCES_ROW(defaults), TRUE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(defaults), TRUE);
  g_signal_connect(defaults, "activated", G_CALLBACK(policy_default), p);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), defaults); gtk_box_append(GTK_BOX(box), group);
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
  BIND(security); BIND(management); BIND(retention); BIND(lock_timeout); BIND(lock_idle); BIND(pause_lock);
#undef BIND
}
static void bootstrap_save(GtkButton *button, gpointer data) {
  (void)button; PageSettings *self = data;
  GtkTextIter start, end; GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->bootstrap_relays);
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
  g_autoptr(GStrvBuilder) values = g_strv_builder_new(); guint count = 0;
  for (guint i = 0; lines[i]; i++) {
    const char *url = g_strstrip(lines[i]); if (!*url) continue;
    g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    if (++count > 16 || strlen(url) > 1024 || !uri || !g_uri_get_host(uri) ||
        g_strcmp0(g_uri_get_scheme(uri), "wss") || g_uri_get_userinfo(uri) || g_uri_get_query(uri) || g_uri_get_fragment(uri)) {
      gtk_label_set_text(self->bootstrap_error, _("Use up to 16 wss:// relay URLs, one per line, without credentials, queries or fragments")); return;
    }
    g_strv_builder_add(values, url);
  }
  g_auto(GStrv) urls = g_strv_builder_end(values);
  gboolean saved = g_settings_set_strv(self->settings, "profile-bootstrap-relays", (const char *const *)urls);
  gtk_label_set_text(self->bootstrap_error, saved ? _("Discovery relays saved") : _("Discovery relays could not be saved"));
}
static void bootstrap_defaults(GtkButton *button, gpointer data) {
  (void)button; PageSettings *self = data;
  g_settings_reset(self->settings, "profile-bootstrap-relays");
  g_auto(GStrv) urls = g_settings_get_strv(self->settings, "profile-bootstrap-relays");
  g_autofree char *text = g_strjoinv("\n", urls);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(self->bootstrap_relays), text, -1);
  gtk_label_set_text(self->bootstrap_error, _("Discovery-only defaults restored"));
}
static void add_profile_discovery(PageSettings *self) {
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(group, _("Profile Discovery"));
  adw_preferences_group_set_description(group, _("For identities with no known relays, share the public key with discovery relays to find a profile. Never used for publishing."));
  self->bootstrap_enabled = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->bootstrap_enabled), _("Find Profiles Automatically"));
  g_settings_bind(self->settings, "profile-bootstrap-enabled", self->bootstrap_enabled, "active", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add(group, GTK_WIDGET(self->bootstrap_enabled));
  AdwExpanderRow *details = ADW_EXPANDER_ROW(adw_expander_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(details), _("Discovery-Only Relays"));
  adw_expander_row_set_subtitle(details, _("Profiles, public relay lists, and messaging relay lists"));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12); gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
  self->bootstrap_relays = GTK_TEXT_VIEW(gtk_text_view_new()); gtk_text_view_set_wrap_mode(self->bootstrap_relays, GTK_WRAP_CHAR);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->bootstrap_relays), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Discovery relay URLs, one per line"), -1);
  g_auto(GStrv) urls = g_settings_get_strv(self->settings, "profile-bootstrap-relays");
  g_autofree char *text = g_strjoinv("\n", urls);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(self->bootstrap_relays), text, -1);
  gtk_widget_set_size_request(GTK_WIDGET(self->bootstrap_relays), -1, 100); gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->bootstrap_relays));
  GtkWidget *save = gtk_button_new_with_label(_("Save Discovery Relays")), *reset = gtk_button_new_with_label(_("Restore Defaults"));
  g_signal_connect(save, "clicked", G_CALLBACK(bootstrap_save), self); g_signal_connect(reset, "clicked", G_CALLBACK(bootstrap_defaults), self);
  gtk_box_append(GTK_BOX(box), save); gtk_box_append(GTK_BOX(box), reset);
  self->bootstrap_error = GTK_LABEL(gtk_label_new("")); gtk_label_set_wrap(self->bootstrap_error, TRUE); gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->bootstrap_error));
  adw_expander_row_add_row(details, box); adw_preferences_group_add(group, GTK_WIDGET(details)); adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), group);
}
static void page_settings_init(PageSettings *self) {
  gtk_widget_init_template(GTK_WIDGET(self)); self->settings = g_settings_new("org.nostr.Grotto");
  gint timeout = g_settings_get_int(self->settings, "lock-timeout-sec");
  for (guint i = 0; i < G_N_ELEMENTS(lock_values); i++) if (timeout == lock_values[i]) adw_combo_row_set_selected(self->lock_timeout, i);
  g_settings_bind(self->settings, "session-lock-on-idle", self->lock_idle, "active", G_SETTINGS_BIND_DEFAULT);
  g_settings_bind(self->settings, "pause-on-screen-lock", self->pause_lock, "active", G_SETTINGS_BIND_DEFAULT);
  g_signal_connect(self->lock_timeout, "notify::selected", G_CALLBACK(combo_changed), self);
  g_settings_bind(self->settings, "log-retention-days", self->retention, "value", G_SETTINGS_BIND_DEFAULT);
  AdwPreferencesGroup *privacy = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(privacy, _("Privacy and Network"));
  AdwSwitchRow *images = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(images), _("Load Profile Images"));
  adw_action_row_set_subtitle(ADW_ACTION_ROW(images), _("Fetch avatars and banners from the web addresses in profiles"));
  g_settings_bind(self->settings, "load-profile-images", images, "active", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add(privacy, GTK_WIDGET(images));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), privacy);
  add_profile_discovery(self);
}
PageSettings *page_settings_new(void) { return g_object_new(TYPE_PAGE_SETTINGS, NULL); }
