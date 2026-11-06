#include "../i18n.h"
#include "signer-window.h"
#include "dialog-actions.h"
#include "sheets/sheet-create-bunker.h"
#include "page-identity-detail.h"
#include "grotto-identity-service.h"
#include "grotto-profile-service.h"
#include "grotto-panels-control1-service.h"
#include "accounts_store.h"
#include "sheets/sheet-qr-display.h"
#include "onboarding-assistant.h"
#include "pairing-flow.h"
#include "app-resources.h"
#include "approval-controller.h"
#include "page-connect.h"
#include "page-settings.h"
#include "lock-screen.h"
#include "../secret_store.h"
#include "../startup-timing.h"
#include "../session-manager.h"
#include "../keyboard-nav.h"
#include <gio/gio.h>
#include <gdk/gdkkeysyms.h>
#include <time.h>

/* GSettings schema ID for the signer app */
#define SIGNER_GSETTINGS_ID "org.nostr.Grotto"

struct _SignerWindow {
  AdwApplicationWindow parent_instance;
  /* Template children */
  GtkStack *main_stack;
  GnLockScreen *lock_screen;
  AdwNavigationSplitView *split_view;
  AdwNavigationPage *content_page;
  GtkStack *panel_stack;
  GtkMenuButton *menu_btn;
  GtkListBox *sidebar;
  GrottoPanel *panels[4];
  GtkLabel *pending_count;
  GListModel *pending;
  gboolean initial_panel_chosen;
  AdwToastOverlay *toast_overlay;
  GrottoHomeModel *model;
  GrottoPanelsService *identity_service;
  GrottoApprovalController *approval_controller;
  gboolean pending_backup;
  GtkWidget *page_settings;
  /* GSettings for persistence */
  GSettings *settings;
  /* Startup optimization: lazy loading state */
  gboolean deferred_init_scheduled;
  gboolean page_data_loaded;
  gboolean session_signals_connected;
  /* Session management */
  gulong session_locked_handler;
  gulong session_unlocked_handler;
  gulong session_timeout_warning_handler;
  gint64 locked_at_time;
  guint breakpoint_region;
  GWeakRef transition_focus;
  guint focus_restore_tick;
};

G_DEFINE_TYPE(SignerWindow, signer_window, ADW_TYPE_APPLICATION_WINDOW)
static void identity_available(GrottoPanelsService *service, SignerWindow *self);
static AdwNavigationView *identity_navigation(SignerWindow *self);
static void setup_panels(SignerWindow *self);
static void show_preferences(SignerWindow *self);

static gboolean restore_transition_focus(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)clock; (void)data; SignerWindow *self = SIGNER_WINDOW(widget);
  GtkWidget *destination = !g_strcmp0(gtk_stack_get_visible_child_name(self->main_stack), "locked") ?
      GTK_WIDGET(self->lock_screen) :
      adw_navigation_split_view_get_collapsed(self->split_view) && !adw_navigation_split_view_get_show_content(self->split_view) ?
      GTK_WIDGET(adw_navigation_split_view_get_sidebar(self->split_view)) : GTK_WIDGET(adw_navigation_split_view_get_content(self->split_view));
  /* AdwBreakpointBin remaps its content on the next frame after reparenting. */
  if (!gtk_widget_get_mapped(destination)) return G_SOURCE_CONTINUE;
  if (!gtk_root_get_focus(GTK_ROOT(self))) {
    g_autoptr(GtkWidget) old = g_weak_ref_get(&self->transition_focus);
    if (!old || gtk_widget_get_root(old) != GTK_ROOT(self) || !gtk_widget_get_mapped(old) ||
        !gtk_widget_is_sensitive(old) || !gtk_widget_grab_focus(old))
      if (!gtk_widget_child_focus(destination, GTK_DIR_TAB_FORWARD)) gtk_widget_grab_focus(GTK_WIDGET(self->menu_btn));
  }
  g_weak_ref_set(&self->transition_focus, NULL); self->focus_restore_tick = 0;
  return G_SOURCE_REMOVE;
}
/* nostrc-9n2n9: upstream a6e1c8a57 fixes the deferred-focus overwrite in
 * GTK 4.23.1. On older GTK, clear before hiding/reparenting, then restore
 * the surviving control (or the destination's first control) after remap. */
static void clear_transition_focus(SignerWindow *self) {
  if (gtk_get_major_version() != 4 || gtk_get_minor_version() > 23 ||
      (gtk_get_minor_version() == 23 && gtk_get_micro_version() >= 1)) return;
  GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self));
  if (focus) g_weak_ref_set(&self->transition_focus, focus);
  gtk_window_set_focus(GTK_WINDOW(self), NULL);
  if (!self->focus_restore_tick)
    self->focus_restore_tick = gtk_widget_add_tick_callback(GTK_WIDGET(self), restore_transition_focus, NULL, NULL);
}
static void signer_window_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
  SignerWindow *self = SIGNER_WINDOW(widget);
  guint region = width <= 600 ? 1 : 0;
  if (region != self->breakpoint_region) clear_transition_focus(self);
  self->breakpoint_region = region;
  GTK_WIDGET_CLASS(signer_window_parent_class)->size_allocate(widget, width, height, baseline);
}

/* Helper to get GSettings if schema is available */
static GSettings *signer_window_get_settings(void) {
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (!source) return NULL;
  GSettingsSchema *schema = g_settings_schema_source_lookup(source, SIGNER_GSETTINGS_ID, TRUE);
  if (!schema) {
    g_debug(_("GSettings schema %s not found"), SIGNER_GSETTINGS_ID);
    return NULL;
  }
  g_settings_schema_unref(schema);
  return g_settings_new(SIGNER_GSETTINGS_ID);
}

/* Save window state to GSettings */
static void signer_window_save_state(SignerWindow *self) {
  if (!self->settings) return;

  gboolean maximized = gtk_window_is_maximized(GTK_WINDOW(self));
  g_settings_set_boolean(self->settings, "window-maximized", maximized);

  /* Only save dimensions if not maximized */
  if (!maximized) {
    int width, height;
    gtk_window_get_default_size(GTK_WINDOW(self), &width, &height);
    if (width > 0 && height > 0) {
      g_settings_set_int(self->settings, "window-width", width);
      g_settings_set_int(self->settings, "window-height", height);
    }
  }
  g_debug(_("Window state saved: maximized=%d"), maximized);
}

/* Restore window state from GSettings */
static void signer_window_restore_state(SignerWindow *self) {
  if (!self->settings) return;

  int width = g_settings_get_int(self->settings, "window-width");
  int height = g_settings_get_int(self->settings, "window-height");
  gboolean maximized = g_settings_get_boolean(self->settings, "window-maximized");

  if (width > 0 && height > 0) {
    gtk_window_set_default_size(GTK_WINDOW(self), width, height);
  }
  if (maximized) {
    gtk_window_maximize(GTK_WINDOW(self));
  }
  g_debug(_("Window state restored: width=%d height=%d maximized=%d"), width, height, maximized);
}

/* Handle close-request to save state before closing */
static gboolean on_close_request(GtkWindow *window, gpointer user_data) {
  (void)user_data;
  SignerWindow *self = SIGNER_WINDOW(window);
  signer_window_save_state(self);
  return FALSE; /* Allow close to proceed */
}

static void on_sidebar_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
  (void)box;
  SignerWindow *self = user_data;
  if (!self || !row) return;
  int idx = gtk_list_box_row_get_index(row);
  const char *names[] = { "identity", "apps", "activity", "remote-signing" };
  if (idx >= 0 && idx < 4) signer_window_show_page(self, names[idx]);

  /* On narrow screens when sidebar is collapsed, selecting a row should close the sidebar */
  if (self->split_view && adw_navigation_split_view_get_collapsed(self->split_view)) {
    adw_navigation_split_view_set_show_content(self->split_view, TRUE);
  }
}

static void signer_window_dispose(GObject *object) {
  SignerWindow *self = SIGNER_WINDOW(object);

  if (self->focus_restore_tick) gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->focus_restore_tick);
  self->focus_restore_tick = 0; g_weak_ref_set(&self->transition_focus, NULL);
  /* Disconnect session manager signals if they were connected */
  if (self->session_signals_connected) {
    GnSessionManager *sm = gn_session_manager_get_default();
    if (self->session_locked_handler > 0) {
      g_signal_handler_disconnect(sm, self->session_locked_handler);
      self->session_locked_handler = 0;
    }
    if (self->session_unlocked_handler > 0) {
      g_signal_handler_disconnect(sm, self->session_unlocked_handler);
      self->session_unlocked_handler = 0;
    }
    if (self->session_timeout_warning_handler > 0) {
      g_signal_handler_disconnect(sm, self->session_timeout_warning_handler);
      self->session_timeout_warning_handler = 0;
    }
    self->session_signals_connected = FALSE;
  }

  if (self->pending) g_signal_handlers_disconnect_by_data(self->pending, self);
  g_clear_object(&self->pending);
  for (guint i = 0; i < G_N_ELEMENTS(self->panels); i++) g_clear_object(&self->panels[i]);
  g_clear_object(&self->page_settings);
  if (self->model) grotto_home_model_stop(self->model);
  g_clear_object(&self->identity_service);
  g_clear_object(&self->approval_controller);
  g_clear_object(&self->model);
  /* Save state on dispose as a backup */
  signer_window_save_state(self);
  g_clear_object(&self->settings);
  gtk_widget_dispose_template(GTK_WIDGET(self), TYPE_SIGNER_WINDOW);
  G_OBJECT_CLASS(signer_window_parent_class)->dispose(object);
}

static void signer_window_finalize(GObject *object) {
  g_weak_ref_clear(&SIGNER_WINDOW(object)->transition_focus);
  G_OBJECT_CLASS(signer_window_parent_class)->finalize(object);
}
static void signer_window_class_init(SignerWindowClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = signer_window_dispose;
  object_class->finalize = signer_window_finalize;
  widget_class->size_allocate = signer_window_size_allocate;

  STARTUP_TIME_MARK("signer-window-class-init");

  /* Ensure custom page types are registered before template instantiation
   * This is tracked as STARTUP_PHASE_TYPES to separate from page data loading */
  STARTUP_TIME_BEGIN(STARTUP_PHASE_TYPES);
  g_type_ensure(GN_TYPE_LOCK_SCREEN);
  g_type_ensure(GROTTO_TYPE_PAGE_CONNECT);
  g_type_ensure(TYPE_PAGE_SETTINGS);
  STARTUP_TIME_END(STARTUP_PHASE_TYPES);

  gtk_widget_class_set_template_from_resource(widget_class, APP_RESOURCE_PATH "/ui/signer-window.ui");
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, main_stack);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, lock_screen);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, split_view);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, content_page);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, panel_stack);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, menu_btn);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, sidebar);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, toast_overlay);
}

/* Window action handlers for keyboard shortcuts */
static void on_win_new_profile(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  SignerWindow *self = SIGNER_WINDOW(user_data);
  signer_window_show_new_profile(self);
}

static void on_win_import_profile(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  SignerWindow *self = SIGNER_WINDOW(user_data);
  signer_window_show_import_profile(self);
}

static void on_win_export(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  SignerWindow *self = SIGNER_WINDOW(user_data);
  signer_window_show_backup(self);
}

static void on_win_lock(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  SignerWindow *self = SIGNER_WINDOW(user_data);
  signer_window_lock_session(self);
}

static void on_win_preferences(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  SignerWindow *self = SIGNER_WINDOW(user_data);
  signer_window_show_page(self, "settings");
}

static void on_win_about(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  SignerWindow *self = SIGNER_WINDOW(user_data);
  AdwDialog *about = adw_about_dialog_new();
  adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "Grotto");
  adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), "org.nostr.Grotto");
  adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), GROTTO_VERSION);
  adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), "https://github.com/chebizarro/nostrc");
  adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about), "https://github.com/chebizarro/nostrc/issues");
  const char *devs[] = { "GNostr Team", NULL };
  adw_about_dialog_set_developers(ADW_ABOUT_DIALOG(about), devs);
  adw_dialog_present(about, GTK_WIDGET(self));
}

static void on_win_quit(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  SignerWindow *self = SIGNER_WINDOW(user_data);
  GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
  if (app) {
    g_application_quit(G_APPLICATION(app));
  }
}

static void on_win_show_shortcuts(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param;
  SignerWindow *self = SIGNER_WINDOW(user_data);

  /* Load shortcuts window from resource */
  GtkBuilder *builder = gtk_builder_new_from_resource(APP_RESOURCE_PATH "/ui/shortcuts-window.ui");
  GtkShortcutsWindow *shortcuts = GTK_SHORTCUTS_WINDOW(gtk_builder_get_object(builder, "shortcuts_window"));

  if (shortcuts) {
    gtk_window_set_transient_for(GTK_WINDOW(shortcuts), GTK_WINDOW(self));
    gtk_window_present(GTK_WINDOW(shortcuts));
  }

  g_object_unref(builder);
}

static void on_pair_application(GSimpleAction *action, GVariant *parameter, gpointer data) {
  (void)action; (void)parameter; SignerWindow *self = data;
  signer_window_show_page(self, "apps");
  grotto_panel_host_invoke(self->panels[1], GROTTO_PANEL_HOST_ADD_APP, NULL, NULL);
}
static void on_activity(GSimpleAction *action, GVariant *parameter, gpointer data) {
  (void)action; (void)parameter; signer_window_show_page(data, "activity");
}
static void on_window_mapped(GtkWidget *widget, gpointer data) {
  (void)data; SignerWindow *self = SIGNER_WINDOW(widget); if (self->model) grotto_home_model_start(self->model);
}
static void on_window_unmapped(GtkWidget *widget, gpointer data) {
  (void)data; SignerWindow *self = SIGNER_WINDOW(widget); if (self->model) grotto_home_model_stop(self->model);
}

/* Setup window-level actions and keyboard shortcuts using GtkShortcutController */
static void setup_window_shortcuts(SignerWindow *self) {
  /* Add window-level actions */
  static const GActionEntry win_entries[] = {
    { "pair-application", on_pair_application, NULL, NULL, NULL },
    { "connect", on_pair_application, NULL, NULL, NULL },
    { "activity", on_activity, NULL, NULL, NULL },
    { "new-profile", on_win_new_profile, NULL, NULL, NULL },
    { "import-profile", on_win_import_profile, NULL, NULL, NULL },
    { "export", on_win_export, NULL, NULL, NULL },
    { "lock", on_win_lock, NULL, NULL, NULL },
    { "preferences", on_win_preferences, NULL, NULL, NULL },
    { "about", on_win_about, NULL, NULL, NULL },
    { "quit", on_win_quit, NULL, NULL, NULL },
    { "show-shortcuts", on_win_show_shortcuts, NULL, NULL, NULL },
  };
  g_action_map_add_action_entries(G_ACTION_MAP(self), win_entries, G_N_ELEMENTS(win_entries), self);
  g_object_bind_property(gn_session_manager_get_default(), "has-password",
      g_action_map_lookup_action(G_ACTION_MAP(self), "lock"), "enabled", G_BINDING_SYNC_CREATE);

  /* Create a GtkShortcutController with global scope for window-wide shortcuts */
  GtkEventController *controller = gtk_shortcut_controller_new();
  gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(controller), GTK_SHORTCUT_SCOPE_GLOBAL);

  /* Ctrl+N: Create new profile */
  GtkShortcut *shortcut_new = gtk_shortcut_new(
    gtk_keyval_trigger_new(GDK_KEY_n, GDK_CONTROL_MASK),
    gtk_named_action_new("win.new-profile")
  );
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(controller), shortcut_new);

  /* Ctrl+I: Import profile */
  GtkShortcut *shortcut_import = gtk_shortcut_new(
    gtk_keyval_trigger_new(GDK_KEY_i, GDK_CONTROL_MASK),
    gtk_named_action_new("win.import-profile")
  );
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(controller), shortcut_import);

  /* Ctrl+E: Export/backup */
  GtkShortcut *shortcut_export = gtk_shortcut_new(
    gtk_keyval_trigger_new(GDK_KEY_e, GDK_CONTROL_MASK),
    gtk_named_action_new("win.export")
  );
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(controller), shortcut_export);

  /* Ctrl+L: Lock session */
  GtkShortcut *shortcut_lock = gtk_shortcut_new(
    gtk_keyval_trigger_new(GDK_KEY_l, GDK_CONTROL_MASK),
    gtk_named_action_new("win.lock")
  );
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(controller), shortcut_lock);

  /* Ctrl+,: Preferences */
  GtkShortcut *shortcut_prefs = gtk_shortcut_new(
    gtk_keyval_trigger_new(GDK_KEY_comma, GDK_CONTROL_MASK),
    gtk_named_action_new("win.preferences")
  );
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(controller), shortcut_prefs);

  /* Ctrl+Q: Quit application */
  GtkShortcut *shortcut_quit = gtk_shortcut_new(
    gtk_keyval_trigger_new(GDK_KEY_q, GDK_CONTROL_MASK),
    gtk_named_action_new("win.quit")
  );
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(controller), shortcut_quit);

  /* F1: Show about dialog */
  GtkShortcut *shortcut_about = gtk_shortcut_new(
    gtk_keyval_trigger_new(GDK_KEY_F1, 0),
    gtk_named_action_new("win.about")
  );
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(controller), shortcut_about);

  /* Ctrl+?: Show keyboard shortcuts help overlay */
  GtkShortcut *shortcut_help = gtk_shortcut_new(
    gtk_keyval_trigger_new(GDK_KEY_question, GDK_CONTROL_MASK | GDK_SHIFT_MASK),
    gtk_named_action_new("win.show-shortcuts")
  );
  gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(controller), shortcut_help);

  /* Note: Escape to close dialogs is handled natively by AdwDialog.
   * AdwDialog automatically closes when Escape is pressed, so we don't
   * need to add a window-level Escape handler. */

  gtk_widget_add_controller(GTK_WIDGET(self), controller);
}

/* Session manager signal handlers */
static void on_session_locked(GnSessionManager *sm, GnLockReason reason, gpointer user_data) {
  (void)sm;
  SignerWindow *self = SIGNER_WINDOW(user_data);

  /* Record lock time and show lock screen */
  self->locked_at_time = (gint64)time(NULL);
  gn_lock_screen_set_lock_reason(self->lock_screen, reason);
  gn_lock_screen_set_locked_at(self->lock_screen, self->locked_at_time);
  gn_lock_screen_clear_error(self->lock_screen);
  gn_lock_screen_clear_password(self->lock_screen);

  /* Switch to lock screen */
  self->pending_backup = FALSE;
  gtk_stack_set_visible_child_name(self->main_stack, "locked");
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self));
  if (dialog) adw_dialog_force_close(dialog);
  gn_lock_screen_focus_password(self->lock_screen);

  g_debug(_("Window: session locked (reason=%d)"), reason);
}

static void on_session_unlocked(GnSessionManager *sm, gpointer user_data) {
  (void)sm;
  SignerWindow *self = SIGNER_WINDOW(user_data);

  /* Clear lock screen state */
  gn_lock_screen_clear_password(self->lock_screen);
  gn_lock_screen_clear_error(self->lock_screen);

  /* Switch to main content */
  gtk_stack_set_visible_child_name(self->main_stack, "unlocked");

  g_debug(_("Window: session unlocked"));
}

static void on_session_timeout_warning(GnSessionManager *sm, guint seconds_remaining, gpointer user_data) {
  (void)sm;
  SignerWindow *self = SIGNER_WINDOW(user_data);

  /* Show a toast warning about impending timeout */
  AdwToast *toast = adw_toast_new_format(_("Management will lock in %u seconds"), seconds_remaining);
  adw_toast_set_timeout(toast, 3);

  adw_toast_overlay_add_toast(self->toast_overlay, toast);
}

static void on_lock_screen_unlock_requested(GnLockScreen *lock_screen, gpointer user_data) {
  (void)lock_screen;
  SignerWindow *self = SIGNER_WINDOW(user_data);

  /* The lock screen already called authenticate() - if we get here, it succeeded */
  /* Switch to main content (the session manager will emit unlocked signal) */
  (void)self;
  g_debug(_("Window: unlock requested from lock screen"));
}

/* Connect session manager signals (deferred to after window display) */
static void signer_window_connect_session_signals(SignerWindow *self) {
  if (self->session_signals_connected) return;

  GnSessionManager *sm = gn_session_manager_get_default();

  self->session_locked_handler = g_signal_connect(sm, "session-locked",
                                                   G_CALLBACK(on_session_locked), self);
  self->session_unlocked_handler = g_signal_connect(sm, "session-unlocked",
                                                     G_CALLBACK(on_session_unlocked), self);
  self->session_timeout_warning_handler = g_signal_connect(sm, "timeout-warning",
                                                            G_CALLBACK(on_session_timeout_warning), self);

  /* Connect to lock screen unlock signal */
  g_signal_connect(self->lock_screen, "unlock-requested",
                   G_CALLBACK(on_lock_screen_unlock_requested), self);

  self->session_signals_connected = TRUE;
}

/* Deferred initialization for non-critical page data */
static gboolean signer_window_deferred_page_init(gpointer user_data) {
  SignerWindow *self = SIGNER_WINDOW(user_data);

  if (self->page_data_loaded) {
    return G_SOURCE_REMOVE;
  }

  gint64 start = startup_timing_measure_start();

  STARTUP_TIME_BEGIN(STARTUP_PHASE_PAGES);

  /* Connect session signals now that window is displayed */
  signer_window_connect_session_signals(self);

  /* Page-specific data loading can be triggered here.
   * Currently pages load their data on demand when shown,
   * but any heavy initialization should be deferred here.
   *
   * Trigger initial data load for the visible (first) page only.
   * Other pages will load on demand when selected.
   */
  self->page_data_loaded = TRUE;

  STARTUP_TIME_END(STARTUP_PHASE_PAGES);
  startup_timing_measure_end(start, "deferred-page-data-init", 50);
  STARTUP_TIME_MARK("pages-data-ready");

  return G_SOURCE_REMOVE;  /* Don't repeat */
}

static void signer_window_init(SignerWindow *self) {
  g_weak_ref_init(&self->transition_focus, NULL);
  gint64 init_start = startup_timing_measure_start();

  gtk_widget_init_template(GTK_WIDGET(self));
  self->model = grotto_home_model_new();
  self->identity_service = grotto_identity_service_new(self->model);
  g_signal_connect_object(self->identity_service, "availability-changed", G_CALLBACK(identity_available), self, 0);
  self->page_settings = GTK_WIDGET(g_object_ref_sink(page_settings_new()));
  page_settings_set_model(PAGE_SETTINGS(self->page_settings), self->model);
  setup_panels(self);
  g_signal_connect(self, "map", G_CALLBACK(on_window_mapped), NULL);
  g_signal_connect(self, "unmap", G_CALLBACK(on_window_unmapped), NULL);

  /* Initialize state */
  self->deferred_init_scheduled = FALSE;
  self->page_data_loaded = FALSE;
  self->session_signals_connected = FALSE;
  self->session_locked_handler = 0;
  self->session_unlocked_handler = 0;
  self->session_timeout_warning_handler = 0;
  self->locked_at_time = 0;

  /* Initialize GSettings for persistence */
  self->settings = signer_window_get_settings();

  /* Restore window state from GSettings */
  signer_window_restore_state(self);

  /* Connect close-request to save state */
  g_signal_connect(self, "close-request", G_CALLBACK(on_close_request), NULL);

  /* Setup keyboard shortcuts using GtkShortcutController */
  setup_window_shortcuts(self);

  /* Get session manager for initial lock state check only
   * Signal connections are deferred to after window display for faster startup */
  GnSessionManager *sm = gn_session_manager_get_default();

  /* Set initial lock state based on session manager */
  if (gn_session_manager_is_locked(sm)) {
    self->locked_at_time = (gint64)time(NULL);
    gn_lock_screen_set_lock_reason(self->lock_screen, GN_LOCK_REASON_STARTUP);
    gn_lock_screen_set_locked_at(self->lock_screen, self->locked_at_time);
    gtk_stack_set_visible_child_name(self->main_stack, "locked");
  } else {
    gtk_stack_set_visible_child_name(self->main_stack, "unlocked");
  }

  /* Schedule deferred page data initialization using g_idle_add
   * This runs after the window is displayed, improving perceived startup time */
  if (!self->deferred_init_scheduled) {
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, signer_window_deferred_page_init,
                    g_object_ref(self), g_object_unref);
    self->deferred_init_scheduled = TRUE;
  }

  startup_timing_measure_end(init_start, "signer-window-init-core", 100);

  /* App menu with keyboard shortcuts */
  if (self->menu_btn) {
    GMenu *menu = g_menu_new();

    /* Profile section - use window actions for shortcuts */
    GMenu *profile_section = g_menu_new();
    g_menu_append(profile_section, _("New Identity…"), "win.new-profile");
    g_menu_append(profile_section, _("Import Identity…"), "win.import-profile");
    g_menu_append(profile_section, _("Back Up Identity…"), "win.export");
    g_menu_append(profile_section, _("Add App…"), "win.pair-application");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(profile_section));
    g_object_unref(profile_section);

    /* Security section */
    GMenu *security_section = g_menu_new();
    g_menu_append(security_section, _("Lock Management"), "win.lock");
    g_menu_append(security_section, _("Pause Signing"), "app.pause-signing");
    g_menu_append(security_section, _("Resume Signing…"), "app.resume-signing");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(security_section));
    g_object_unref(security_section);

    /* App section */
    GMenu *app_section = g_menu_new();
    g_menu_append(app_section, _("Preferences"), "win.preferences");
    g_menu_append(app_section, _("Keyboard Shortcuts"), "win.show-shortcuts");
    g_menu_append(app_section, _("About Grotto"), "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(app_section));
    g_object_unref(app_section);

    gtk_menu_button_set_menu_model(self->menu_btn, G_MENU_MODEL(menu));
    g_object_unref(menu);
  }
  g_signal_connect(self->sidebar, "row-activated", G_CALLBACK(on_sidebar_row_activated), self);
  /* Select first row and show default page */
  GtkListBoxRow *first = gtk_list_box_get_row_at_index(self->sidebar, 0);
  if (first) gtk_list_box_select_row(self->sidebar, first);
  gtk_stack_set_visible_child(self->panel_stack, GTK_WIDGET(self->panels[0]));
}

void signer_window_add_background_group(SignerWindow *self, AdwPreferencesGroup *group) {
  page_settings_add_background_group(PAGE_SETTINGS(self->page_settings), group);
}

static void pending_changed(GListModel *model, guint position, guint removed, guint added, gpointer data) {
  (void)position; (void)removed; (void)added;
  SignerWindow *self = data;
  guint count = g_list_model_get_n_items(model);
  g_autofree char *label = g_strdup_printf("%u", count);
  gtk_label_set_text(self->pending_count, label);
  gtk_widget_set_visible(GTK_WIDGET(self->pending_count), count > 0);
  g_autofree char *accessible = count ? g_strdup_printf(ngettext("Connected Apps, %u pending request", "Connected Apps, %u pending requests", count), count) : g_strdup(_("Connected Apps"));
  gtk_accessible_update_property(GTK_ACCESSIBLE(gtk_list_box_get_row_at_index(self->sidebar, 1)), GTK_ACCESSIBLE_PROPERTY_LABEL, accessible, -1);
}
static void pending_reconciled(GrottoApprovalController *controller, gboolean success, SignerWindow *self) {
  (void)controller;
  if (self->initial_panel_chosen || !success) return;
  if (self->pending && g_list_model_get_n_items(self->pending)) signer_window_show_page(self, "apps");
  self->initial_panel_chosen = TRUE;
}
void signer_window_set_pending_group(SignerWindow *self, GtkWidget *group) {
  /* Both the hosted inbox and the sidebar use the application-owned controller.
   * Changing panels never changes notification or request lifetime. */
  GrottoApprovalController *controller = g_object_get_data(G_OBJECT(group), "controller");
  g_set_object(&self->approval_controller, controller);
  grotto_apps_panel_set_pending_group(GROTTO_APPS_PANEL(self->panels[1]), group);
  if (self->pending) g_signal_handlers_disconnect_by_data(self->pending, self);
  g_set_object(&self->pending, grotto_approval_controller_get_pending(controller));
  g_signal_connect(self->pending, "items-changed", G_CALLBACK(pending_changed), self);
  pending_changed(self->pending, 0, 0, 0, self);
  g_signal_connect_object(controller, "reconciled", G_CALLBACK(pending_reconciled), self, 0);
  if (!self->initial_panel_chosen && g_list_model_get_n_items(self->pending))
    pending_reconciled(controller, TRUE, self);
}
static void action_enabled_changed(GActionGroup *app, const char *name, gboolean enabled, SignerWindow *self) {
  (void)app; (void)enabled;
  if (!strcmp(name, "resume-signing")) g_signal_emit_by_name(self->identity_service, "capabilities-changed");
}
SignerWindow *signer_window_new(AdwApplication *app) {
  g_return_val_if_fail(ADW_IS_APPLICATION(app), NULL);
  SignerWindow *self = g_object_new(TYPE_SIGNER_WINDOW, "application", app, NULL);
  g_signal_connect_object(app, "action-enabled-changed", G_CALLBACK(action_enabled_changed), self, 0);
  return self;
}

void signer_window_toast(SignerWindow *self, const gchar *message) {
  if (self->toast_overlay) adw_toast_overlay_add_toast(self->toast_overlay, adw_toast_new(message));
}
static void identity_host_free(gpointer data) { g_weak_ref_clear(data); g_free(data); }
static void identity_toast(gpointer data, const char *message) {
  g_autoptr(SignerWindow) self = g_weak_ref_get(data); if (self) signer_window_toast(self, message);
}
static void identity_add(gpointer data) {
  g_autoptr(SignerWindow) self = g_weak_ref_get(data); if (self) signer_window_show_new_profile(self);
}
static AdwNavigationView *identity_navigation(SignerWindow *self) {
  AdwNavigationView *navigation = grotto_identity_panel_get_navigation(GROTTO_IDENTITY_PANEL(self->panels[0]));
  AdwNavigationPage *page = adw_navigation_view_get_visible_page(navigation);
  while (GROTTO_IS_IDENTITY_PANEL(page)) {
    navigation = grotto_identity_panel_get_navigation(GROTTO_IDENTITY_PANEL(page));
    page = adw_navigation_view_get_visible_page(navigation);
  }
  return navigation;
}
static void identity_backup(gpointer data, const char *npub) {
  g_autoptr(SignerWindow) self = g_weak_ref_get(data);
  if (!self || gn_session_manager_is_locked(gn_session_manager_get_default())) return;
  adw_navigation_view_push(identity_navigation(self), ADW_NAVIGATION_PAGE(page_identity_detail_new(npub)));
}
static void identity_open(gpointer data, const char *target) {
  g_autoptr(SignerWindow) self = g_weak_ref_get(data);
  if (!self || gn_session_manager_is_locked(gn_session_manager_get_default())) return;
  if (g_str_has_prefix(target, "public-key:npub1")) {
    SheetQrDisplay *qr = sheet_qr_display_new(); sheet_qr_display_set_npub(qr, target + strlen("public-key:"));
    adw_dialog_present(ADW_DIALOG(qr), GTK_WIDGET(self));
  } else if (g_str_has_prefix(target, "apps:npub1")) {
    signer_window_show_page(self, "apps");
    grotto_apps_panel_filter_identity(GROTTO_APPS_PANEL(self->panels[1]), target + strlen("apps:"));
  } else if (g_str_has_prefix(target, "app:")) {
    signer_window_show_page(self, "apps");
    grotto_apps_panel_show_app(GROTTO_APPS_PANEL(self->panels[1]), target + 4);
  } else if (!strcmp(target, "ask-defaults")) {
    page_settings_show_ask_defaults(PAGE_SETTINGS(self->page_settings), GTK_WIDGET(self));
  } else if (!strcmp(target, "activity-retention")) {
    page_settings_show_retention(PAGE_SETTINGS(self->page_settings), GTK_WIDGET(self));
  } else if (g_str_has_prefix(target, "activity:")) {
    signer_window_show_page(self, "activity");
    grotto_activity_panel_filter_app(GROTTO_ACTIVITY_PANEL(self->panels[2]), target + 9);
  } else signer_window_show_page(self, target);
}
static void removal_done(GObject *source, GAsyncResult *result, gpointer data) {
  GTask *task = data; SignerWindow *self = g_task_get_source_object(task);
  g_autoptr(GVariant) reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, NULL);
  gboolean removed = FALSE;
  if (reply) g_variant_get(reply, "(b)", &removed);
  g_autofree gchar *owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(source));
  if (g_strcmp0(g_object_get_data(G_OBJECT(task), "owner"), owner)) removed = FALSE;
  if (removed) accounts_store_remove(accounts_store_get_default(), g_task_get_task_data(task), NULL);
  signer_window_toast(self, removed ? _("Identity removed from this device") : _("Identity wasn't removed. Check pending requests and retry"));
  g_task_return_boolean(task, removed); g_object_unref(task);
}
static void removal_proxy(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; GTask *task = data; SignerWindow *self = g_task_get_source_object(task);
  g_autoptr(GDBusProxy) proxy = g_dbus_proxy_new_for_bus_finish(result, NULL);
  g_autofree gchar *owner = proxy ? g_dbus_proxy_get_name_owner(proxy) : NULL;
  if (!owner || gn_session_manager_is_locked(gn_session_manager_get_default())) {
    signer_window_toast(self, _("Identity wasn't removed. Unlock management and retry")); g_task_return_boolean(task, FALSE); g_object_unref(task); return;
  }
  /* GDBusProxy pins this call to its current unique owner, not a later daemon. */
  const char *npub = g_task_get_task_data(task);
  g_object_set_data_full(G_OBJECT(task), "owner", g_steal_pointer(&owner), g_free);
  g_dbus_proxy_call(proxy, "RemoveIdentity", g_variant_new("(ss)", npub, npub), G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000, NULL, removal_done, task);
}
static void removal_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  SignerWindow *self = data;
  if (strcmp(response, "remove") || gn_session_manager_is_locked(gn_session_manager_get_default())) return;
  GTask *task = g_task_new(self, NULL, NULL, NULL);
  g_task_set_task_data(task, g_strdup(g_object_get_data(G_OBJECT(dialog), "identity")), g_free);
  g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NULL,
    "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", NULL, removal_proxy, task);
}
static void removal_typed(GtkEditable *entry, gpointer data) {
  AdwAlertDialog *dialog = data;
  adw_alert_dialog_set_response_enabled(dialog, "remove", !g_strcmp0(gtk_editable_get_text(entry), g_object_get_data(G_OBJECT(dialog), "identity")));
}
static void removal_copy(GtkButton *button, gpointer data) {
  AdwAlertDialog *dialog = data;
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(button)), g_object_get_data(G_OBJECT(dialog), "identity"));
}
static void identity_remove(gpointer data, const char *npub) {
  g_autoptr(SignerWindow) self = g_weak_ref_get(data);
  if (!self || gn_session_manager_is_locked(gn_session_manager_get_default())) return;
  AdwDialog *dialog = adw_alert_dialog_new(_("Remove Identity?"), _("This identity is deleted from this device. Without a backup it can't be recovered."));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *key = gtk_label_new(npub); gtk_label_set_wrap(GTK_LABEL(key), TRUE); gtk_label_set_selectable(GTK_LABEL(key), TRUE);
  GtkWidget *copy = gtk_button_new_with_label(_("Copy Public Key"));
  GtkWidget *entry = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry), _("Type the full npub to confirm removal"));
  gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Public Key Confirmation"), -1);
  gtk_box_append(GTK_BOX(box), key); gtk_box_append(GTK_BOX(box), copy); gtk_box_append(GTK_BOX(box), entry);
  adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), box);
  g_object_set_data_full(G_OBJECT(dialog), "identity", g_strdup(npub), g_free);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", _("_Cancel"), "remove", _("_Remove"), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "remove", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel"); adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(dialog), "remove", FALSE);
  g_signal_connect(copy, "clicked", G_CALLBACK(removal_copy), dialog);
  g_signal_connect(entry, "changed", G_CALLBACK(removal_typed), dialog);
  g_signal_connect_object(dialog, "response", G_CALLBACK(removal_response), self, 0);
  adw_dialog_present(dialog, GTK_WIDGET(self));
}
static void identity_available(GrottoPanelsService *service, SignerWindow *self) {
  if (!self->pending_backup || !grotto_panels_service_is_available(service)) return;
  AdwNavigationPage *page = ADW_NAVIGATION_PAGE(gtk_stack_get_visible_child(self->panel_stack));
  if (!GROTTO_IS_IDENTITY_PANEL(page)) { self->pending_backup = FALSE; return; }
  GrottoPanel *panel = GROTTO_PANEL(page);
  GListModel *identities = grotto_panels_service_get_model(service, GROTTO_PANELS_IDENTITIES);
  g_autoptr(GrottoPanelsItem) selected = NULL;
  for (guint i = 0; identities && i < g_list_model_get_n_items(identities); i++) {
    g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(identities, i);
    const GrottoPanelsMetadata *metadata = grotto_panels_item_get_metadata(item);
    if (!selected || metadata->is_default) g_set_object(&selected, item);
  }
  if (!selected) return; /* Inventory notifications complete the navigation. */
  const GrottoPanelsMetadata *metadata = grotto_panels_item_get_metadata(selected);
  if (!metadata->can_sign && !metadata->watch_only) return; /* Wait for daemon inventory. */
  self->pending_backup = FALSE;
  if (metadata->can_sign && !metadata->watch_only)
    grotto_panel_host_invoke(panel, GROTTO_PANEL_HOST_BACKUP, metadata->public_key, NULL);
}
static void preferences_closed(AdwDialog *dialog, gpointer data) {
  SignerWindow *self = data;
  adw_preferences_dialog_remove(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(self->page_settings));
}
static void show_preferences(SignerWindow *self) {
  if (gn_session_manager_is_locked(gn_session_manager_get_default())) return;
  if (gtk_widget_get_parent(self->page_settings)) return;
  AdwDialog *dialog = adw_preferences_dialog_new();
  adw_dialog_set_title(dialog, _("Preferences"));
  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(self->page_settings));
  g_signal_connect_object(dialog, "closed", G_CALLBACK(preferences_closed), self, 0);
  adw_dialog_present(dialog, GTK_WIDGET(self));
}
static void create_app_link(GtkButton *button, gpointer data) {
  (void)button; adw_dialog_present(ADW_DIALOG(sheet_create_bunker_new()), GTK_WIDGET(data));
}
static void host_add_app(gpointer data) {
  g_autoptr(SignerWindow) self = g_weak_ref_get(data);
  if (!self || gn_session_manager_is_locked(gn_session_manager_get_default())) return;
  AdwDialog *dialog = adw_dialog_new(); adw_dialog_set_title(dialog, _("Add App"));
  adw_dialog_set_content_width(dialog, 440); adw_dialog_set_content_height(dialog, 520);
  GtkWidget *toolbar = adw_toolbar_view_new();
  GtkWidget *create = gtk_button_new_with_mnemonic(_("Create _Link…"));
  gtk_widget_set_tooltip_text(create, _("Create Pairing Link"));
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), grotto_dialog_header(dialog, create));
  GtkWidget *page = g_object_new(GROTTO_TYPE_PAGE_CONNECT, NULL);
  g_signal_connect_object(create, "clicked", G_CALLBACK(create_app_link), page, 0);
  grotto_page_connect_set_model(GROTTO_PAGE_CONNECT(page), self->model);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
  adw_dialog_set_child(dialog, toolbar); adw_dialog_present(dialog, GTK_WIDGET(self));
}
static void host_review(gpointer data, const char *request) {
  g_autoptr(SignerWindow) self = g_weak_ref_get(data);
  if (!self) return;
  GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
  if (app) g_action_group_activate_action(G_ACTION_GROUP(app), "review-request", g_variant_new_string(request));
}
static gboolean host_can_command(gpointer data, GrottoPanelCommand command) {
  g_autoptr(SignerWindow) self = g_weak_ref_get(data);
  if (!self || command == GROTTO_PANEL_SET_IDENTITY_LABEL ||
      gn_session_manager_is_locked(gn_session_manager_get_default()) || !grotto_home_model_writable(self->model)) return FALSE;
  if (command == GROTTO_PANEL_RESUME) {
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
    return app && g_action_group_has_action(G_ACTION_GROUP(app), "resume-signing") &&
      g_action_group_get_action_enabled(G_ACTION_GROUP(app), "resume-signing");
  }
  return TRUE;
}
static void host_command(gpointer data, GrottoPanelCommand command, const GrottoPanelCommandArgs *args,
  GCancellable *cancel, GAsyncReadyCallback callback, gpointer user_data) {
  g_autoptr(SignerWindow) self = g_weak_ref_get(data);
  if (!self || !host_can_command(data, command)) {
    GTask *task = g_task_new(self, cancel, callback, user_data);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Unlock management first");
    g_object_unref(task); return;
  }
  GrottoPanelsService *control = grotto_identity_service_get_control(GROTTO_IDENTITY_SERVICE(self->identity_service));
  grotto_panels_control1_service_host_command_async(control, command, args, cancel, callback, user_data);
}
static void host_profile(gpointer data, GrottoProfileAction action, const char *key,
  const GrottoProfileInfo *info, GCancellable *cancel, GAsyncReadyCallback callback, gpointer user_data) {
  g_autoptr(SignerWindow) self = g_weak_ref_get(data);
  gboolean allowed = self && !gn_session_manager_is_locked(gn_session_manager_get_default());
  if (allowed && action != GROTTO_PROFILE_FETCH && action != GROTTO_PROFILE_VERIFY_NIP05) {
    allowed = FALSE;
    GListModel *identities = grotto_panels_service_get_model(self->identity_service, GROTTO_PANELS_IDENTITIES);
    for (guint i = 0; identities && i < g_list_model_get_n_items(identities); i++) {
      g_autoptr(GrottoPanelsItem) item = g_list_model_get_item(identities, i);
      const GrottoPanelsMetadata *m = grotto_panels_item_get_metadata(item);
      if (!g_strcmp0(key, m->public_key)) allowed = m->can_sign && !m->watch_only && grotto_home_model_writable(self->model);
    }
  }
  if (!allowed) {
    GTask *task = g_task_new(self, cancel, callback, user_data);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Unlock management and choose a signing identity"); g_object_unref(task); return;
  }
  grotto_profile_service_async(GTK_WINDOW(self), self->approval_controller, action, key, info, cancel, callback, user_data);
}
static void setup_panels(SignerWindow *self) {
  self->panels[0] = GROTTO_PANEL(g_object_ref_sink(grotto_identity_panel_new(self->identity_service, NULL)));
  self->panels[1] = GROTTO_PANEL(g_object_ref_sink(grotto_apps_panel_new(self->identity_service)));
  self->panels[2] = GROTTO_PANEL(g_object_ref_sink(grotto_activity_panel_new(self->identity_service)));
  self->panels[3] = GROTTO_PANEL(g_object_ref_sink(grotto_remote_panel_new(self->identity_service)));
  const GrottoPanelsHost host = {.show_toast = identity_toast, .review_request = host_review,
    .add_identity = identity_add, .add_app = host_add_app, .open_backup = identity_backup,
    .remove_identity = identity_remove, .open_in_signer = identity_open,
    .can_command = host_can_command, .command_async = host_command, .profile_async = host_profile};
  gsize count = 0; const GrottoPanelDescriptor *descriptors = grotto_panels_get_descriptors(&count);
  for (guint i = 0; i < count; i++) {
    GWeakRef *ref = g_new0(GWeakRef, 1); g_weak_ref_init(ref, self);
    grotto_panel_set_host(self->panels[i], &host, ref, identity_host_free);
    gtk_stack_add_named(self->panel_stack, GTK_WIDGET(self->panels[i]), descriptors[i].id);
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), g_dgettext("grotto-panels", descriptors[i].title));
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name(descriptors[i].icon_name));
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    if (i == 1) {
      self->pending_count = GTK_LABEL(gtk_label_new(""));
      gtk_widget_add_css_class(GTK_WIDGET(self->pending_count), "numeric");
      gtk_widget_set_visible(GTK_WIDGET(self->pending_count), FALSE);
      adw_action_row_add_suffix(ADW_ACTION_ROW(row), GTK_WIDGET(self->pending_count));
    }
    gtk_list_box_append(self->sidebar, row);
  }
}
void signer_window_open_management(SignerWindow *self, const gchar *section, const gchar *target) {
  g_return_if_fail(SIGNER_IS_WINDOW(self));
  if (!section || !target || strlen(target) > 4096 || !grotto_panels_public_text_validate(target, NULL)) return;
  /* Activation never unlocks management, exports a key or replays an edit. */
  if (gn_session_manager_is_locked(gn_session_manager_get_default())) return;
  if (!strcmp(section, "target")) {
    if (g_str_has_prefix(target, "app:")) {
      signer_window_show_page(self, "apps");
      if (self->panels[1]) grotto_apps_panel_show_app(GROTTO_APPS_PANEL(self->panels[1]), target + 4);
    } else if (g_str_has_prefix(target, "activity:")) {
      signer_window_show_page(self, "activity");
      if (self->panels[2]) grotto_activity_panel_filter_app(GROTTO_ACTIVITY_PANEL(self->panels[2]), target + 9);
    } else if (g_str_has_prefix(target, "apps:npub1")) {
      signer_window_show_page(self, "apps");
      if (self->panels[1]) grotto_apps_panel_filter_identity(GROTTO_APPS_PANEL(self->panels[1]), target + 5);
    } else if (g_str_has_prefix(target, "public-key:npub1")) signer_window_show_identity(self, target + 11);
    else if (!strcmp(target, "remote-signing")) signer_window_show_page(self, "remote-signing");
    else signer_window_show_page(self, "identity");
  } else if (!strcmp(section, "identity") || !strcmp(section, "backup")) {
    signer_window_show_identity(self, g_str_has_prefix(target, "npub1") ? target : NULL);
    if (!strcmp(section, "backup")) signer_window_toast(self, _("Choose Backup for this identity."));
  } else if (!strcmp(section, "apps") || !strcmp(section, "activity") || !strcmp(section, "remote-signing"))
    signer_window_show_page(self, section);
  else if (!strcmp(section, "review")) signer_window_show_page(self, "apps");
  else if (!strcmp(section, "add-identity")) signer_window_show_page(self, "identity");
}
void signer_window_show_identity(SignerWindow *self, const gchar *npub) {
  g_return_if_fail(SIGNER_IS_WINDOW(self));
  self->pending_backup = FALSE;
  if (gn_session_manager_is_locked(gn_session_manager_get_default())) return;
  clear_transition_focus(self);
  signer_window_show_page(self, "identity");
  if (npub && *npub) {
    GrottoPanel *panel = GROTTO_PANEL(grotto_identity_panel_new(self->identity_service, npub));
    const GrottoPanelsHost host = {.show_toast = identity_toast, .add_identity = identity_add,
      .open_backup = identity_backup, .remove_identity = identity_remove, .open_in_signer = identity_open,
      .profile_async = host_profile};
    GWeakRef *host_window = g_new0(GWeakRef, 1); g_weak_ref_init(host_window, self);
    grotto_panel_set_host(panel, &host, host_window, identity_host_free);
    adw_navigation_view_push(identity_navigation(self), ADW_NAVIGATION_PAGE(panel));
  }
}
void signer_window_show_page(SignerWindow *self, const char *name) {
  g_return_if_fail(self != NULL && name != NULL);
  self->initial_panel_chosen = TRUE; /* Explicit navigation wins over first reconciliation. */
  if (!strcmp(name, "backup")) { signer_window_show_backup(self); return; }
  if (!strcmp(name, "settings") || !strcmp(name, "preferences")) { show_preferences(self); return; }
  if (!strcmp(name, "home")) name = "identity";
  if (!strcmp(name, "connect")) name = "apps";
  AdwDialog *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self));
  if (ADW_IS_PREFERENCES_DIALOG(dialog)) adw_dialog_force_close(dialog);
  self->pending_backup = FALSE;
  const gchar *names[] = {"identity", "apps", "activity", "remote-signing"};
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    if (strcmp(name, names[i])) continue;
    clear_transition_focus(self);
    gtk_stack_set_visible_child(self->panel_stack, GTK_WIDGET(self->panels[i]));
    adw_navigation_page_set_title(self->content_page, adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(self->panels[i])));
    if (i == 1) grotto_apps_panel_show_app(GROTTO_APPS_PANEL(self->panels[1]), NULL);
    gtk_list_box_select_row(self->sidebar, gtk_list_box_get_row_at_index(self->sidebar, i));
    adw_navigation_split_view_set_show_content(self->split_view, TRUE);
    break;
  }
}

/**
 * signer_window_get_settings:
 * @self: a #SignerWindow
 *
 * Returns the #GSettings instance used by this window for persistence.
 * The returned settings object is owned by the window and should not be freed.
 *
 * Returns: (transfer none) (nullable): the #GSettings instance, or %NULL if not available
 */
GSettings *signer_window_get_gsettings(SignerWindow *self) {
  g_return_val_if_fail(SIGNER_IS_WINDOW(self), NULL);
  return self->settings;
}

/**
 * signer_get_app_settings:
 *
 * Gets or creates a #GSettings instance for the signer app.
 * This is a convenience function for components that need settings
 * but don't have access to a window instance.
 *
 * Returns: (transfer full) (nullable): a new #GSettings instance, or %NULL if schema not available
 */
GSettings *signer_get_app_settings(void) {
  return signer_window_get_settings();
}

/**
 * signer_window_show_new_profile:
 * @self: a #SignerWindow
 *
 * Opens the create new profile dialog.
 */
void signer_window_show_new_profile(SignerWindow *self) {
  g_return_if_fail(SIGNER_IS_WINDOW(self));
  /* Add Identity keeps the existing create/import flow and its one-live-window
   * contract, with the everyday Identity root as the return destination. */
  signer_window_show_identity(self, NULL);
  onboarding_assistant_present(GTK_WINDOW(self));
}

/**
 * signer_window_show_import_profile:
 * @self: a #SignerWindow
 *
 * Opens the import profile dialog.
 */
void signer_window_show_import_profile(SignerWindow *self) {
  g_return_if_fail(SIGNER_IS_WINDOW(self));
  onboarding_assistant_present(GTK_WINDOW(self));
}

/**
 * signer_window_show_create_account:
 * @self: a #SignerWindow
 *
 * Opens the account creation wizard dialog.
 */
void signer_window_show_create_account(SignerWindow *self) {
  g_return_if_fail(SIGNER_IS_WINDOW(self));
  onboarding_assistant_present(GTK_WINDOW(self));
}

/**
 * signer_window_show_backup:
 * @self: a #SignerWindow
 *
 * Opens the export/backup dialog with both backup and recovery features.
 */
void signer_window_show_backup(SignerWindow *self) {
  g_return_if_fail(SIGNER_IS_WINDOW(self));

  signer_window_show_identity(self, NULL);
  self->pending_backup = TRUE;
  identity_available(self->identity_service, self);
}

/**
 * signer_window_lock_session:
 * @self: a #SignerWindow
 *
 * Locks the current session, requiring re-authentication.
 * Uses the session manager for in-process lock management.
 */
void signer_window_lock_session(SignerWindow *self) {
  g_return_if_fail(SIGNER_IS_WINDOW(self));

  /* Lock via session manager - this will emit the locked signal */
  GnSessionManager *sm = gn_session_manager_get_default();
  gn_session_manager_lock(sm, GN_LOCK_REASON_MANUAL);

  g_message(_("Session locked via Ctrl+L"));
}

/**
 * signer_window_is_locked:
 * @self: a #SignerWindow
 *
 * Returns whether the window is currently showing the lock screen.
 *
 * Returns: %TRUE if locked
 */
gboolean signer_window_is_locked(SignerWindow *self) {
  g_return_val_if_fail(SIGNER_IS_WINDOW(self), TRUE);

  const gchar *visible = gtk_stack_get_visible_child_name(self->main_stack);
  return g_strcmp0(visible, "locked") == 0;
}
