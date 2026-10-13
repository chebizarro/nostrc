#include "gn-session-bus-probe.h"
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

#define SIGNER_NAME  "org.nostr.Signer"
#define SIGNER_PATH  "/org/nostr/signer"
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

/* Global application instance for callbacks that need it */

static void on_app_preferences(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  GtkApplication *app = GTK_APPLICATION(user_data);
  GtkWindow *win = gtk_application_get_active_window(app);
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
  GtkWindow *win = gtk_application_get_active_window(app);
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

  gtk_window_present(GTK_WINDOW(win));
}

/* Global deferred state for D-Bus connection */
static GDBusConnection *deferred_dbus_conn = NULL;
static void on_approval_message(GrottoApprovalController *controller, const char *message, gpointer data) {
  (void)controller; GtkApplication *app = data;
  if (shutting_down) return;
  GtkWindow *win = gtk_application_get_active_window(app);
  if (g_strcmp0(message, "That request is no longer waiting.") == 0 ||
      g_strcmp0(message, "No requests are waiting.") == 0) {
    g_application_activate(G_APPLICATION(app)); win = management_window(app);
  }
  if (win) {
    /* G10 hosts the pending group on Home; retain today's management page. */
    AdwDialog *alert = adw_alert_dialog_new("Grotto", message);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", "Close");
    adw_dialog_present(alert, GTK_WIDGET(win));
  }
}
static void on_review_request(GSimpleAction *action, GVariant *parameter, gpointer data) {
  (void)action; (void)data; const char *id = g_variant_get_string(parameter, NULL);
  if (approval_controller) grotto_approval_controller_review(approval_controller, id);
  else { g_free(queued_review); queued_review = g_strdup(id); }
}
static void deny_done(GObject *source, GAsyncResult *result, gpointer data) {
  g_autoptr(GApplication) app = data;
  g_autoptr(GError) error = NULL;
  if (!grotto_approval_controller_decide_finish(GROTTO_APPROVAL_CONTROLLER(source), result, &error))
    on_approval_message(GROTTO_APPROVAL_CONTROLLER(source), "That request is no longer waiting.", app);
}
static void on_reconciled(GrottoApprovalController *controller, gpointer data) {
  (void)data;
  if (queued_deny) {
    g_autofree gchar *id = g_steal_pointer(&queued_deny);
    grotto_approval_controller_decide_async(controller, id, FALSE, FALSE, 0, NULL, deny_done, g_object_ref(global_app));
  }
}
static void on_deny_request(GSimpleAction *action, GVariant *parameter, gpointer data) {
  (void)action; (void)data;
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
  if (approval_controller) grotto_approval_controller_stop(approval_controller);
  g_clear_pointer(&queued_review, g_free); g_clear_pointer(&queued_deny, g_free);
  g_clear_object(&deferred_dbus_conn);
}

/* Callback when async D-Bus connection completes */
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
  if (queued_review) {
    grotto_approval_controller_review(approval_controller, queued_review);
    g_clear_pointer(&queued_review, g_free);
  }

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

static void on_startup(GtkApplication *app, gpointer user_data) {
  (void)user_data;
  startup_cancel = g_cancellable_new();
  g_application_hold(G_APPLICATION(app)); setup_held = TRUE;
  g_idle_add(deferred_init_cb, NULL);
  GnStatusNotifier *indicator = gn_status_notifier_new(G_APPLICATION(app), "org.nostr.Grotto", "Grotto");
  g_signal_connect(indicator, "activate", G_CALLBACK(indicator_activate), app);
  g_signal_connect(indicator, "quit", G_CALLBACK(indicator_quit), app);
  g_object_set_data_full(G_OBJECT(app), "grotto-indicator", indicator, g_object_unref);
}

static void on_activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;

  GtkWindow *existing = management_window(app);
  if (existing) {
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

  gtk_window_present(GTK_WINDOW(win));
  if (approval_controller) grotto_approval_controller_reconcile(approval_controller);
  STARTUP_TIME_END(STARTUP_PHASE_WINDOW);

  STARTUP_TIME_MARK("window-presented");

  /* Schedule deferred initialization for non-critical subsystems */
  g_idle_add(deferred_init_cb, NULL);
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
  AdwApplication *app = adw_application_new("org.nostr.Grotto", app_flags);

  STARTUP_TIME_END(STARTUP_PHASE_INIT);

  /* Store global app reference for theme change callbacks */
  global_app = GTK_APPLICATION(app);

  /* Settings will be initialized in on_activate after GTK is ready */

  /* Install app actions */
  static const GActionEntry app_entries[] = {
    { "quit", on_app_quit, NULL, NULL, NULL },
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

  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
  g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);

  /* Shutdown secure memory subsystem - securely zeros and frees all remaining allocations */
  gnostr_secure_mem_shutdown();

  return status;
}
