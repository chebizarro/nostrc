/* Settings page controller: wires buttons to open sheets */
#include "page-settings.h"
#include "app-resources.h"
#include "sheets/sheet-select-account.h"
#include "sheets/sheet-import-key.h"
#include "sheets/sheet-account-backup.h"
#include "sheets/sheet-orbot-setup.h"

struct _PageSettings {
  AdwPreferencesPage parent_instance;
  /* Template children */
  GtkButton *btn_add_account;
  GtkButton *btn_select_account;
  GtkButton *btn_backup_keys;
  GtkButton *btn_orbot_setup;
  GtkButton *btn_relays;
  GtkButton *btn_logs;
  GtkButton *btn_sign_policy;
  GtkSwitch *switch_listen;
};

G_DEFINE_TYPE(PageSettings, page_settings, ADW_TYPE_PREFERENCES_PAGE)

/* Helpers */
static GtkWindow *get_parent_window(GtkWidget *w){
  GtkRoot *root = gtk_widget_get_root(w);
  return root ? GTK_WINDOW(root) : NULL;
}

/* Handlers */
static void on_select_account(GtkButton *b, gpointer user_data){
  (void)b; PageSettings *self = user_data; if (!self) return;
  SheetSelectAccount *dlg = sheet_select_account_new();
  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}
static void on_add_account(GtkButton *b, gpointer user_data){
  (void)b; PageSettings *self = user_data; if (!self) return;
  SheetImportKey *dlg = sheet_import_key_new();
  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}
static void on_backup_keys(GtkButton *b, gpointer user_data){
  (void)b; PageSettings *self = user_data; if (!self) return;
  SheetAccountBackup *dlg = sheet_account_backup_new();
  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}
static void on_orbot_setup(GtkButton *b, gpointer user_data){
  (void)b; PageSettings *self = user_data; if (!self) return;
  SheetOrbotSetup *dlg = sheet_orbot_setup_new();
  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}
static void on_relays(GtkButton *b, gpointer user_data){
  (void)b; (void)user_data; /* TODO: implement relays manager */
}
static void on_logs(GtkButton *b, gpointer user_data){
  (void)b; (void)user_data; /* TODO: implement log viewer */
}
static void on_sign_policy(GtkButton *b, gpointer user_data){
  (void)b; (void)user_data; /* TODO: implement sign policy editor */
}
static void on_listen_notify(GObject *obj, GParamSpec *pspec, gpointer user_data){
  (void)pspec; (void)user_data; GtkSwitch *sw = GTK_SWITCH(obj);
  gboolean active = gtk_switch_get_active(sw);
  g_message("Listen for new connections: %s", active ? "on" : "off");
}

static void page_settings_class_init(PageSettingsClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-settings.ui");
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_add_account);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_select_account);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_backup_keys);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_orbot_setup);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_relays);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_logs);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_sign_policy);
  gtk_widget_class_bind_template_child(wc, PageSettings, switch_listen);
}

static void page_settings_init(PageSettings *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  /* Handlers */
  g_signal_connect(self->btn_add_account, "clicked", G_CALLBACK(on_add_account), self);
  g_signal_connect(self->btn_select_account, "clicked", G_CALLBACK(on_select_account), self);
  g_signal_connect(self->btn_backup_keys, "clicked", G_CALLBACK(on_backup_keys), self);
  g_signal_connect(self->btn_orbot_setup, "clicked", G_CALLBACK(on_orbot_setup), self);
  g_signal_connect(self->btn_relays, "clicked", G_CALLBACK(on_relays), self);
  g_signal_connect(self->btn_logs, "clicked", G_CALLBACK(on_logs), self);
  g_signal_connect(self->btn_sign_policy, "clicked", G_CALLBACK(on_sign_policy), self);
  g_signal_connect(self->switch_listen, "notify::active", G_CALLBACK(on_listen_notify), self);
}

PageSettings *page_settings_new(void) {
  return g_object_new(TYPE_PAGE_SETTINGS, NULL);
}
