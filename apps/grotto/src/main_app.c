#include "gn-session-bus-probe.h"
#include "main_app.h"
#include <gtk/gtk.h>
#include <adwaita.h>
#include <gio/gio.h>
#include <string.h>
#include "legacy_dirs.h"
#include "signer_grants.h"
#include "accounts_store.h"
#include "settings_manager.h"
#include "startup-timing.h"
#include "gn-status-notifier.h"
#include "secure-mem.h"
#include "i18n.h"
#include "ui/signer-window.h"
#include "ui/onboarding-assistant.h"
#include "approval-controller.h"
#include "background.h"

#define SIGNER_NAME  "org.nostr.Signer"
#define SIGNER_PATH  "/org/nostr/signer"
#define CONTROL_PATH "/org/nostr/signer/grotto"
#define REQUEST_KIND_EVENT "event" /* ApprovalRequested kind for SignEvent */

// Forward declarations for UI pages
GtkWidget *gnostr_home_page_new(void);
GtkWidget *gnostr_settings_page_new(AccountsStore *as);
void gnostr_settings_page_refresh(GtkWidget *page, AccountsStore *as);
void gnostr_settings_open_import_dialog_with_callback(GtkWindow *parent, AccountsStore *as, const char *initial_identity,
                                                      void (*on_success)(const char*, gpointer), gpointer user_data);
// Approval dialog API
#include "ui/approval_dialog.h"

static GtkApplication *global_app;
static GrottoApprovalController *approval_controller;
static gchar *queued_review;
static gchar *queued_deny;
static GCancellable *startup_cancel;
static gboolean shutting_down, setup_held;
static guint setup_timeout;
static GSettings *background_settings;
static GrottoBackground *background;
static guint shell_watch, control_watch, control_signal, control_revision;
static gchar *control_owner;
static gboolean shell_indicator, signer_available, signing_paused, review_activation, desktop_locked;
static void refresh_indicator(void);
static void fetch_status(void);
static void maybe_disclose_background(GtkWindow *win);

static GtkWindow *management_window(GtkApplication *app) {
  for (GList *l = gtk_application_get_windows(app); l; l = l->next)
    if (SIGNER_IS_WINDOW(l->data) || ONBOARDING_IS_ASSISTANT(l->data)) return l->data;
  return NULL;
}

/* Forward declarations */
static void apply_theme_preference(SettingsTheme theme);
static void on_theme_setting_changed(const char *key, gpointer user_data);
static void update_high_contrast_css(gboolean enable);
static void on_system_high_contrast_changed(GObject *obj, GParamSpec *pspec, gpointer user_data);
static gboolean should_use_high_contrast(void);
static void attach_pending_group(GtkWindow *win);

/* Global application instance for callbacks that need it */

static void on_app_preferences(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  g_application_activate(G_APPLICATION(app));
  GtkWindow *win = management_window(app);
  if (!win || !SIGNER_IS_WINDOW(win)) return;
  signer_window_show_page((SignerWindow*)win, "settings");
}

static void on_app_about(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  GtkWindow *parent = gtk_application_get_active_window(app);
  AdwDialog *about = adw_about_dialog_new();
  adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "Grotto");
  adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), "org.nostr.Grotto");
  adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), "0.1.0");
  adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), "https://github.com/chebizarro/nostrc");
  adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about), "https://github.com/chebizarro/nostrc/issues");
  adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
    "Keeps your Nostr keys and signs for all your Nostr apps: local apps over "
    "NIP-55L and remote apps over NIP-46 (Nostr Connect). One Grotto serves "
    "them all; your private keys never leave it.");
  const char *devs[] = { "GNostr Team", NULL };
  adw_about_dialog_set_developers(ADW_ABOUT_DIALOG(about), devs);
  adw_dialog_present(about, parent ? GTK_WIDGET(parent) : NULL);
}

/* Action handler for new profile (Ctrl+N) */
static void on_app_new_profile(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  g_application_activate(G_APPLICATION(app));
  GtkWindow *win = management_window(app);
  if (!win || !SIGNER_IS_WINDOW(win)) return;
  signer_window_show_new_profile((SignerWindow*)win);
}

/* Action handler for import profile (Ctrl+I) */
static void on_app_import_profile(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  GtkWindow *win = gtk_application_get_active_window(app);
  if (!win || !SIGNER_IS_WINDOW(win)) return;
  signer_window_show_import_profile((SignerWindow*)win);
}

/* Action handler for export/backup (Ctrl+E) */
static void on_app_export(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  GtkWindow *win = gtk_application_get_active_window(app);
  if (!win || !SIGNER_IS_WINDOW(win)) return;
  signer_window_show_backup((SignerWindow*)win);
}

/* Action handler for lock session (Ctrl+L) */
static void on_app_lock(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  GtkWindow *win = gtk_application_get_active_window(app);
  if (!win || !SIGNER_IS_WINDOW(win)) return;
  signer_window_lock_session((SignerWindow*)win);
}

/* Global high-contrast CSS provider (loaded once, added/removed as needed) */
static GtkCssProvider *high_contrast_provider = NULL;

/* Apply high-contrast styling to all windows */
static void apply_high_contrast_to_windows(GtkApplication *app, gboolean enable, SettingsHighContrastVariant variant) {
  GList *windows = gtk_application_get_windows(app);
  for (GList *l = windows; l != NULL; l = l->next) {
    GtkWidget *win = GTK_WIDGET(l->data);
    /* Remove existing high-contrast classes */
    gtk_widget_remove_css_class(win, "high-contrast");
    gtk_widget_remove_css_class(win, "inverted");
    gtk_widget_remove_css_class(win, "yellow-on-black");

    if (enable) {
      /* Add base high-contrast class */
      gtk_widget_add_css_class(win, "high-contrast");
      /* Add variant-specific class */
      switch (variant) {
        case SETTINGS_HC_INVERTED:
          gtk_widget_add_css_class(win, "inverted");
          break;
        case SETTINGS_HC_YELLOW_ON_BLACK:
          gtk_widget_add_css_class(win, "yellow-on-black");
          break;
        case SETTINGS_HC_DEFAULT:
        default:
          /* Default uses just .high-contrast, no additional class */
          break;
      }
    }
  }
}

/* Load or unload high-contrast CSS provider */
static void update_high_contrast_css(gboolean enable) {
  GdkDisplay *display = gdk_display_get_default();
  if (!display) return;

  if (enable) {
    if (!high_contrast_provider) {
      high_contrast_provider = gtk_css_provider_new();
      gtk_css_provider_load_from_resource(high_contrast_provider, "/org/gnostr/signer/css/high-contrast.css");
    }
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(high_contrast_provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_debug("High-contrast CSS loaded");
  } else {
    if (high_contrast_provider) {
      gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(high_contrast_provider));
      g_debug("High-contrast CSS unloaded");
    }
  }
}

/* Determine if high contrast should be used based on:
 * 1. Force high contrast setting (user preference)
 * 2. Theme explicitly set to high-contrast
 * 3. System high contrast mode (GNOME/GTK accessibility setting)
 */
static gboolean should_use_high_contrast(void) {
  SettingsManager *sm = settings_manager_get_default();

  /* Check if user has forced high contrast */
  if (settings_manager_get_force_high_contrast(sm)) {
    g_debug("High contrast enabled via force-high-contrast setting");
    return TRUE;
  }

  /* Check if theme is explicitly set to high-contrast */
  SettingsTheme theme = settings_manager_get_theme(sm);
  if (theme == SETTINGS_THEME_HIGH_CONTRAST) {
    g_debug("High contrast enabled via theme=high-contrast");
    return TRUE;
  }

  /* Check system high contrast preference via AdwStyleManager */
  AdwStyleManager *style_manager = adw_style_manager_get_default();
  if (adw_style_manager_get_high_contrast(style_manager)) {
    g_debug("High contrast enabled via system accessibility setting");
    return TRUE;
  }

  return FALSE;
}

/* Callback for system high contrast changes (GNOME accessibility settings) */
static void on_system_high_contrast_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  (void)user_data;

  AdwStyleManager *style_manager = ADW_STYLE_MANAGER(obj);
  gboolean system_hc = adw_style_manager_get_high_contrast(style_manager);

  g_debug("System high contrast changed: %s", system_hc ? "enabled" : "disabled");

  /* Re-evaluate whether to use high contrast */
  gboolean use_hc = should_use_high_contrast();
  update_high_contrast_css(use_hc);

  /* Apply/remove high-contrast classes on windows */
  if (global_app) {
    SettingsManager *sm = settings_manager_get_default();
    SettingsHighContrastVariant hc_variant = settings_manager_get_high_contrast_variant(sm);
    apply_high_contrast_to_windows(global_app, use_hc, hc_variant);
  }
}

/* Callback when onboarding finishes - present main window */
static void on_onboarding_finished(gboolean completed, gpointer user_data) {
  GtkApplication *app = GTK_APPLICATION(user_data);
  g_debug("Onboarding finished: completed=%s", completed ? "true" : "false");

  /* Present the main window now */
  SignerWindow *win = signer_window_new(ADW_APPLICATION(app));

  /* Apply high-contrast class if needed (considers system, force, and theme settings) */
  gboolean use_high_contrast = should_use_high_contrast();
  if (use_high_contrast) {
    SettingsManager *sm = settings_manager_get_default();
    SettingsHighContrastVariant hc_variant = settings_manager_get_high_contrast_variant(sm);
    gtk_widget_add_css_class(GTK_WIDGET(win), "high-contrast");
    switch (hc_variant) {
      case SETTINGS_HC_INVERTED:
        gtk_widget_add_css_class(GTK_WIDGET(win), "inverted");
        break;
      case SETTINGS_HC_YELLOW_ON_BLACK:
        gtk_widget_add_css_class(GTK_WIDGET(win), "yellow-on-black");
        break;
      case SETTINGS_HC_DEFAULT:
      default:
        break;
    }
  }

  attach_pending_group(GTK_WINDOW(win));
  gtk_window_present(GTK_WINDOW(win));
  maybe_disclose_background(GTK_WINDOW(win));
}

static void background_description(GrottoBackground *self, AdwPreferencesGroup *group) {
  adw_preferences_group_set_description(group, grotto_background_get_description(self));
}
static void background_permission(GtkButton *button, gpointer data) {
  (void)data;
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
  if (GTK_IS_WINDOW(root)) maybe_disclose_background(GTK_WINDOW(root));
}
static void background_choice(AdwSwitchRow *row, GParamSpec *pspec, gpointer data) {
  (void)pspec; (void)data;
  if (adw_switch_row_get_active(row))
    g_settings_set_boolean(background_settings, "background-disclosure-accepted", TRUE);
}
static void attach_pending_group(GtkWindow *win) {
  if (SIGNER_IS_WINDOW(win) && background &&
      !g_object_get_data(G_OBJECT(win), "grotto-background-group")) {
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Background and signing");
    AdwActionRow *permission = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(permission), "Allow background operation");
    adw_action_row_set_subtitle(permission, "Review the setup disclosure before requesting desktop permission");
    GtkWidget *button = gtk_button_new_with_label("Review");
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(permission, button);
    g_signal_connect(button, "clicked", G_CALLBACK(background_permission), NULL);
    g_settings_bind(background_settings, "background-disclosure-accepted", permission, "visible",
                    G_SETTINGS_BIND_GET | G_SETTINGS_BIND_INVERT_BOOLEAN);
    adw_preferences_group_add(group, GTK_WIDGET(permission));
    const char *keys[] = { "run-in-background", "launch-on-login", "pause-on-screen-lock" };
    const char *titles[] = { "Review requests in the background", "Launch on login", "Pause signing on screen lock" };
    const char *subtitles[] = {
      "Keep Grotto available after closing its window; your desktop may ask permission",
      "Start without opening a window; requires background mode",
      "Unlocking does not resume signing. Resume explicitly in Grotto." };
    for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
      AdwSwitchRow *row = ADW_SWITCH_ROW(adw_switch_row_new());
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), titles[i]);
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitles[i]);
      g_settings_bind(background_settings, keys[i], row, "active", G_SETTINGS_BIND_DEFAULT);
      if (i == 0) g_signal_connect(row, "notify::active", G_CALLBACK(background_choice), NULL);
      if (i == 1) g_settings_bind(background_settings, "run-in-background", row, "sensitive", G_SETTINGS_BIND_GET);
      adw_preferences_group_add(group, GTK_WIDGET(row));
    }
    background_description(background, group);
    g_signal_connect_object(background, "state-changed", G_CALLBACK(background_description), group, 0);
    signer_window_add_background_group(SIGNER_WINDOW(win), group);
    g_object_set_data(G_OBJECT(win), "grotto-background-group", group);
  }
  if (!SIGNER_IS_WINDOW(win) || !approval_controller ||
      g_object_get_data(G_OBJECT(win), "grotto-pending-group")) return;
  GtkWidget *group = grotto_approval_pending_group_new(approval_controller);
  signer_window_set_pending_group(SIGNER_WINDOW(win), group);
  g_object_set_data(G_OBJECT(win), "grotto-pending-group", group);
}
static GtkWindow *present_pending(GtkApplication *app) {
  GtkWindow *win = NULL;
  for (GList *w = gtk_application_get_windows(app); w; w = w->next)
    if (SIGNER_IS_WINDOW(w->data)) { win = w->data; break; }
  if (!win) {
    g_application_activate(G_APPLICATION(app));
    for (GList *w = gtk_application_get_windows(app); w; w = w->next)
      if (SIGNER_IS_WINDOW(w->data)) { win = w->data; break; }
    if (!win) win = GTK_WINDOW(signer_window_new(ADW_APPLICATION(app)));
  }
  attach_pending_group(win);
  signer_window_show_page(SIGNER_WINDOW(win), "permissions");
  gtk_window_present(win);
  return win;
}
/* Global deferred state for D-Bus connection */
static GDBusConnection *deferred_dbus_conn = NULL;
static void on_approval_message(GrottoApprovalController *controller, const char *message, gpointer data) {
  (void)controller; GtkApplication *app = data;
  if (shutting_down) return;
  GtkWindow *win = gtk_application_get_active_window(app);
  if (g_strcmp0(message, "That request is no longer waiting.") == 0 ||
      g_strcmp0(message, "No requests are waiting.") == 0) {
    review_activation = TRUE;
    win = present_pending(app);
    review_activation = FALSE;
  }
  if (win) {
    /* The existing default page hosts pending review until G10 supplies Home. */
    AdwDialog *alert = adw_alert_dialog_new("Grotto", message);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", "Close");
    adw_dialog_present(alert, GTK_WIDGET(win));
  }
}
static void on_review_request(GSimpleAction *action, GVariant *parameter, gpointer data) {
  (void)action; (void)data; const char *id = g_variant_get_string(parameter, NULL);
  review_activation = TRUE;
  present_pending(global_app);
  review_activation = FALSE;
  if (!*id) {
    g_clear_pointer(&queued_review, g_free);
    if (approval_controller) grotto_approval_controller_review(approval_controller, "");
  } else if (approval_controller) grotto_approval_controller_review(approval_controller, id);
  else { g_free(queued_review); queued_review = g_strdup(id); }
}
static void deny_done(GObject *source, GAsyncResult *result, gpointer data) {
  g_autoptr(GApplication) app = data;
  g_autoptr(GError) error = NULL;
  if (!grotto_approval_controller_decide_finish(GROTTO_APPROVAL_CONTROLLER(source), result, &error))
    on_approval_message(GROTTO_APPROVAL_CONTROLLER(source), "That request is no longer waiting.", app);
}
static void on_reconciled(GrottoApprovalController *controller, gboolean success, gpointer data) {
  (void)data;
  if (queued_deny) {
    g_autofree gchar *id = g_steal_pointer(&queued_deny);
    if (success)
      grotto_approval_controller_decide_async(controller, id, FALSE, FALSE, 0, NULL, deny_done, g_object_ref(global_app));
    else
      on_approval_message(controller, "That request is no longer waiting.", global_app);
  }
}
static void on_deny_request(GSimpleAction *action, GVariant *parameter, gpointer data) {
  (void)action; (void)data;
  review_activation = TRUE;
  present_pending(global_app);
  review_activation = FALSE;
  g_free(queued_deny); queued_deny = g_strdup(g_variant_get_string(parameter, NULL));
  if (approval_controller) grotto_approval_controller_reconcile(approval_controller);
}
static void on_shutdown(GApplication *app, gpointer data) {
  (void)data;
  shutting_down = TRUE;
  if (setup_timeout) { g_source_remove(setup_timeout); setup_timeout = 0; }
  if (startup_cancel) g_cancellable_cancel(startup_cancel);
  g_clear_object(&startup_cancel);
  if (setup_held) { setup_held = FALSE; g_application_release(app); }
  if (background) grotto_background_stop(background);
  if (shell_watch) { g_bus_unwatch_name(shell_watch); shell_watch = 0; }
  if (control_watch) { g_bus_unwatch_name(control_watch); control_watch = 0; }
  if (control_signal) { g_dbus_connection_signal_unsubscribe(deferred_dbus_conn, control_signal); control_signal = 0; }
  control_revision++;
  g_clear_pointer(&control_owner, g_free);
  if (approval_controller) grotto_approval_controller_stop(approval_controller);
  g_clear_pointer(&queued_review, g_free); g_clear_pointer(&queued_deny, g_free);
  g_clear_object(&deferred_dbus_conn);
}

/* Callback when async D-Bus connection completes */
static void start_desktop_observation(void);
static void on_dbus_connected(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  g_autoptr(GApplication) app = user_data;
  g_autoptr(GError) err = NULL;
  g_autoptr(GDBusConnection) bus = g_bus_get_finish(res, &err);
  if (setup_timeout) { g_source_remove(setup_timeout); setup_timeout = 0; }
  if (setup_held) { setup_held = FALSE; g_application_release(app); }
  STARTUP_TIME_END(STARTUP_PHASE_DBUS);
  if (shutting_down) return;
  if (!bus) {
    g_warning("Deferred D-Bus connection failed: %s", err ? err->message : "unavailable");
    return;
  }
  deferred_dbus_conn = g_steal_pointer(&bus);
  g_debug("D-Bus connection established in deferred init");
  approval_controller = grotto_approval_controller_new(G_APPLICATION(global_app), deferred_dbus_conn);
  g_object_set_data_full(G_OBJECT(global_app), "grotto-approval-controller", approval_controller, g_object_unref);
  g_signal_connect(approval_controller, "message", G_CALLBACK(on_approval_message), global_app);
  g_signal_connect(approval_controller, "reconciled", G_CALLBACK(on_reconciled), global_app);
  for (GList *w = gtk_application_get_windows(global_app); w; w = w->next)
    attach_pending_group(w->data);
  if (queued_review) {
    grotto_approval_controller_review(approval_controller, queued_review);
    g_clear_pointer(&queued_review, g_free);
  }

  background = grotto_background_new(G_APPLICATION(global_app), deferred_dbus_conn,
      background_settings, !g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS) && !g_getenv("SNAP"));
  g_object_set_data_full(G_OBJECT(global_app), "grotto-background", background, g_object_unref);
  for (GList *w = gtk_application_get_windows(global_app); w; w = w->next)
    attach_pending_group(w->data);
  start_desktop_observation();
  STARTUP_TIME_END(STARTUP_PHASE_DBUS);
}

static gboolean cancel_setup(gpointer data) {
  (void)data; setup_timeout = 0;
  if (startup_cancel) g_cancellable_cancel(startup_cancel);
  return G_SOURCE_REMOVE;
}

/* Deferred initialization callback - runs after window is presented */
static gboolean deferred_init_cb(gpointer user_data) {
  (void)user_data;
  static gboolean done;
  if (done) return G_SOURCE_REMOVE;
  done = TRUE;

  gint64 deferred_start = startup_timing_measure_start();

  if (shutting_down) return G_SOURCE_REMOVE;
  /* Approvals use the daemon's pinned identity, never a second GUI account
   * store or eager keyring enumeration. Management owns its account model. */
  STARTUP_TIME_BEGIN(STARTUP_PHASE_DBUS);
  setup_timeout = g_timeout_add_seconds(5, cancel_setup, NULL);
  g_bus_get(G_BUS_TYPE_SESSION, startup_cancel, on_dbus_connected, g_object_ref(global_app));

  startup_timing_measure_end(deferred_start, "deferred-init-scheduled", 50);

  return G_SOURCE_REMOVE;  /* Don't repeat */
}

/* Runs whether the app was launched by the person or started by the bus for
 * a request (--gapplication-service, which never activates): the signer
 * subscription must exist in both cases. */
/* W32 (owner decision): an app indicator where a StatusNotifierWatcher runs
 * (Ubuntu's AppIndicator extension, KDE); nothing on stock GNOME. */
static void indicator_activate(GnStatusNotifier *indicator, GApplication *app) {
  (void)indicator;
  g_application_activate(app);
}
static void indicator_quit(GnStatusNotifier *indicator, GApplication *app) {
  (void)indicator;
  g_application_quit(app);
}

static void pause_done(GObject *source, GAsyncResult *result, gpointer data) {
  g_autoptr(GApplication) app = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (!reply && !shutting_down && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_warning("Could not change signing pause: %s", error->message);
    GtkWindow *win = management_window(GTK_APPLICATION(app));
    if (win) on_approval_message(approval_controller, "Signing pause could not be changed.", app);
  }
}
static void set_signing_paused(gboolean paused) {
  if (!control_owner || shutting_down) return;
  g_dbus_connection_call(deferred_dbus_conn, control_owner, CONTROL_PATH,
      "org.nostr.Grotto.Control1", "SetPaused", g_variant_new("(b)", paused), G_VARIANT_TYPE_UNIT,
      G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, startup_cancel, pause_done, g_object_ref(global_app));
}
static void on_pause_signing(GSimpleAction *action, GVariant *parameter, gpointer data) {
  (void)action; (void)parameter; (void)data;
  if (signer_available) set_signing_paused(!signing_paused);
}
static void screen_lock_changed(GrottoApprovalController *controller, gboolean locked, gpointer data) {
  (void)controller; (void)data;
  desktop_locked = locked;
  if (locked && g_settings_get_boolean(background_settings, "pause-on-screen-lock"))
    set_signing_paused(TRUE); /* Never auto-resume on unlock. */
}
static void pause_lock_policy_changed(GSettings *settings, const char *key, gpointer data) {
  (void)key; (void)data;
  if (desktop_locked && g_settings_get_boolean(settings, "pause-on-screen-lock"))
    set_signing_paused(TRUE);
}
static void indicator_item(GnStatusNotifier *indicator, gint id, gpointer data) {
  (void)indicator; (void)data;
  if (id == 3) g_action_group_activate_action(G_ACTION_GROUP(global_app), "review-request", g_variant_new_string(""));
  else if (id == 4) g_action_group_activate_action(G_ACTION_GROUP(global_app), "pause-signing", NULL);
}
static void refresh_indicator(void) {
  if (shutting_down) return;
  guint pending = approval_controller ? g_list_model_get_n_items(grotto_approval_controller_get_pending(approval_controller)) : 0;
  GnStatusNotifier *indicator = g_object_get_data(G_OBJECT(global_app), "grotto-indicator");
  if (indicator) {
    g_autofree gchar *title = !signer_available ? g_strdup("Grotto · Signer unavailable") :
        signing_paused ? g_strdup("Grotto · Signing paused") : pending ?
        g_strdup_printf("Grotto · Waiting (%u)", pending) : g_strdup("Grotto · Ready");
    gn_status_notifier_set_presentation(indicator, !shell_indicator, title, "org.nostr.Grotto-symbolic");
    GnStatusNotifierItem menu[] = {
      { 5, title, FALSE, TRUE, FALSE, -1 },
      { 1, "Open Grotto", TRUE, TRUE, FALSE, -1 },
      { 3, "Review requests", TRUE, TRUE, FALSE, -1 },
      { 4, signing_paused ? "Resume signing" : "Pause signing", signer_available, TRUE, FALSE, -1 },
      { 2, "Quit approval UI (authorised signing continues)", TRUE, TRUE, FALSE, -1 }
    };
    gn_status_notifier_set_menu_items(indicator, menu, G_N_ELEMENTS(menu));
  }
  GAction *pause = g_action_map_lookup_action(G_ACTION_MAP(global_app), "pause-signing");
  if (pause) {
    g_simple_action_set_enabled(G_SIMPLE_ACTION(pause), signer_available);
    g_simple_action_set_state(G_SIMPLE_ACTION(pause), g_variant_new_boolean(signing_paused));
  }
  if (background) grotto_background_set_status(background, pending != 0, signing_paused);
}
static void pending_changed(GListModel *model, guint position, guint removed, guint added, gpointer data) {
  (void)model; (void)position; (void)removed; (void)added; (void)data; refresh_indicator();
}
static void status_done(GObject *source, GAsyncResult *result, gpointer data) {
  guint revision = GPOINTER_TO_UINT(data);
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  if (shutting_down || revision != control_revision) return;
  signer_available = reply != NULL;
  signing_paused = FALSE;
  if (reply) {
    g_autoptr(GVariant) values = NULL; g_variant_get(reply, "(@a{sv})", &values);
    g_variant_lookup(values, "paused", "b", &signing_paused);
  }
  refresh_indicator();
}
static void fetch_status(void) {
  if (!control_owner || shutting_down) return;
  control_revision++;
  g_dbus_connection_call(deferred_dbus_conn, control_owner, CONTROL_PATH,
      "org.nostr.Grotto.Control1", "GetStatus", NULL, G_VARIANT_TYPE("(a{sv})"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, startup_cancel, status_done, GUINT_TO_POINTER(control_revision));
}
static void control_changed(GDBusConnection *bus, const char *sender, const char *path,
    const char *interface, const char *signal, GVariant *args, gpointer data) {
  (void)bus; (void)path; (void)interface; (void)signal; (void)args; (void)data;
  if (!g_strcmp0(sender, control_owner)) fetch_status();
}
static void control_appeared(GDBusConnection *bus, const char *name, const char *owner, gpointer data) {
  (void)bus; (void)name; (void)data;
  g_free(control_owner); control_owner = g_strdup(owner);
  if (desktop_locked && g_settings_get_boolean(background_settings, "pause-on-screen-lock"))
    set_signing_paused(TRUE);
  fetch_status();
}
static void control_vanished(GDBusConnection *bus, const char *name, gpointer data) {
  (void)bus; (void)name; (void)data;
  control_revision++; g_clear_pointer(&control_owner, g_free);
  signer_available = FALSE; signing_paused = FALSE; refresh_indicator();
}
static void shell_appeared(GDBusConnection *bus, const char *name, const char *owner, gpointer data) {
  (void)bus; (void)name; (void)owner; (void)data; shell_indicator = TRUE; refresh_indicator();
}
static void shell_vanished(GDBusConnection *bus, const char *name, gpointer data) {
  (void)bus; (void)name; (void)data; shell_indicator = FALSE; refresh_indicator();
}
static void start_desktop_observation(void) {
  GnStatusNotifier *indicator = g_object_get_data(G_OBJECT(global_app), "grotto-indicator");
  g_signal_connect(indicator, "item-activated", G_CALLBACK(indicator_item), NULL);
  g_signal_connect(approval_controller, "screen-lock-changed", G_CALLBACK(screen_lock_changed), NULL);
  g_signal_connect(background_settings, "changed::pause-on-screen-lock", G_CALLBACK(pause_lock_policy_changed), NULL);
  g_signal_connect(grotto_approval_controller_get_pending(approval_controller), "items-changed", G_CALLBACK(pending_changed), NULL);
  control_signal = g_dbus_connection_signal_subscribe(deferred_dbus_conn, SIGNER_NAME,
      "org.nostr.Grotto.Control1", "StatusChanged", CONTROL_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, control_changed, NULL, NULL);
  control_watch = g_bus_watch_name_on_connection(deferred_dbus_conn, SIGNER_NAME,
      G_BUS_NAME_WATCHER_FLAGS_NONE, control_appeared, control_vanished, NULL, NULL);
  shell_watch = g_bus_watch_name_on_connection(deferred_dbus_conn, "org.nostr.Shell.Signer1",
      G_BUS_NAME_WATCHER_FLAGS_NONE, shell_appeared, shell_vanished, NULL, NULL);
}

static void on_startup(GtkApplication *app, gpointer user_data) {
  (void)user_data;
  background_settings = g_settings_new("org.nostr.Grotto");
  grotto_background_migrate_settings(background_settings);
  g_object_set_data_full(G_OBJECT(app), "grotto-background-settings", background_settings, g_object_unref);
  startup_cancel = g_cancellable_new();
  g_application_hold(G_APPLICATION(app)); setup_held = TRUE;
  g_idle_add(deferred_init_cb, NULL);
  GnStatusNotifier *indicator = gn_status_notifier_new(G_APPLICATION(app), "org.nostr.Grotto-symbolic", "Grotto");
  g_signal_connect(indicator, "activate", G_CALLBACK(indicator_activate), app);
  g_signal_connect(indicator, "quit", G_CALLBACK(indicator_quit), app);
  g_object_set_data_full(G_OBJECT(app), "grotto-indicator", indicator, g_object_unref);
  /* Keep the opportunistic notifier passive until Shell ownership is known. */
  gn_status_notifier_set_presentation(indicator, FALSE, "Grotto · Signer unavailable", "org.nostr.Grotto-symbolic");
}

static void on_activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;

  GtkWindow *existing = management_window(app);
  if (existing) {
    attach_pending_group(existing);
    if (SIGNER_IS_WINDOW(existing) && approval_controller &&
        g_list_model_get_n_items(grotto_approval_controller_get_pending(approval_controller)))
      signer_window_show_page(SIGNER_WINDOW(existing), "permissions");
    gtk_window_present(existing);
    if (approval_controller) grotto_approval_controller_reconcile(approval_controller);
    return;
  }
  STARTUP_TIME_MARK("activate-start");

  /* Initialize settings manager and apply theme preference after GTK is initialized */
  STARTUP_TIME_BEGIN(STARTUP_PHASE_SETTINGS);
  SettingsManager *sm = settings_manager_get_default();

  /* Preload startup-critical settings in batch to reduce D-Bus round trips */
  settings_manager_preload_startup_settings(sm);

  SettingsTheme initial_theme = settings_manager_get_theme(sm);
  STARTUP_TIME_END(STARTUP_PHASE_SETTINGS);

  STARTUP_TIME_BEGIN(STARTUP_PHASE_THEME);
  apply_theme_preference(initial_theme);
  STARTUP_TIME_END(STARTUP_PHASE_THEME);

  /* Listen for theme setting changes */
  settings_manager_connect_changed(sm, "theme", on_theme_setting_changed, NULL);
  settings_manager_connect_changed(sm, "high-contrast-variant", on_theme_setting_changed, NULL);
  settings_manager_connect_changed(sm, "force-high-contrast", on_theme_setting_changed, NULL);

  /* Listen for system high contrast changes (GNOME accessibility settings) */
  AdwStyleManager *style_manager = adw_style_manager_get_default();
  g_signal_connect(style_manager, "notify::high-contrast",
                   G_CALLBACK(on_system_high_contrast_changed), NULL);

  /* Load application stylesheet from resources */
  STARTUP_TIME_BEGIN(STARTUP_PHASE_CSS);
  GtkCssProvider *prov = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(prov, "/org/gnostr/signer/css/app.css");
  GdkDisplay *display = gdk_display_get_default();
  if (display) {
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
  g_object_unref(prov);
  STARTUP_TIME_END(STARTUP_PHASE_CSS);

  /* Load high-contrast CSS if needed (considers system, force, and theme settings) */
  gboolean use_high_contrast = should_use_high_contrast();
  if (use_high_contrast) {
    update_high_contrast_css(use_high_contrast);
  }

  /* Check if onboarding should be shown (first run) - this is fast INI file check */
  STARTUP_TIME_MARK("onboarding-check-start");
  if (onboarding_assistant_check_should_show()) {
    g_debug("First run detected, showing onboarding wizard");

    /* Ensure OnboardingAssistant type is registered */
    g_type_ensure(TYPE_ONBOARDING_ASSISTANT);

    OnboardingAssistant *onboarding = onboarding_assistant_new();
    g_debug("Created onboarding assistant: %p", onboarding);
    onboarding_assistant_set_on_finished(onboarding, on_onboarding_finished, app);
    gtk_application_add_window(app, GTK_WINDOW(onboarding));
    gtk_window_present(GTK_WINDOW(onboarding));

    /* Schedule deferred init */
    g_idle_add(deferred_init_cb, NULL);
    return;
  }

  /* Not first run - present main window directly */
  STARTUP_TIME_BEGIN(STARTUP_PHASE_WINDOW);
  SignerWindow *win = signer_window_new(ADW_APPLICATION(app));
  g_debug("Created main window: %p", win);

  /* Apply high-contrast class if needed */
  if (use_high_contrast) {
    SettingsHighContrastVariant hc_variant = settings_manager_get_high_contrast_variant(sm);
    gtk_widget_add_css_class(GTK_WIDGET(win), "high-contrast");
    switch (hc_variant) {
      case SETTINGS_HC_INVERTED:
        gtk_widget_add_css_class(GTK_WIDGET(win), "inverted");
        break;
      case SETTINGS_HC_YELLOW_ON_BLACK:
        gtk_widget_add_css_class(GTK_WIDGET(win), "yellow-on-black");
        break;
      case SETTINGS_HC_DEFAULT:
      default:
        break;
    }
  }

  attach_pending_group(GTK_WINDOW(win));
  gtk_window_present(GTK_WINDOW(win));
  if (!review_activation) maybe_disclose_background(GTK_WINDOW(win));
  if (approval_controller) grotto_approval_controller_reconcile(approval_controller);
  STARTUP_TIME_END(STARTUP_PHASE_WINDOW);

  STARTUP_TIME_MARK("window-presented");

  /* Schedule deferred initialization for non-critical subsystems */
  g_idle_add(deferred_init_cb, NULL);
}

static void on_close_window(GSimpleAction *action, GVariant *param, gpointer data) {
  (void)action; (void)param;
  GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(data));
  if (win) {
    g_object_ref(win);
    gtk_window_close(win);
    /* AdwWindow can consume close to dismiss its modal dialog. Ctrl+W is
     * explicitly window-close, not hide-or-retain; disposal saves geometry. */
    gtk_window_destroy(win);
    g_object_unref(win);
  }
}
static void disclosure_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)dialog; (void)data;
  if (shutting_down) return;
  g_settings_set_boolean(background_settings, "run-in-background", !strcmp(response, "enable"));
  g_settings_set_boolean(background_settings, "background-disclosure-accepted", TRUE);
}
static void maybe_disclose_background(GtkWindow *win) {
  if (!background_settings || review_activation ||
      g_settings_get_boolean(background_settings, "background-disclosure-accepted")) return;
  AdwDialog *dialog = adw_alert_dialog_new("Review requests in the background?",
      "Closing a window destroys it. Grotto can keep reviewing requests without a window, "
      "subject to desktop permission. Launch on login stays off. Quitting the approval UI "
      "does not pause already-authorised signing; a later request can start Grotto again.");
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "disable", "Only when requested",
                                "enable", "Run in background", NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "enable");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "disable");
  g_signal_connect(dialog, "response", G_CALLBACK(disclosure_response), NULL);
  adw_dialog_present(dialog, GTK_WIDGET(win));
}
/* G06 integration seam: never parse a token or contact a relay here.
 * TODO(nostrc-u6v55.7/G06): hand this URI to the identity-first pairing entry point. */
static void open_pairing_uri(GtkApplication *app, const char *uri) {
  (void)uri;
  review_activation = TRUE;
  present_pending(app);
  review_activation = FALSE;
  on_approval_message(approval_controller,
      "Pairing from a link is not available yet. Review requests here; no relay has been contacted.", app);
}
static void on_open(GApplication *app, GFile **files, gint count, const char *hint, gpointer data) {
  (void)hint; (void)data;
  for (gint i = 0; i < count; i++) {
    g_autofree gchar *uri = g_file_get_uri(files[i]);
    g_autofree gchar *scheme = g_uri_parse_scheme(uri);
    if (scheme && (!g_ascii_strcasecmp(scheme, "bunker") || !g_ascii_strcasecmp(scheme, "nostrconnect")))
      open_pairing_uri(GTK_APPLICATION(app), uri);
    else if (scheme && !g_ascii_strcasecmp(scheme, "grotto"))
      g_application_activate(app);
    else
      g_warning("Grotto does not handle this URI scheme"); /* Never log pairing secrets. */
  }
}
static gint on_command_line(GApplication *app, GApplicationCommandLine *command, gpointer data) {
  (void)data;
  GVariantDict *options = g_application_command_line_get_options_dict(command);
  if (g_variant_dict_contains(options, "new-identity"))
    g_action_group_activate_action(G_ACTION_GROUP(app), "new-identity", NULL);
  else if (g_variant_dict_contains(options, "settings"))
    g_action_group_activate_action(G_ACTION_GROUP(app), "settings", NULL);
  else {
    gint argc;
    g_auto(GStrv) argv = g_application_command_line_get_arguments(command, &argc);
    if (argc > 1) {
      g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_object_unref);
      for (gint i = 1; i < argc; i++)
        g_ptr_array_add(files, g_application_command_line_create_file_for_arg(command, argv[i]));
      g_application_open(app, (GFile **)files->pdata, files->len, "");
    } else g_application_activate(app);
  }
  return 0;
}
static void on_app_quit(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  g_application_quit(G_APPLICATION(app));
}

/* Apply theme preference to AdwStyleManager and handle high-contrast */
static void apply_theme_preference(SettingsTheme theme) {
  AdwStyleManager *style_manager = adw_style_manager_get_default();
  AdwColorScheme color_scheme;

  /* Determine if high contrast should be used (considers system, force, and theme settings) */
  gboolean use_high_contrast = should_use_high_contrast();

  switch (theme) {
    case SETTINGS_THEME_LIGHT:
      color_scheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
      break;
    case SETTINGS_THEME_DARK:
      color_scheme = ADW_COLOR_SCHEME_FORCE_DARK;
      break;
    case SETTINGS_THEME_HIGH_CONTRAST:
      /* High contrast uses force-light as base for Black-on-White variant,
       * but the CSS overrides will take effect */
      color_scheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
      break;
    case SETTINGS_THEME_SYSTEM:
    default:
      color_scheme = ADW_COLOR_SCHEME_DEFAULT;
      break;
  }

  adw_style_manager_set_color_scheme(style_manager, color_scheme);

  /* Load/unload high-contrast CSS */
  update_high_contrast_css(use_high_contrast);

  /* Apply/remove high-contrast classes on windows */
  if (global_app) {
    SettingsManager *sm = settings_manager_get_default();
    SettingsHighContrastVariant hc_variant = settings_manager_get_high_contrast_variant(sm);
    apply_high_contrast_to_windows(global_app, use_high_contrast, hc_variant);
  }

  g_debug("Theme applied: %d -> color_scheme=%d, high_contrast=%s", theme, color_scheme,
          use_high_contrast ? "true" : "false");
}

/* Callback for theme setting changes */
static void on_theme_setting_changed(const gchar *key, gpointer user_data) {
  (void)user_data;
  if (g_strcmp0(key, "theme") != 0 &&
      g_strcmp0(key, "high-contrast-variant") != 0 &&
      g_strcmp0(key, "force-high-contrast") != 0) return;

  SettingsManager *sm = settings_manager_get_default();
  SettingsTheme theme = settings_manager_get_theme(sm);
  apply_theme_preference(theme);
}

/* Callback when re-running onboarding finishes - nothing special needed */
static void on_rerun_onboarding_finished(gboolean completed, gpointer user_data) {
  (void)user_data;
  g_debug("Re-run onboarding finished: completed=%s", completed ? "true" : "false");
  /* Main window already exists, just let onboarding close */
}

/* Action handler to re-run onboarding (from settings menu) */
static void on_app_show_onboarding(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);

  g_debug("Re-running onboarding wizard from settings");

  /* Reset onboarding state so it can be run again */
  onboarding_assistant_reset();

  /* Ensure OnboardingAssistant type is registered */
  g_type_ensure(TYPE_ONBOARDING_ASSISTANT);

  OnboardingAssistant *onboarding = onboarding_assistant_new();
  onboarding_assistant_set_on_finished(onboarding, on_rerun_onboarding_finished, app);

  /* Set as transient to active window */
  GtkWindow *active_win = gtk_application_get_active_window(app);
  if (active_win) {
    gtk_window_set_transient_for(GTK_WINDOW(onboarding), active_win);
  }

  gtk_application_add_window(app, GTK_WINDOW(onboarding));
  gtk_window_present(GTK_WINDOW(onboarding));
}

/* Action handler for showing keyboard shortcuts help overlay (Ctrl+?) */
static void on_app_show_shortcuts(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  GtkWindow *win = gtk_application_get_active_window(app);
  if (!win) return;

  /* Load shortcuts window from resource */
  GtkBuilder *builder = gtk_builder_new_from_resource("/org/gnostr/signer/ui/shortcuts-window.ui");
  GtkShortcutsWindow *shortcuts = GTK_SHORTCUTS_WINDOW(gtk_builder_get_object(builder, "shortcuts_window"));

  if (shortcuts) {
    gtk_window_set_transient_for(GTK_WINDOW(shortcuts), win);
    gtk_window_present(GTK_WINDOW(shortcuts));
  }

  g_object_unref(builder);
}

#ifndef GROTTO_APP_NO_MAIN
int main(int argc, char **argv) {
  /* Initialize startup timing first thing */
  startup_timing_init();
  STARTUP_TIME_BEGIN(STARTUP_PHASE_INIT);

  /* W31: carry the pre-rename directories over before anything reads them. */
  grotto_legacy_dirs_migrate();

  /* Initialize secure memory subsystem for handling sensitive data
   * (private keys, passwords, session tokens, etc.) */
  gnostr_secure_mem_init();

  /* Initialize internationalization before GTK */
  gn_i18n_init();

  g_set_prgname("grotto");
  /* nostrc-py0hg: registration connects to the session bus; one that never
   * answers AUTH (a dead launchd dbus agent on macOS) would hang here before
   * any window. Run without uniqueness instead. */
  GApplicationFlags app_flags = G_APPLICATION_DEFAULT_FLAGS;
  if (gn_session_bus_fallback_if_unresponsive(5000)) {
    g_message("Grotto: the session bus did not respond within 5 s; "
              "running without D-Bus");
    app_flags |= G_APPLICATION_NON_UNIQUE;
  }
  AdwApplication *app = grotto_application_new(app_flags);
  STARTUP_TIME_END(STARTUP_PHASE_INIT);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  gnostr_secure_mem_shutdown();
  return status;
}
#endif

AdwApplication *grotto_application_new(GApplicationFlags app_flags) {
  AdwApplication *app = adw_application_new("org.nostr.Grotto", app_flags | G_APPLICATION_HANDLES_OPEN | G_APPLICATION_HANDLES_COMMAND_LINE);
  g_application_add_main_option(G_APPLICATION(app), "new-identity", 0, G_OPTION_FLAG_NONE,
      G_OPTION_ARG_NONE, "Create or import an identity", NULL);
  g_application_add_main_option(G_APPLICATION(app), "settings", 0, G_OPTION_FLAG_NONE,
      G_OPTION_ARG_NONE, "Open settings", NULL);

  /* Store global app reference for theme change callbacks */
  global_app = GTK_APPLICATION(app);

  /* Settings will be initialized in on_activate after GTK is ready */

  /* Install app actions */
  static const GActionEntry app_entries[] = {
    { "quit", on_app_quit, NULL, NULL, NULL },
    { "close-window", on_close_window, NULL, NULL, NULL },
    { "new-identity", on_app_new_profile, NULL, NULL, NULL },
    { "settings", on_app_preferences, NULL, NULL, NULL },
    { "pause-signing", on_pause_signing, NULL, "false", NULL },
    { "review-request", on_review_request, "s", NULL, NULL },
    { "deny-request", on_deny_request, "s", NULL, NULL },
    { "preferences", on_app_preferences, NULL, NULL, NULL },
    { "about", on_app_about, NULL, NULL, NULL },
    { "new-profile", on_app_new_profile, NULL, NULL, NULL },
    { "import-profile", on_app_import_profile, NULL, NULL, NULL },
    { "export", on_app_export, NULL, NULL, NULL },
    { "lock", on_app_lock, NULL, NULL, NULL },
    { "show-onboarding", on_app_show_onboarding, NULL, NULL, NULL },
    { "show-shortcuts", on_app_show_shortcuts, NULL, NULL, NULL },
  };
  g_action_map_add_action_entries(G_ACTION_MAP(app), app_entries, G_N_ELEMENTS(app_entries), app);

  const char *close_accels[] = { "<Primary>w", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.close-window", close_accels);

  /* Register keyboard accelerators */
  const char *quit_accels[] = { "<Primary>q", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);

  const char *prefs_accels[] = { "<Primary>comma", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.preferences", prefs_accels);

  const char *new_profile_accels[] = { "<Primary>n", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.new-profile", new_profile_accels);

  const char *import_accels[] = { "<Primary>i", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.import-profile", import_accels);

  const char *export_accels[] = { "<Primary>e", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.export", export_accels);

  const char *lock_accels[] = { "<Primary>l", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.lock", lock_accels);

  const char *about_accels[] = { "F1", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.about", about_accels);

  const char *shortcuts_accels[] = { "<Primary>question", NULL };
  gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.show-shortcuts", shortcuts_accels);

  g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), NULL);
  g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
  g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  return app;
}
