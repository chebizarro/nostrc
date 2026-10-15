#include "../i18n.h"
#include "signer-window.h"
#include "page-identity-detail.h"
#include "onboarding-assistant.h"
#include "pairing-flow.h"
#include "app-resources.h"
#include "page-home.h"
#include "page-connect.h"
#include "page-client-detail.h"
#include "page-history.h"
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
  AdwToolbarView *toolbar_view;
  AdwHeaderBar *header_bar;
  AdwViewSwitcherTitle *switcher_title;
  AdwNavigationSplitView *split_view;
  GtkToggleButton *sidebar_toggle_btn;
  GtkMenuButton *menu_btn;
  GtkListBox *sidebar;
  AdwViewStack *stack;
  /* pages */
  GtkWidget *page_home;
  GtkWidget *page_connect;
  AdwToastOverlay *toast_overlay;
  AdwNavigationView *detail_navigation;
  GrottoHomeModel *model;
  GtkWidget *page_history;
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

static gboolean restore_transition_focus(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)clock; (void)data; SignerWindow *self = SIGNER_WINDOW(widget);
  GtkWidget *destination = !g_strcmp0(gtk_stack_get_visible_child_name(self->main_stack), "locked") ?
      GTK_WIDGET(self->lock_screen) :
      adw_navigation_split_view_get_collapsed(self->split_view) && !adw_navigation_split_view_get_show_content(self->split_view) ?
      GTK_WIDGET(adw_navigation_split_view_get_sidebar(self->split_view)) : GTK_WIDGET(adw_navigation_view_get_visible_page(self->detail_navigation));
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
  guint region = width <= 500 ? 2 : width <= 800 ? 1 : 0;
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
  if (!self || !self->stack || !row) return;
  int idx = gtk_list_box_row_get_index(row);
  const char *names[] = { "home", "activity", "connect", "settings" };
  if (idx >= 0 && idx < 4) signer_window_show_page(self, names[idx]);

  /* On narrow screens when sidebar is collapsed, selecting a row should close the sidebar */
  if (self->split_view && adw_navigation_split_view_get_collapsed(self->split_view)) {
    adw_navigation_split_view_set_show_content(self->split_view, TRUE);
    if (self->sidebar_toggle_btn) {
      gtk_toggle_button_set_active(self->sidebar_toggle_btn, FALSE);
    }
  }
}

/* Handle sidebar toggle button click for responsive navigation */
static void on_sidebar_toggle_clicked(GtkToggleButton *button, gpointer user_data) {
  SignerWindow *self = SIGNER_WINDOW(user_data);
  if (!self || !self->split_view) return;

  gboolean active = gtk_toggle_button_get_active(button);
  /* When toggle is active, show sidebar; when inactive, show content */
  adw_navigation_split_view_set_show_content(self->split_view, !active);
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

  if (self->model) grotto_home_model_stop(self->model);
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
  g_type_ensure(GROTTO_TYPE_PAGE_HOME);
  g_type_ensure(GROTTO_TYPE_PAGE_CONNECT);
  g_type_ensure(GN_TYPE_PAGE_HISTORY);
  g_type_ensure(TYPE_PAGE_SETTINGS);
  STARTUP_TIME_END(STARTUP_PHASE_TYPES);

  gtk_widget_class_set_template_from_resource(widget_class, APP_RESOURCE_PATH "/ui/signer-window.ui");
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, main_stack);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, lock_screen);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, toolbar_view);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, header_bar);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, switcher_title);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, split_view);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, sidebar_toggle_btn);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, menu_btn);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, sidebar);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, stack);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, page_home);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, page_connect);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, toast_overlay);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, detail_navigation);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, page_history);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, page_settings);
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
  adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), "0.1.0");
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
  (void)action; (void)parameter; signer_window_show_page(data, "connect");
}
static void on_activity(GSimpleAction *action, GVariant *parameter, gpointer data) {
  (void)action; (void)parameter; signer_window_show_page(data, "activity");
}
static void on_client_selected(GrottoPageHome *home, const gchar *principal, gpointer data) {
  (void)home; SignerWindow *self = data;
  adw_navigation_view_push(self->detail_navigation, ADW_NAVIGATION_PAGE(grotto_client_detail_new(self->model, principal)));
}
static void on_stack_changed(GObject *object, GParamSpec *pspec, gpointer data) {
  (void)object; (void)pspec; SignerWindow *self = data;
  const gchar *name = adw_view_stack_get_visible_child_name(self->stack);
  const gchar *names[] = {"home", "activity", "connect", "settings"};
  adw_navigation_view_pop(self->detail_navigation);
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) if (!g_strcmp0(name, names[i])) gtk_list_box_select_row(self->sidebar, gtk_list_box_get_row_at_index(self->sidebar, i));
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
  gtk_stack_set_visible_child_name(self->main_stack, "locked");
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
  grotto_page_home_set_model(GROTTO_PAGE_HOME(self->page_home), self->model);
  grotto_page_connect_set_model(GROTTO_PAGE_CONNECT(self->page_connect), self->model);
  page_settings_set_model(PAGE_SETTINGS(self->page_settings), self->model);
  gn_page_history_set_model(GN_PAGE_HISTORY(self->page_history), self->model);
  g_signal_connect(self->page_home, "client-selected", G_CALLBACK(on_client_selected), self);
  g_signal_connect(self->stack, "notify::visible-child-name", G_CALLBACK(on_stack_changed), self);
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
    g_menu_append(profile_section, _("New Identity…\tCtrl+N"), "win.new-profile");
    g_menu_append(profile_section, _("Import Identity…\tCtrl+I"), "win.import-profile");
    g_menu_append(profile_section, _("Export/Backup…\tCtrl+E"), "win.export");
    g_menu_append(profile_section, _("Connect an Application…"), "win.pair-application");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(profile_section));
    g_object_unref(profile_section);

    /* Security section */
    GMenu *security_section = g_menu_new();
    g_menu_append(security_section, _("Lock Management\tCtrl+L"), "win.lock");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(security_section));
    g_object_unref(security_section);

    /* App section */
    GMenu *app_section = g_menu_new();
    g_menu_append(app_section, _("Preferences"), "win.preferences");
    g_menu_append(app_section, _("Keyboard Shortcuts\tCtrl+?"), "win.show-shortcuts");
    g_menu_append(app_section, _("About Grotto\tF1"), "win.about");
    g_menu_append(app_section, _("Pause Signing"), "app.pause-signing");
    g_menu_append(app_section, _("Resume Signing…"), "app.resume-signing");
    g_menu_append(app_section, _("Quit Approval UI (authorised signing continues)\tCtrl+Q"), "win.quit");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(app_section));
    g_object_unref(app_section);

    gtk_menu_button_set_menu_model(self->menu_btn, G_MENU_MODEL(menu));
    g_object_unref(menu);
  }
  g_signal_connect(self->sidebar, "row-activated", G_CALLBACK(on_sidebar_row_activated), self);
  /* Select first row and show default page */
  GtkListBoxRow *first = gtk_list_box_get_row_at_index(self->sidebar, 0);
  if (first) gtk_list_box_select_row(self->sidebar, first);
  if (self->stack) adw_view_stack_set_visible_child_name(self->stack, "home");

  /* Connect sidebar toggle button for responsive navigation */
  if (self->sidebar_toggle_btn) {
    g_signal_connect(self->sidebar_toggle_btn, "toggled",
                     G_CALLBACK(on_sidebar_toggle_clicked), self);
  }

  /* Setup enhanced keyboard navigation for sidebar */
  static const char *page_names[] = { "home", "activity", "connect", "settings", NULL };
  gn_keyboard_nav_setup_sidebar(self->sidebar, self->stack, page_names);
}

void signer_window_add_background_group(SignerWindow *self, AdwPreferencesGroup *group) {
  page_settings_add_background_group(PAGE_SETTINGS(self->page_settings), group);
}

void signer_window_set_pending_group(SignerWindow *self, GtkWidget *group) {
  grotto_page_home_set_pending_group(GROTTO_PAGE_HOME(self->page_home), group);
}
SignerWindow *signer_window_new(AdwApplication *app) {
  g_return_val_if_fail(ADW_IS_APPLICATION(app), NULL);
  return g_object_new(TYPE_SIGNER_WINDOW, "application", app, NULL);
}

void signer_window_show_page(SignerWindow *self, const char *name) {
  g_return_if_fail(self != NULL && name != NULL);
  const gchar *names[] = {"home", "activity", "connect", "settings"};
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    if (strcmp(name, names[i])) continue;
    if (g_strcmp0(adw_view_stack_get_visible_child_name(self->stack), name) ||
        adw_navigation_view_get_visible_page(self->detail_navigation) != adw_navigation_view_find_page(self->detail_navigation, "root") ||
        !adw_navigation_split_view_get_show_content(self->split_view)) clear_transition_focus(self);
    adw_navigation_view_pop(self->detail_navigation);
    adw_view_stack_set_visible_child_name(self->stack, name);
    gtk_list_box_select_row(self->sidebar, gtk_list_box_get_row_at_index(self->sidebar, i));
    adw_navigation_split_view_set_show_content(self->split_view, TRUE);
    if (!i) { adw_preferences_page_scroll_to_top(ADW_PREFERENCES_PAGE(self->page_home)); grotto_home_model_refresh(self->model); }
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
  /* G08's repeated activation contract uses the one live G09 setup flow,
   * not the retired CreateProfile sheet. */
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

  page_identity_detail_present(GTK_WIDGET(self));
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
