/* Settings page controller: wires buttons to open sheets */
#include "page-settings.h"
#include "app-resources.h"
#include "sheets/sheet-select-account.h"
#include "sheets/sheet-import-key.h"
#include "sheets/sheet-account-backup.h"
#include "sheets/sheet-backup.h"
#include "sheets/sheet-orbot-setup.h"
#include "sheets/sheet-user-list.h"
#include "sheets/sheet-relay-config.h"
#include "sheets/sheet-profile-editor.h"
#include "sheets/sheet-key-rotation.h"
#include "../secret_store.h"
#include "../profile_store.h"
#include "../settings_manager.h"
#include "../session-manager.h"
#include "../client_session.h"
#include "../startup-timing.h"

/* Provided by settings_page.c for cross-component updates */
extern void gnostr_settings_apply_import_success(const char *npub, const char *label);

/* Sheet import success callback (file-scope) */
static void on_sheet_import_success(const char *npub, const char *label, gpointer ud){ (void)ud; gnostr_settings_apply_import_success(npub, label); }

struct _PageSettings {
  AdwPreferencesPage parent_instance;
  /* Template children */
  AdwComboRow *combo_theme;
  GtkButton *btn_add_account;
  GtkButton *btn_select_account;
  GtkButton *btn_backup_keys;
  GtkButton *btn_edit_profile;
  GtkButton *btn_key_rotation;
  GtkButton *btn_orbot_setup;
  GtkButton *btn_relays;
  GtkButton *btn_logs;
  GtkButton *btn_sign_policy;
  GtkSwitch *switch_listen;
  /* Social list buttons */
  GtkButton *btn_follows;
  GtkButton *btn_mutes;
  /* Session settings widgets */
  AdwComboRow *combo_lock_timeout;
  AdwSwitchRow *switch_lock_on_idle;
  AdwComboRow *combo_client_session_timeout;
  GtkButton *btn_manage_sessions;
  /* Internal state */
  gboolean updating_theme; /* Guard against recursive updates */
  gboolean updating_lock_timeout; /* Guard against recursive updates */
  gboolean updating_client_session_timeout; /* Guard against recursive updates */
  GSettings *settings; /* GSettings reference for session settings */
};

G_DEFINE_TYPE(PageSettings, page_settings, ADW_TYPE_PREFERENCES_PAGE)

/* Helpers */
static GtkWindow *get_parent_window(GtkWidget *w){
  GtkRoot *root = gtk_widget_get_root(w);
  return root ? GTK_WINDOW(root) : NULL;
}

/* Theme combo handler - maps combo index to SettingsTheme enum */
static void on_theme_combo_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  PageSettings *self = user_data;
  if (!self || self->updating_theme) return;

  AdwComboRow *combo = ADW_COMBO_ROW(obj);
  guint selected = adw_combo_row_get_selected(combo);

  /* Map: 0 = System, 1 = Light, 2 = Dark */
  SettingsTheme theme;
  switch (selected) {
    case 1:
      theme = SETTINGS_THEME_LIGHT;
      break;
    case 2:
      theme = SETTINGS_THEME_DARK;
      break;
    default:
      theme = SETTINGS_THEME_SYSTEM;
      break;
  }

  SettingsManager *sm = settings_manager_get_default();
  settings_manager_set_theme(sm, theme);
  g_message("Theme preference changed to: %u", selected);
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
  /* When import succeeds, Settings applies account changes and refreshes */
  sheet_import_key_set_on_success(dlg, on_sheet_import_success, NULL);
  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}
static void on_backup_keys(GtkButton *b, gpointer user_data){
  (void)b; PageSettings *self = user_data; if (!self) return;

  /* Get the currently active npub */
  gchar *npub = NULL;
  SecretStoreResult rc = secret_store_get_public_key(NULL, &npub);

  if (rc != SECRET_STORE_OK || !npub || !*npub) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("No account selected. Please select or add an account first.");
    gtk_alert_dialog_show(ad, get_parent_window(GTK_WIDGET(self)));
    g_object_unref(ad);
    return;
  }

  /* Create and present the comprehensive backup/recovery dialog */
  SheetBackup *dlg = sheet_backup_new();
  sheet_backup_set_account(dlg, npub);
  g_free(npub);

  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}

/* Profile editor save callback - called when user saves profile locally */
static void on_profile_save(const gchar *npub, const gchar *event_json, gpointer ud) {
  (void)ud;
  g_message("Profile saved for %s: %s", npub, event_json);
  /* The profile is saved to local cache by the editor */
}

/* Profile editor publish callback - called when profile is signed and ready */
static void on_profile_publish(const gchar *npub, const gchar *signed_event_json, gpointer ud) {
  (void)ud;
  g_message("Publishing profile for %s: %s", npub, signed_event_json);
  /* TODO: Publish to relays via relay_store or bunker_service */
}

static void on_edit_profile(GtkButton *b, gpointer user_data){
  (void)b; PageSettings *self = user_data; if (!self) return;

  /* Get the currently active npub */
  gchar *npub = NULL;
  SecretStoreResult rc = secret_store_get_public_key(NULL, &npub);

  if (rc != SECRET_STORE_OK || !npub || !*npub) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("No account selected. Please select or add an account first.");
    gtk_alert_dialog_show(ad, get_parent_window(GTK_WIDGET(self)));
    g_object_unref(ad);
    return;
  }

  /* Create the profile editor dialog */
  SheetProfileEditor *dlg = sheet_profile_editor_new();
  sheet_profile_editor_set_npub(dlg, npub);
  sheet_profile_editor_set_on_save(dlg, on_profile_save, NULL);
  sheet_profile_editor_set_on_publish(dlg, on_profile_publish, NULL);

  /* Try to load existing profile data from cache */
  ProfileStore *ps = profile_store_new();
  NostrProfile *profile = profile_store_get(ps, npub);

  if (profile) {
    sheet_profile_editor_load_profile(dlg,
                                      profile->name,
                                      profile->about,
                                      profile->picture,
                                      profile->banner,
                                      profile->nip05,
                                      profile->lud16,
                                      profile->website);
    nostr_profile_free(profile);
  }

  profile_store_free(ps);
  g_free(npub);

  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}

/* Key rotation completion callback */
static void on_key_rotation_complete(const gchar *old_npub, const gchar *new_npub, gpointer ud) {
  (void)ud;
  g_message("Key rotation complete: %s -> %s", old_npub, new_npub);
  /* The accounts_store is already updated by the rotation module */
}

static void on_key_rotation(GtkButton *b, gpointer user_data){
  (void)b; PageSettings *self = user_data; if (!self) return;

  /* Get the currently active npub */
  gchar *npub = NULL;
  SecretStoreResult rc = secret_store_get_public_key(NULL, &npub);

  if (rc != SECRET_STORE_OK || !npub || !*npub) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("No account selected. Please select or add an account first.");
    gtk_alert_dialog_show(ad, get_parent_window(GTK_WIDGET(self)));
    g_object_unref(ad);
    return;
  }

  /* Create and present the key rotation dialog */
  SheetKeyRotation *dlg = sheet_key_rotation_new();
  sheet_key_rotation_set_account(dlg, npub);
  sheet_key_rotation_set_on_complete(dlg, on_key_rotation_complete, NULL);
  g_free(npub);

  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}

static void on_orbot_setup(GtkButton *b, gpointer user_data){
  (void)b; PageSettings *self = user_data; if (!self) return;
  SheetOrbotSetup *dlg = sheet_orbot_setup_new();
  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}
static void on_relays(GtkButton *b, gpointer user_data){
  (void)b; PageSettings *self = user_data; if (!self) return;

  /* Get the currently active npub for per-identity relay config */
  gchar *npub = NULL;
  SecretStoreResult rc = secret_store_get_public_key(NULL, &npub);

  SheetRelayConfig *dlg;
  if (rc == SECRET_STORE_OK && npub && *npub) {
    /* Open relay config for this identity */
    dlg = sheet_relay_config_new_for_identity(npub);
    g_free(npub);
  } else {
    /* No active identity, open global relay config */
    dlg = sheet_relay_config_new();
  }

  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
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

/* User list publish callback - called when user saves and publishes the list */
static void on_user_list_publish(UserListType type, const gchar *event_json, gpointer ud) {
  (void)ud;
  const gchar *list_name = (type == USER_LIST_FOLLOWS) ? "follows" : "mutes";
  g_message("Publishing %s list event: %s", list_name, event_json);
  /* TODO: Sign and publish to relays via bunker service or relay_store */
}

static void on_follows(GtkButton *b, gpointer user_data){
  (void)b; PageSettings *self = user_data; if (!self) return;
  SheetUserList *dlg = sheet_user_list_new(USER_LIST_FOLLOWS);
  sheet_user_list_set_on_publish(dlg, on_user_list_publish, NULL);
  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}

static void on_mutes(GtkButton *b, gpointer user_data){
  (void)b; PageSettings *self = user_data; if (!self) return;
  SheetUserList *dlg = sheet_user_list_new(USER_LIST_MUTES);
  sheet_user_list_set_on_publish(dlg, on_user_list_publish, NULL);
  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}

/* Lock timeout combo indices to seconds:
 * 0 = Never (0), 1 = 1 min (60), 2 = 5 min (300), 3 = 15 min (900),
 * 4 = 30 min (1800), 5 = 1 hour (3600) */
static const gint lock_timeout_values[] = { 0, 60, 300, 900, 1800, 3600 };
static const gsize lock_timeout_count = G_N_ELEMENTS(lock_timeout_values);

static guint lock_timeout_to_index(gint seconds) {
  for (gsize i = 0; i < lock_timeout_count; i++) {
    if (lock_timeout_values[i] == seconds) return (guint)i;
  }
  /* Default to 5 minutes if unknown */
  return 2;
}

/* Client session timeout indices to seconds:
 * 0 = 10 min (600), 1 = 15 min (900), 2 = 30 min (1800), 3 = 1 hour (3600),
 * 4 = 4 hours (14400), 5 = 24 hours (86400), 6 = Forever (0) */
static const gint client_session_values[] = { 600, 900, 1800, 3600, 14400, 86400, 0 };
static const gsize client_session_count = G_N_ELEMENTS(client_session_values);

static guint client_session_to_index(gint seconds) {
  for (gsize i = 0; i < client_session_count; i++) {
    if (client_session_values[i] == seconds) return (guint)i;
  }
  /* Default to 15 minutes if unknown */
  return 1;
}

static void on_lock_timeout_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  PageSettings *self = user_data;
  if (!self || self->updating_lock_timeout) return;

  AdwComboRow *combo = ADW_COMBO_ROW(obj);
  guint idx = adw_combo_row_get_selected(combo);

  if (idx < lock_timeout_count) {
    gint seconds = lock_timeout_values[idx];
    GnSessionManager *sm = gn_session_manager_get_default();
    gn_session_manager_set_timeout(sm, (guint)seconds);

    /* Also save to GSettings */
    if (self->settings) {
      g_settings_set_int(self->settings, "lock-timeout-sec", seconds);
    }

    g_message("Lock timeout changed to %d seconds", seconds);
  }
}

static void on_lock_on_idle_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  PageSettings *self = user_data;
  if (!self) return;

  AdwSwitchRow *row = ADW_SWITCH_ROW(obj);
  gboolean active = adw_switch_row_get_active(row);

  if (self->settings) {
    g_settings_set_boolean(self->settings, "session-lock-on-idle", active);
  }

  g_message("Lock on system idle: %s", active ? "enabled" : "disabled");
}

static void on_client_session_timeout_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  PageSettings *self = user_data;
  if (!self || self->updating_client_session_timeout) return;

  AdwComboRow *combo = ADW_COMBO_ROW(obj);
  guint idx = adw_combo_row_get_selected(combo);

  if (idx < client_session_count) {
    gint seconds = client_session_values[idx];
    GnClientSessionManager *csm = gn_client_session_manager_get_default();
    gn_client_session_manager_set_timeout(csm, (guint)seconds);

    /* Also save to GSettings */
    if (self->settings) {
      g_settings_set_int(self->settings, "client-session-timeout-sec", seconds);
    }

    g_message("Client session timeout changed to %d seconds", seconds);
  }
}

static void on_manage_sessions(GtkButton *b, gpointer user_data) {
  (void)b;
  PageSettings *self = user_data;
  if (!self) return;

  /* Navigate to the sessions page - find the window and switch pages */
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  if (root) {
    /* The signer window has a stack, we can activate the "sessions" action
     * or just emit a signal. For now, log that this should navigate. */
    g_message("Navigate to sessions page requested");

    /* Try to activate the sessions page via the view stack */
    /* This requires access to the parent window's stack */
    GtkWidget *window = GTK_WIDGET(root);

    /* Use action to navigate if available */
    gtk_widget_activate_action(window, "win.show-sessions", NULL);
  }
}

static void page_settings_dispose(GObject *object) {
  PageSettings *self = PAGE_SETTINGS(object);
  g_clear_object(&self->settings);
  G_OBJECT_CLASS(page_settings_parent_class)->dispose(object);
}

static void page_settings_class_init(PageSettingsClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);

  object_class->dispose = page_settings_dispose;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-settings.ui");
  gtk_widget_class_bind_template_child(wc, PageSettings, combo_theme);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_add_account);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_select_account);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_backup_keys);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_edit_profile);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_key_rotation);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_orbot_setup);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_relays);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_logs);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_sign_policy);
  gtk_widget_class_bind_template_child(wc, PageSettings, switch_listen);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_follows);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_mutes);
  /* Session settings bindings */
  gtk_widget_class_bind_template_child(wc, PageSettings, combo_lock_timeout);
  gtk_widget_class_bind_template_child(wc, PageSettings, switch_lock_on_idle);
  gtk_widget_class_bind_template_child(wc, PageSettings, combo_client_session_timeout);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_manage_sessions);
}

static void page_settings_init(PageSettings *self) {
  gint64 init_start = startup_timing_measure_start();

  gtk_widget_init_template(GTK_WIDGET(self));

  /* Initialize GSettings */
  self->settings = g_settings_new("org.gnostr.Signer");

  /* Initialize theme combo with current setting (settings already loaded, fast) */
  self->updating_theme = TRUE;
  SettingsManager *sm = settings_manager_get_default();
  SettingsTheme theme = settings_manager_get_theme(sm);
  /* Map SettingsTheme to combo index: SYSTEM=0, LIGHT=1, DARK=2
   * Note: HIGH_CONTRAST falls back to SYSTEM since the combo only has 3 options.
   * High contrast is still applied via AdwStyleManager in main_app.c */
  guint theme_idx;
  switch (theme) {
    case SETTINGS_THEME_LIGHT:
      theme_idx = 1;
      break;
    case SETTINGS_THEME_DARK:
      theme_idx = 2;
      break;
    case SETTINGS_THEME_HIGH_CONTRAST:
      /* High contrast: fall back to System in UI (actual theme still applied) */
      theme_idx = 0;
      break;
    case SETTINGS_THEME_SYSTEM:
    default:
      theme_idx = 0;
      break;
  }
  adw_combo_row_set_selected(self->combo_theme, theme_idx);
  self->updating_theme = FALSE;

  /* Initialize session settings from GSettings */
  self->updating_lock_timeout = TRUE;
  self->updating_client_session_timeout = TRUE;

  if (self->settings) {
    /* Load lock timeout */
    gint lock_timeout = g_settings_get_int(self->settings, "lock-timeout-sec");
    adw_combo_row_set_selected(self->combo_lock_timeout, lock_timeout_to_index(lock_timeout));

    /* Load lock on idle setting */
    gboolean lock_on_idle = g_settings_get_boolean(self->settings, "session-lock-on-idle");
    adw_switch_row_set_active(self->switch_lock_on_idle, lock_on_idle);

    /* Load client session timeout */
    gint client_timeout = g_settings_get_int(self->settings, "client-session-timeout-sec");
    adw_combo_row_set_selected(self->combo_client_session_timeout, client_session_to_index(client_timeout));
  }

  self->updating_lock_timeout = FALSE;
  self->updating_client_session_timeout = FALSE;

  /* Connect theme combo change handler */
  g_signal_connect(self->combo_theme, "notify::selected", G_CALLBACK(on_theme_combo_changed), self);

  /* Connect session settings handlers */
  g_signal_connect(self->combo_lock_timeout, "notify::selected", G_CALLBACK(on_lock_timeout_changed), self);
  g_signal_connect(self->switch_lock_on_idle, "notify::active", G_CALLBACK(on_lock_on_idle_changed), self);
  g_signal_connect(self->combo_client_session_timeout, "notify::selected", G_CALLBACK(on_client_session_timeout_changed), self);
  g_signal_connect(self->btn_manage_sessions, "clicked", G_CALLBACK(on_manage_sessions), self);

  /* Handlers */
  g_signal_connect(self->btn_add_account, "clicked", G_CALLBACK(on_add_account), self);
  g_signal_connect(self->btn_select_account, "clicked", G_CALLBACK(on_select_account), self);
  g_signal_connect(self->btn_backup_keys, "clicked", G_CALLBACK(on_backup_keys), self);
  g_signal_connect(self->btn_edit_profile, "clicked", G_CALLBACK(on_edit_profile), self);
  g_signal_connect(self->btn_key_rotation, "clicked", G_CALLBACK(on_key_rotation), self);
  g_signal_connect(self->btn_orbot_setup, "clicked", G_CALLBACK(on_orbot_setup), self);
  g_signal_connect(self->btn_relays, "clicked", G_CALLBACK(on_relays), self);
  g_signal_connect(self->btn_logs, "clicked", G_CALLBACK(on_logs), self);
  g_signal_connect(self->btn_sign_policy, "clicked", G_CALLBACK(on_sign_policy), self);
  g_signal_connect(self->switch_listen, "notify::active", G_CALLBACK(on_listen_notify), self);
  g_signal_connect(self->btn_follows, "clicked", G_CALLBACK(on_follows), self);
  g_signal_connect(self->btn_mutes, "clicked", G_CALLBACK(on_mutes), self);

  startup_timing_measure_end(init_start, "page-settings-init", 50);
}

PageSettings *page_settings_new(void) {
  return g_object_new(TYPE_PAGE_SETTINGS, NULL);
}
