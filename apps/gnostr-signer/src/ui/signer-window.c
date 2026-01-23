#include "signer-window.h"
#include "app-resources.h"
#include "page-permissions.h"
#include "page-applications.h"
#include "page-sessions.h"
#include "page-settings.h"
#include "sheets/sheet-create-profile.h"
#include "sheets/sheet-import-profile.h"
#include "sheets/sheet-account-backup.h"
#include "sheets/sheet-backup.h"
#include "../secret_store.h"
#include "../startup-timing.h"
#include <gio/gio.h>
#include <gdk/gdkkeysyms.h>

/* GSettings schema ID for the signer app */
#define SIGNER_GSETTINGS_ID "org.gnostr.Signer"

struct _SignerWindow {
  AdwApplicationWindow parent_instance;
  /* Template children */
  AdwToolbarView *toolbar_view;
  AdwHeaderBar *header_bar;
  AdwViewSwitcherTitle *switcher_title;
  AdwNavigationSplitView *split_view;
  GtkMenuButton *menu_btn;
  GtkListBox *sidebar;
  AdwViewStack *stack;
  /* pages */
  GtkWidget *page_permissions;
  GtkWidget *page_applications;
  GtkWidget *page_sessions;
  GtkWidget *page_settings;
  /* GSettings for persistence */
  GSettings *settings;
  /* Startup optimization: lazy loading state */
  gboolean deferred_init_scheduled;
  gboolean page_data_loaded;
};

G_DEFINE_TYPE(SignerWindow, signer_window, ADW_TYPE_APPLICATION_WINDOW)

/* Helper to get GSettings if schema is available */
static GSettings *signer_window_get_settings(void) {
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (!source) return NULL;
  GSettingsSchema *schema = g_settings_schema_source_lookup(source, SIGNER_GSETTINGS_ID, TRUE);
  if (!schema) {
    g_debug("GSettings schema %s not found", SIGNER_GSETTINGS_ID);
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
  g_debug("Window state saved: maximized=%d", maximized);
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
  g_debug("Window state restored: width=%d height=%d maximized=%d", width, height, maximized);
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
  const char *names[] = { "permissions", "applications", "sessions", "settings" };
  if (idx >= 0 && idx < 4) adw_view_stack_set_visible_child_name(self->stack, names[idx]);
}

static void signer_window_dispose(GObject *object) {
  SignerWindow *self = SIGNER_WINDOW(object);
  /* Save state on dispose as a backup */
  signer_window_save_state(self);
  g_clear_object(&self->settings);
  G_OBJECT_CLASS(signer_window_parent_class)->dispose(object);
}

static void signer_window_class_init(SignerWindowClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = signer_window_dispose;

  STARTUP_TIME_MARK("signer-window-class-init");

  /* Ensure custom page types are registered before template instantiation */
  STARTUP_TIME_BEGIN(STARTUP_PHASE_PAGES);
  g_type_ensure(TYPE_PAGE_PERMISSIONS);
  g_type_ensure(TYPE_PAGE_APPLICATIONS);
  g_type_ensure(GN_TYPE_PAGE_SESSIONS);
  g_type_ensure(TYPE_PAGE_SETTINGS);
  STARTUP_TIME_END(STARTUP_PHASE_PAGES);

  gtk_widget_class_set_template_from_resource(widget_class, APP_RESOURCE_PATH "/ui/signer-window.ui");
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, toolbar_view);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, header_bar);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, switcher_title);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, split_view);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, menu_btn);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, sidebar);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, stack);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, page_permissions);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, page_applications);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, page_sessions);
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
  adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "GNostr Signer");
  adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), "org.gnostr.Signer");
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

/* Setup window-level actions and keyboard shortcuts using GtkShortcutController */
static void setup_window_shortcuts(SignerWindow *self) {
  /* Add window-level actions */
  static const GActionEntry win_entries[] = {
    { "new-profile", on_win_new_profile, NULL, NULL, NULL },
    { "import-profile", on_win_import_profile, NULL, NULL, NULL },
    { "export", on_win_export, NULL, NULL, NULL },
    { "lock", on_win_lock, NULL, NULL, NULL },
    { "preferences", on_win_preferences, NULL, NULL, NULL },
    { "about", on_win_about, NULL, NULL, NULL },
    { "quit", on_win_quit, NULL, NULL, NULL },
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

  /* Ctrl+,: Preferences/Settings */
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

  /* Note: Escape to close dialogs is handled natively by AdwDialog.
   * AdwDialog automatically closes when Escape is pressed, so we don't
   * need to add a window-level Escape handler. */

  gtk_widget_add_controller(GTK_WIDGET(self), controller);
}

/* Deferred initialization for non-critical page data */
static gboolean signer_window_deferred_page_init(gpointer user_data) {
  SignerWindow *self = SIGNER_WINDOW(user_data);

  if (self->page_data_loaded) {
    return G_SOURCE_REMOVE;
  }

  gint64 start = startup_timing_measure_start();

  /* Page-specific data loading can be triggered here.
   * Currently pages load their data on demand when shown,
   * but any heavy initialization should be deferred here.
   */
  self->page_data_loaded = TRUE;

  startup_timing_measure_end(start, "deferred-page-data-init", 50);
  STARTUP_TIME_MARK("pages-data-ready");

  return G_SOURCE_REMOVE;  /* Don't repeat */
}

static void signer_window_init(SignerWindow *self) {
  gint64 init_start = startup_timing_measure_start();

  gtk_widget_init_template(GTK_WIDGET(self));

  /* Initialize state */
  self->deferred_init_scheduled = FALSE;
  self->page_data_loaded = FALSE;

  /* Initialize GSettings for persistence */
  self->settings = signer_window_get_settings();

  /* Restore window state from GSettings */
  signer_window_restore_state(self);

  /* Connect close-request to save state */
  g_signal_connect(self, "close-request", G_CALLBACK(on_close_request), NULL);

  /* Setup keyboard shortcuts using GtkShortcutController */
  setup_window_shortcuts(self);

  /* Schedule deferred page data initialization using g_idle_add
   * This runs after the window is displayed, improving perceived startup time */
  if (!self->deferred_init_scheduled) {
    g_idle_add(signer_window_deferred_page_init, self);
    self->deferred_init_scheduled = TRUE;
  }

  startup_timing_measure_end(init_start, "signer-window-init-core", 100);

  /* App menu with keyboard shortcuts */
  if (self->menu_btn) {
    GMenu *menu = g_menu_new();

    /* Profile section - use window actions for shortcuts */
    GMenu *profile_section = g_menu_new();
    g_menu_append(profile_section, "New Profile\tCtrl+N", "win.new-profile");
    g_menu_append(profile_section, "Import Profile\tCtrl+I", "win.import-profile");
    g_menu_append(profile_section, "Export/Backup\tCtrl+E", "win.export");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(profile_section));
    g_object_unref(profile_section);

    /* Security section */
    GMenu *security_section = g_menu_new();
    g_menu_append(security_section, "Lock Session\tCtrl+L", "win.lock");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(security_section));
    g_object_unref(security_section);

    /* App section */
    GMenu *app_section = g_menu_new();
    g_menu_append(app_section, "Preferences\tCtrl+,", "win.preferences");
    g_menu_append(app_section, "About GNostr Signer\tF1", "win.about");
    g_menu_append(app_section, "Quit\tCtrl+Q", "win.quit");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(app_section));
    g_object_unref(app_section);

    gtk_menu_button_set_menu_model(self->menu_btn, G_MENU_MODEL(menu));
    g_object_unref(menu);
  }
  g_signal_connect(self->sidebar, "row-activated", G_CALLBACK(on_sidebar_row_activated), self);
  /* Select first row and show default page */
  GtkListBoxRow *first = gtk_list_box_get_row_at_index(self->sidebar, 0);
  if (first) gtk_list_box_select_row(self->sidebar, first);
  if (self->stack) adw_view_stack_set_visible_child_name(self->stack, "permissions");
}

SignerWindow *signer_window_new(AdwApplication *app) {
  g_return_val_if_fail(ADW_IS_APPLICATION(app), NULL);
  return g_object_new(TYPE_SIGNER_WINDOW, "application", app, NULL);
}

void signer_window_show_page(SignerWindow *self, const char *name) {
  g_return_if_fail(self != NULL && name != NULL);
  if (self->stack) adw_view_stack_set_visible_child_name(self->stack, name);
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
  SheetCreateProfile *dialog = sheet_create_profile_new();
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

/**
 * signer_window_show_import_profile:
 * @self: a #SignerWindow
 *
 * Opens the import profile dialog.
 */
void signer_window_show_import_profile(SignerWindow *self) {
  g_return_if_fail(SIGNER_IS_WINDOW(self));
  SheetImportProfile *dialog = sheet_import_profile_new();
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

/**
 * signer_window_show_backup:
 * @self: a #SignerWindow
 *
 * Opens the export/backup dialog with both backup and recovery features.
 */
void signer_window_show_backup(SignerWindow *self) {
  g_return_if_fail(SIGNER_IS_WINDOW(self));

  /* Get the currently active npub if available */
  gchar *npub = NULL;
  secret_store_get_public_key(NULL, &npub);

  /* Create the comprehensive backup/recovery dialog */
  SheetBackup *dialog = sheet_backup_new();
  if (npub && *npub) {
    sheet_backup_set_account(dialog, npub);
    g_free(npub);
  }

  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

/**
 * signer_window_lock_session:
 * @self: a #SignerWindow
 *
 * Locks the current session, requiring re-authentication.
 * For now, this calls the D-Bus method to lock the session.
 */
void signer_window_lock_session(SignerWindow *self) {
  g_return_if_fail(SIGNER_IS_WINDOW(self));

  /* Call D-Bus method to lock the session */
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) {
    g_warning("Failed to connect to session bus for lock: %s", err ? err->message : "unknown");
    g_clear_error(&err);
    return;
  }

  g_dbus_connection_call(bus,
                         "org.nostr.Signer",
                         "/org/nostr/signer",
                         "org.nostr.Signer",
                         "LockSession",
                         NULL,
                         NULL,
                         G_DBUS_CALL_FLAGS_NONE,
                         5000,
                         NULL,
                         NULL,
                         NULL);

  g_object_unref(bus);
  g_message("Session lock requested via Ctrl+L");
}
