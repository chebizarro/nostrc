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
#include "sheets/sheet-social-recovery.h"
#include "../secret_store.h"
#include "../profile_store.h"
#include "../relay_store.h"
#include "../settings_manager.h"
#include "../session-manager.h"
#include "../client_session.h"
#include "../startup-timing.h"
#include "../i18n.h"
#include "../event_history.h"
#include "../policy_store.h"

/* Forward declarations for relay publish helpers */
static void publish_signed_event_to_relays(const gchar *signed_event_json, const gchar *event_type);

/* Provided by settings_page.c for cross-component updates */
extern void gnostr_settings_apply_import_success(const char *npub, const char *label);

/* Sheet import success callback (file-scope) */
static void on_sheet_import_success(const char *npub, const char *label, gpointer ud){ (void)ud; gnostr_settings_apply_import_success(npub, label); }

struct _PageSettings {
  AdwPreferencesPage parent_instance;
  /* Template children */
  AdwComboRow *combo_theme;
  AdwComboRow *combo_language;
  AdwSwitchRow *switch_force_high_contrast;
  AdwComboRow *combo_high_contrast_variant;
  GtkButton *btn_add_account;
  GtkButton *btn_select_account;
  GtkButton *btn_backup_keys;
  GtkButton *btn_social_recovery;
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
  gboolean updating_language; /* Guard against recursive updates */
  gboolean updating_high_contrast; /* Guard against recursive updates */
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

  /* Map: 0 = System, 1 = Light, 2 = Dark, 3 = High Contrast */
  SettingsTheme theme;
  switch (selected) {
    case 1:
      theme = SETTINGS_THEME_LIGHT;
      break;
    case 2:
      theme = SETTINGS_THEME_DARK;
      break;
    case 3:
      theme = SETTINGS_THEME_HIGH_CONTRAST;
      break;
    default:
      theme = SETTINGS_THEME_SYSTEM;
      break;
  }

  SettingsManager *sm = settings_manager_get_default();
  settings_manager_set_theme(sm, theme);
  g_message("Theme preference changed to: %u", selected);
}

/* Language codes mapping to combo indices:
 * 0 = System Default, 1 = en, 2 = ja, 3 = es, 4 = pt_BR, 5 = id, 6 = fa */
static const gchar *language_codes[] = { NULL, "en", "ja", "es", "pt_BR", "id", "fa" };
static const gsize language_count = G_N_ELEMENTS(language_codes);

static guint language_to_index(const gchar *lang) {
  if (!lang || !*lang) return 0; /* System default */
  for (gsize i = 1; i < language_count; i++) {
    if (g_str_equal(language_codes[i], lang)) return (guint)i;
  }
  return 0; /* Unknown language = system default */
}

/* Language combo handler */
static void on_language_combo_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  PageSettings *self = user_data;
  if (!self || self->updating_language) return;

  AdwComboRow *combo = ADW_COMBO_ROW(obj);
  guint selected = adw_combo_row_get_selected(combo);

  if (selected < language_count) {
    const gchar *lang = language_codes[selected];
    gn_i18n_set_language(lang);

    /* Show restart prompt */
    GtkAlertDialog *ad = gtk_alert_dialog_new(
      _("Language changed. Please restart the application for changes to take effect."));
    gtk_alert_dialog_show(ad, get_parent_window(GTK_WIDGET(self)));
    g_object_unref(ad);

    g_message("Language preference changed to: %s", lang ? lang : "system default");
  }
}

/* Force high contrast switch handler */
static void on_force_high_contrast_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  PageSettings *self = user_data;
  if (!self || self->updating_high_contrast) return;

  AdwSwitchRow *row = ADW_SWITCH_ROW(obj);
  gboolean active = adw_switch_row_get_active(row);

  SettingsManager *sm = settings_manager_get_default();
  settings_manager_set_force_high_contrast(sm, active);
  g_message("Force high contrast: %s", active ? "enabled" : "disabled");
}

/* High contrast variant combo handler */
static void on_high_contrast_variant_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  PageSettings *self = user_data;
  if (!self || self->updating_high_contrast) return;

  AdwComboRow *combo = ADW_COMBO_ROW(obj);
  guint selected = adw_combo_row_get_selected(combo);

  /* Map: 0 = Black on White (default), 1 = White on Black (inverted), 2 = Yellow on Black */
  SettingsHighContrastVariant variant;
  switch (selected) {
    case 1:
      variant = SETTINGS_HC_INVERTED;
      break;
    case 2:
      variant = SETTINGS_HC_YELLOW_ON_BLACK;
      break;
    default:
      variant = SETTINGS_HC_DEFAULT;
      break;
  }

  SettingsManager *sm = settings_manager_get_default();
  settings_manager_set_high_contrast_variant(sm, variant);
  g_message("High contrast variant changed to: %u", selected);
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

/* Helper to publish signed events to configured relays.
 * In a full implementation, this would use WebSocket connections to relays.
 * For now, it verifies relay configuration and logs the publish attempt. */
static void publish_signed_event_to_relays(const gchar *signed_event_json, const gchar *event_type) {
  if (!signed_event_json || !event_type) return;

  /* Load relay configuration */
  RelayStore *relay_store = relay_store_new();
  relay_store_load(relay_store);

  GPtrArray *write_relays = relay_store_get_write_relays(relay_store);

  if (write_relays->len == 0) {
    g_warning("No write relays configured - %s event not published", event_type);
    g_ptr_array_unref(write_relays);
    relay_store_free(relay_store);
    return;
  }

  g_message("Publishing %s event to %u relays:", event_type, write_relays->len);
  for (guint i = 0; i < write_relays->len; i++) {
    const gchar *relay_url = g_ptr_array_index(write_relays, i);
    g_message("  - %s", relay_url);
  }

  /* In a full implementation, we would:
   * 1. Connect to each write relay via WebSocket
   * 2. Send ["EVENT", signed_event_json]
   * 3. Wait for ["OK", event_id, true, ""] response
   * 4. Handle errors and retry logic
   *
   * For now, just log that we would publish. The actual WebSocket
   * relay publishing will be implemented when the relay connection
   * infrastructure is complete.
   */
  g_message("Event JSON: %.200s%s", signed_event_json,
            strlen(signed_event_json) > 200 ? "..." : "");

  g_ptr_array_unref(write_relays);
  relay_store_free(relay_store);
}

/* Profile editor publish callback - called when profile is signed and ready */
static void on_profile_publish(const gchar *npub, const gchar *signed_event_json, gpointer ud) {
  (void)ud;
  g_message("Publishing profile for %s", npub);

  /* Publish to configured write relays */
  publish_signed_event_to_relays(signed_event_json, "profile (kind:0)");
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

/* Social recovery completion callback */
static void on_social_recovery_complete(const gchar *npub, gpointer ud) {
  (void)ud;
  g_message("Social recovery action complete for %s", npub);
}

static void on_social_recovery(GtkButton *b, gpointer user_data){
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

  /* Create and present the social recovery dialog */
  SheetSocialRecovery *dlg = sheet_social_recovery_new();
  sheet_social_recovery_set_account(dlg, npub);
  sheet_social_recovery_set_on_complete(dlg, on_social_recovery_complete, NULL);
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
/* Create a log entry row widget */
static GtkWidget *create_log_entry_row(GnEventHistoryEntry *entry) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  /* Format timestamp */
  gchar *time_str = gn_event_history_entry_format_timestamp(entry);

  /* Build title: kind and method */
  gint kind = gn_event_history_entry_get_event_kind(entry);
  const gchar *method = gn_event_history_entry_get_method(entry);
  gchar *title = g_strdup_printf("Kind %d - %s", kind, method);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  g_free(title);

  /* Build subtitle: time, result, client app */
  const gchar *client_app = gn_event_history_entry_get_client_app(entry);
  GnEventHistoryResult result = gn_event_history_entry_get_result(entry);
  const gchar *result_str = result == GN_EVENT_HISTORY_SUCCESS ? "Success" :
                            result == GN_EVENT_HISTORY_DENIED ? "Denied" :
                            result == GN_EVENT_HISTORY_TIMEOUT ? "Timeout" : "Error";

  gchar *subtitle = g_strdup_printf("%s | %s%s%s",
                                    time_str, result_str,
                                    client_app ? " | " : "",
                                    client_app ? client_app : "");
  adw_action_row_set_subtitle(row, subtitle);
  g_free(subtitle);
  g_free(time_str);

  /* Add result icon */
  const gchar *icon_name = result == GN_EVENT_HISTORY_SUCCESS ? "emblem-ok-symbolic" :
                           result == GN_EVENT_HISTORY_DENIED ? "dialog-error-symbolic" :
                           "dialog-warning-symbolic";
  GtkImage *icon = GTK_IMAGE(gtk_image_new_from_icon_name(icon_name));
  adw_action_row_add_prefix(row, GTK_WIDGET(icon));

  return GTK_WIDGET(row);
}

static void on_logs(GtkButton *b, gpointer user_data) {
  (void)b;
  PageSettings *self = PAGE_SETTINGS(user_data);

  /* Create dialog */
  AdwDialog *dlg = adw_dialog_new();
  adw_dialog_set_title(dlg, "Event History");
  adw_dialog_set_content_width(dlg, 500);
  adw_dialog_set_content_height(dlg, 600);

  /* Create main box */
  GtkBox *main_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  /* Header bar */
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_header_bar_set_show_start_title_buttons(header, FALSE);
  adw_header_bar_set_show_end_title_buttons(header, FALSE);

  /* Close button */
  GtkButton *btn_close = GTK_BUTTON(gtk_button_new_with_label("Close"));
  gtk_widget_add_css_class(GTK_WIDGET(btn_close), "suggested-action");
  adw_header_bar_pack_end(header, GTK_WIDGET(btn_close));
  g_signal_connect_swapped(btn_close, "clicked", G_CALLBACK(adw_dialog_close), dlg);

  gtk_box_append(main_box, GTK_WIDGET(header));

  /* Scrolled window for list */
  GtkScrolledWindow *scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
  gtk_scrolled_window_set_policy(scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(GTK_WIDGET(scroll), TRUE);

  /* List box for entries */
  GtkListBox *list_box = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(list_box, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(list_box), "boxed-list");

  /* Load and display history entries */
  GnEventHistory *history = gn_event_history_get_default();
  gn_event_history_load(history);

  GPtrArray *entries = gn_event_history_list_entries(history, 0, 100);

  if (entries && entries->len > 0) {
    for (guint i = 0; i < entries->len; i++) {
      GnEventHistoryEntry *entry = g_ptr_array_index(entries, i);
      GtkWidget *row = create_log_entry_row(entry);
      gtk_list_box_append(list_box, row);
    }
    g_ptr_array_unref(entries);
  } else {
    /* Empty state */
    AdwStatusPage *empty = ADW_STATUS_PAGE(adw_status_page_new());
    adw_status_page_set_icon_name(empty, "document-open-symbolic");
    adw_status_page_set_title(empty, "No Events");
    adw_status_page_set_description(empty, "Event history will appear here after signing operations.");
    gtk_box_append(main_box, GTK_WIDGET(empty));
    gtk_widget_set_visible(GTK_WIDGET(scroll), FALSE);
  }

  gtk_scrolled_window_set_child(scroll, GTK_WIDGET(list_box));
  gtk_box_append(main_box, GTK_WIDGET(scroll));

  adw_dialog_set_child(dlg, GTK_WIDGET(main_box));
  adw_dialog_present(dlg, GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}

/* Forward declaration for policy remove callback */
static void on_policy_remove_clicked(GtkButton *btn, gpointer ud);

/* Create a policy entry row widget */
static GtkWidget *create_policy_entry_row(PolicyEntry *entry, PolicyStore *store) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  /* Title: app_id truncated */
  gchar *title = g_strdup(entry->app_id);
  if (strlen(title) > 16) {
    gchar *truncated = g_strdup_printf("%.12s...%.4s", title, title + strlen(title) - 4);
    g_free(title);
    title = truncated;
  }
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  g_free(title);

  /* Subtitle: decision and expiration */
  const gchar *decision_str = entry->decision ? "Allowed" : "Denied";
  gchar *subtitle;
  if (entry->expires_at == 0) {
    subtitle = g_strdup_printf("%s (permanent)", decision_str);
  } else {
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    if ((gint64)entry->expires_at > now) {
      gint64 remaining = (gint64)entry->expires_at - now;
      subtitle = g_strdup_printf("%s (expires in %" G_GINT64_FORMAT " min)", decision_str, remaining / 60);
    } else {
      subtitle = g_strdup_printf("%s (expired)", decision_str);
    }
  }
  adw_action_row_set_subtitle(row, subtitle);
  g_free(subtitle);

  /* Decision icon */
  const gchar *icon_name = entry->decision ? "emblem-ok-symbolic" : "action-unavailable-symbolic";
  GtkImage *icon = GTK_IMAGE(gtk_image_new_from_icon_name(icon_name));
  adw_action_row_add_prefix(row, GTK_WIDGET(icon));

  /* Remove button */
  GtkButton *btn_remove = GTK_BUTTON(gtk_button_new_from_icon_name("user-trash-symbolic"));
  gtk_widget_set_valign(GTK_WIDGET(btn_remove), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(btn_remove), "flat");
  adw_action_row_add_suffix(row, GTK_WIDGET(btn_remove));

  /* Store entry data for removal callback */
  g_object_set_data_full(G_OBJECT(row), "app-id", g_strdup(entry->app_id), g_free);
  g_object_set_data_full(G_OBJECT(row), "identity", g_strdup(entry->identity), g_free);
  g_object_set_data(G_OBJECT(row), "policy-store", store);
  g_object_set_data(G_OBJECT(btn_remove), "row", row);

  /* Connect remove button callback */
  g_signal_connect(btn_remove, "clicked", G_CALLBACK(on_policy_remove_clicked), NULL);

  return GTK_WIDGET(row);
}

/* Callback for policy remove button */
static void on_policy_remove_clicked(GtkButton *btn, gpointer ud) {
  (void)ud;
  GtkWidget *row = GTK_WIDGET(g_object_get_data(G_OBJECT(btn), "row"));
  const gchar *app_id = g_object_get_data(G_OBJECT(row), "app-id");
  const gchar *identity = g_object_get_data(G_OBJECT(row), "identity");
  PolicyStore *ps = g_object_get_data(G_OBJECT(row), "policy-store");

  if (app_id && identity && ps) {
    policy_store_unset(ps, app_id, identity);
    policy_store_save(ps);
  }

  GtkListBox *list = GTK_LIST_BOX(gtk_widget_get_parent(row));
  if (list) {
    gtk_list_box_remove(list, row);
  }
}

static void on_sign_policy(GtkButton *b, gpointer user_data) {
  (void)b;
  PageSettings *self = PAGE_SETTINGS(user_data);

  /* Create and load policy store */
  PolicyStore *ps = policy_store_new();
  policy_store_load(ps);

  /* Create dialog */
  AdwDialog *dlg = adw_dialog_new();
  adw_dialog_set_title(dlg, "Sign Policy");
  adw_dialog_set_content_width(dlg, 500);
  adw_dialog_set_content_height(dlg, 500);

  /* Create main box */
  GtkBox *main_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  /* Header bar */
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  adw_header_bar_set_show_start_title_buttons(header, FALSE);
  adw_header_bar_set_show_end_title_buttons(header, FALSE);

  /* Close button */
  GtkButton *btn_close = GTK_BUTTON(gtk_button_new_with_label("Close"));
  gtk_widget_add_css_class(GTK_WIDGET(btn_close), "suggested-action");
  adw_header_bar_pack_end(header, GTK_WIDGET(btn_close));

  gtk_box_append(main_box, GTK_WIDGET(header));

  /* Description label */
  GtkLabel *desc = GTK_LABEL(gtk_label_new(
    "Manage remembered signing decisions for applications. "
    "Remove entries to require re-approval."));
  gtk_label_set_wrap(desc, TRUE);
  gtk_label_set_xalign(desc, 0);
  gtk_widget_set_margin_start(GTK_WIDGET(desc), 16);
  gtk_widget_set_margin_end(GTK_WIDGET(desc), 16);
  gtk_widget_set_margin_top(GTK_WIDGET(desc), 12);
  gtk_widget_set_margin_bottom(GTK_WIDGET(desc), 12);
  gtk_widget_add_css_class(GTK_WIDGET(desc), "dim-label");
  gtk_box_append(main_box, GTK_WIDGET(desc));

  /* Scrolled window for list */
  GtkScrolledWindow *scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
  gtk_scrolled_window_set_policy(scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(GTK_WIDGET(scroll), TRUE);

  /* List box for entries */
  GtkListBox *list_box = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(list_box, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(list_box), "boxed-list");
  gtk_widget_set_margin_start(GTK_WIDGET(list_box), 16);
  gtk_widget_set_margin_end(GTK_WIDGET(list_box), 16);
  gtk_widget_set_margin_bottom(GTK_WIDGET(list_box), 16);

  /* Load and display policy entries */
  GPtrArray *entries = policy_store_list(ps);

  if (entries && entries->len > 0) {
    for (guint i = 0; i < entries->len; i++) {
      PolicyEntry *entry = g_ptr_array_index(entries, i);
      GtkWidget *row = create_policy_entry_row(entry, ps);
      gtk_list_box_append(list_box, row);
    }

    /* Free entries (but not the policy store - keep for modifications) */
    for (guint i = 0; i < entries->len; i++) {
      PolicyEntry *entry = g_ptr_array_index(entries, i);
      g_free(entry->app_id);
      g_free(entry->identity);
      g_free(entry);
    }
    g_ptr_array_free(entries, TRUE);
  } else {
    /* Empty state */
    AdwStatusPage *empty = ADW_STATUS_PAGE(adw_status_page_new());
    adw_status_page_set_icon_name(empty, "preferences-system-symbolic");
    adw_status_page_set_title(empty, "No Policies");
    adw_status_page_set_description(empty, "When you approve or deny signing requests, your decisions will appear here.");
    gtk_scrolled_window_set_child(scroll, GTK_WIDGET(empty));
    gtk_box_append(main_box, GTK_WIDGET(scroll));
    adw_dialog_set_child(dlg, GTK_WIDGET(main_box));

    /* Store policy store for cleanup */
    g_object_set_data_full(G_OBJECT(dlg), "policy-store", ps,
                           (GDestroyNotify)policy_store_free);

    g_signal_connect_swapped(btn_close, "clicked", G_CALLBACK(adw_dialog_close), dlg);
    adw_dialog_present(dlg, GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
    return;
  }

  gtk_scrolled_window_set_child(scroll, GTK_WIDGET(list_box));
  gtk_box_append(main_box, GTK_WIDGET(scroll));

  /* Store policy store for cleanup */
  g_object_set_data_full(G_OBJECT(dlg), "policy-store", ps,
                         (GDestroyNotify)policy_store_free);

  adw_dialog_set_child(dlg, GTK_WIDGET(main_box));
  g_signal_connect_swapped(btn_close, "clicked", G_CALLBACK(adw_dialog_close), dlg);
  adw_dialog_present(dlg, GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}
static void on_listen_notify(GObject *obj, GParamSpec *pspec, gpointer user_data){
  (void)pspec; (void)user_data; GtkSwitch *sw = GTK_SWITCH(obj);
  gboolean active = gtk_switch_get_active(sw);
  g_message("Listen for new connections: %s", active ? "on" : "off");
}

/* User list publish callback - called when user saves and publishes the list.
 * Note: The event_json here is unsigned. We need to sign it before publishing. */
static void on_user_list_publish(UserListType type, const gchar *event_json, gpointer ud) {
  (void)ud;
  const gchar *list_name = (type == USER_LIST_FOLLOWS) ? "follows" : "mutes";
  gint kind = (type == USER_LIST_FOLLOWS) ? 3 : 10000;

  g_message("Publishing %s list (kind:%d)", list_name, kind);

  /* Get the current identity for signing */
  gchar *npub = NULL;
  SecretStoreResult rc = secret_store_get_public_key(NULL, &npub);

  if (rc != SECRET_STORE_OK || !npub || !*npub) {
    g_warning("No account selected - cannot sign %s list for publishing", list_name);
    return;
  }

  /* Sign the event */
  gchar *signature = NULL;
  rc = secret_store_sign_event(event_json, npub, &signature);

  if (rc != SECRET_STORE_OK || !signature) {
    g_warning("Failed to sign %s list event: %s", list_name,
              secret_store_result_to_string(rc));
    g_free(npub);
    return;
  }

  /* The secret_store_sign_event returns the full signed event JSON */
  gchar *event_type = g_strdup_printf("%s list (kind:%d)", list_name, kind);
  publish_signed_event_to_relays(signature, event_type);
  g_free(event_type);

  g_free(signature);
  g_free(npub);
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
  gtk_widget_class_bind_template_child(wc, PageSettings, combo_language);
  gtk_widget_class_bind_template_child(wc, PageSettings, switch_force_high_contrast);
  gtk_widget_class_bind_template_child(wc, PageSettings, combo_high_contrast_variant);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_add_account);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_select_account);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_backup_keys);
  gtk_widget_class_bind_template_child(wc, PageSettings, btn_social_recovery);
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

  /* Initialize theme and language combos with current settings */
  self->updating_theme = TRUE;
  self->updating_language = TRUE;
  self->updating_high_contrast = TRUE;
  SettingsManager *sm = settings_manager_get_default();

  /* Initialize language combo with current setting */
  const gchar *current_lang = gn_i18n_get_language();
  adw_combo_row_set_selected(self->combo_language, language_to_index(current_lang));
  SettingsTheme theme = settings_manager_get_theme(sm);
  /* Map SettingsTheme to combo index: SYSTEM=0, LIGHT=1, DARK=2, HIGH_CONTRAST=3 */
  guint theme_idx;
  switch (theme) {
    case SETTINGS_THEME_LIGHT:
      theme_idx = 1;
      break;
    case SETTINGS_THEME_DARK:
      theme_idx = 2;
      break;
    case SETTINGS_THEME_HIGH_CONTRAST:
      theme_idx = 3;
      break;
    case SETTINGS_THEME_SYSTEM:
    default:
      theme_idx = 0;
      break;
  }
  adw_combo_row_set_selected(self->combo_theme, theme_idx);

  /* Initialize force high contrast switch */
  gboolean force_hc = settings_manager_get_force_high_contrast(sm);
  adw_switch_row_set_active(self->switch_force_high_contrast, force_hc);

  /* Initialize high contrast variant combo */
  SettingsHighContrastVariant hc_variant = settings_manager_get_high_contrast_variant(sm);
  guint hc_variant_idx;
  switch (hc_variant) {
    case SETTINGS_HC_INVERTED:
      hc_variant_idx = 1;
      break;
    case SETTINGS_HC_YELLOW_ON_BLACK:
      hc_variant_idx = 2;
      break;
    case SETTINGS_HC_DEFAULT:
    default:
      hc_variant_idx = 0;
      break;
  }
  adw_combo_row_set_selected(self->combo_high_contrast_variant, hc_variant_idx);

  self->updating_theme = FALSE;
  self->updating_language = FALSE;
  self->updating_high_contrast = FALSE;

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

  /* Connect theme and language combo change handlers */
  g_signal_connect(self->combo_theme, "notify::selected", G_CALLBACK(on_theme_combo_changed), self);
  g_signal_connect(self->combo_language, "notify::selected", G_CALLBACK(on_language_combo_changed), self);
  g_signal_connect(self->switch_force_high_contrast, "notify::active", G_CALLBACK(on_force_high_contrast_changed), self);
  g_signal_connect(self->combo_high_contrast_variant, "notify::selected", G_CALLBACK(on_high_contrast_variant_changed), self);

  /* Connect session settings handlers */
  g_signal_connect(self->combo_lock_timeout, "notify::selected", G_CALLBACK(on_lock_timeout_changed), self);
  g_signal_connect(self->switch_lock_on_idle, "notify::active", G_CALLBACK(on_lock_on_idle_changed), self);
  g_signal_connect(self->combo_client_session_timeout, "notify::selected", G_CALLBACK(on_client_session_timeout_changed), self);
  g_signal_connect(self->btn_manage_sessions, "clicked", G_CALLBACK(on_manage_sessions), self);

  /* Handlers */
  g_signal_connect(self->btn_add_account, "clicked", G_CALLBACK(on_add_account), self);
  g_signal_connect(self->btn_select_account, "clicked", G_CALLBACK(on_select_account), self);
  g_signal_connect(self->btn_backup_keys, "clicked", G_CALLBACK(on_backup_keys), self);
  g_signal_connect(self->btn_social_recovery, "clicked", G_CALLBACK(on_social_recovery), self);
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
