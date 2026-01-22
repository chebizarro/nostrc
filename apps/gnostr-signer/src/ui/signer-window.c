#include "signer-window.h"
#include "page-permissions.h"
#include "page-applications.h"
#include "page-settings.h"
#include <gio/gio.h>

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
  GtkWidget *page_settings;
  /* GSettings for persistence */
  GSettings *settings;
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
  const char *names[] = { "permissions", "applications", "settings" };
  if (idx >= 0 && idx < 3) adw_view_stack_set_visible_child_name(self->stack, names[idx]);
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

  /* Ensure custom page types are registered before template instantiation */
  g_type_ensure(TYPE_PAGE_PERMISSIONS);
  g_type_ensure(TYPE_PAGE_APPLICATIONS);
  g_type_ensure(TYPE_PAGE_SETTINGS);
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
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, page_settings);
}

static void signer_window_init(SignerWindow *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Initialize GSettings for persistence */
  self->settings = signer_window_get_settings();

  /* Restore window state from GSettings */
  signer_window_restore_state(self);

  /* Connect close-request to save state */
  g_signal_connect(self, "close-request", G_CALLBACK(on_close_request), NULL);

  /* App menu */
  if (self->menu_btn) {
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Preferences", "app.preferences");
    g_menu_append(menu, "About GNostr Signer", "app.about");
    g_menu_append(menu, "Quit", "app.quit");
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
