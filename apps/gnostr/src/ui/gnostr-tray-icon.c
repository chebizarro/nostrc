/*
 * gnostr-tray-icon.c - System tray icon support for Linux
 *
 * Implementation using libayatana-appindicator3 with libdbusmenu-glib.
 *
 * Note: AppIndicator3 uses GTK3 internally for GtkMenu, but for GTK4 apps
 * we use libdbusmenu-glib directly to create the menu items. This avoids
 * GTK3/GTK4 symbol conflicts.
 *
 * This file is only compiled on Linux - macOS uses gnostr-menubar-macos.m instead.
 */

#ifndef __APPLE__

#include "gnostr-tray-icon.h"
#include <gio/gio.h>

#ifdef HAVE_APPINDICATOR
#ifdef HAVE_LEGACY_APPINDICATOR
#include <libappindicator/app-indicator.h>
#else
#include <libayatana-appindicator/app-indicator.h>
#endif
#include <libdbusmenu-glib/dbusmenu-glib.h>
#endif

struct _GnostrTrayIcon {
  GObject parent_instance;

  GtkApplication *app;     /* weak ref */
  GtkWindow *window;       /* weak ref */

#ifdef HAVE_APPINDICATOR
  AppIndicator *indicator;
  DbusmenuServer *menu_server;
  DbusmenuMenuitem *root_menu;
  DbusmenuMenuitem *item_show_hide;
#endif
};

G_DEFINE_TYPE(GnostrTrayIcon, gnostr_tray_icon, G_TYPE_OBJECT)

#ifdef HAVE_APPINDICATOR
/* Menu item callbacks */
static void on_menu_show_hide(DbusmenuMenuitem *item, guint timestamp, gpointer user_data);
static void on_menu_quit(DbusmenuMenuitem *item, guint timestamp, gpointer user_data);
#endif

static void
gnostr_tray_icon_dispose(GObject *object)
{
  GnostrTrayIcon *self = GNOSTR_TRAY_ICON(object);

#ifdef HAVE_APPINDICATOR
  g_clear_object(&self->indicator);
  g_clear_object(&self->menu_server);
  /* root_menu and items are owned by menu_server, no need to free */
  self->root_menu = NULL;
  self->item_show_hide = NULL;
#endif

  self->app = NULL;
  self->window = NULL;

  G_OBJECT_CLASS(gnostr_tray_icon_parent_class)->dispose(object);
}

static void
gnostr_tray_icon_class_init(GnostrTrayIconClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gnostr_tray_icon_dispose;
}

static void
gnostr_tray_icon_init(GnostrTrayIcon *self)
{
#ifdef HAVE_APPINDICATOR
  self->indicator = NULL;
  self->menu_server = NULL;
  self->root_menu = NULL;
  self->item_show_hide = NULL;
#endif
  self->app = NULL;
  self->window = NULL;
}

gboolean
gnostr_tray_icon_is_available(void)
{
#ifdef HAVE_APPINDICATOR
  return TRUE;
#else
  return FALSE;
#endif
}

GnostrTrayIcon *
gnostr_tray_icon_new(GtkApplication *app)
{
  g_return_val_if_fail(GTK_IS_APPLICATION(app), NULL);

#ifndef HAVE_APPINDICATOR
  g_debug("tray-icon: AppIndicator support not compiled in");
  return NULL;
#else
  GnostrTrayIcon *self = g_object_new(GNOSTR_TYPE_TRAY_ICON, NULL);
  self->app = app;  /* weak reference, app outlives tray icon */

  /* Create the AppIndicator
   * id: unique identifier for this indicator
   * icon_name: will look up icon by name in theme
   * category: APPLICATION_STATUS for regular apps
   */
  self->indicator = app_indicator_new(
    "gnostr-client",
    "org.gnostr.Client",  /* Icon name - will be looked up in icon theme */
    APP_INDICATOR_CATEGORY_APPLICATION_STATUS
  );

  if (!self->indicator) {
    g_warning("tray-icon: Failed to create AppIndicator");
    g_object_unref(self);
    return NULL;
  }

  /* Set tooltip/title */
  app_indicator_set_title(self->indicator, "GNostr");

  /* Create menu using libdbusmenu-glib (GTK-independent)
   * This is the proper way to create menus for AppIndicator in GTK4 apps
   */
  self->root_menu = dbusmenu_menuitem_new();

  /* Show/Hide Window menu item */
  self->item_show_hide = dbusmenu_menuitem_new();
  dbusmenu_menuitem_property_set(self->item_show_hide,
                                 DBUSMENU_MENUITEM_PROP_LABEL,
                                 "Hide Window");
  dbusmenu_menuitem_child_append(self->root_menu, self->item_show_hide);
  g_signal_connect(self->item_show_hide, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                   G_CALLBACK(on_menu_show_hide), self);

  /* Separator */
  DbusmenuMenuitem *separator = dbusmenu_menuitem_new();
  dbusmenu_menuitem_property_set(separator,
                                 DBUSMENU_MENUITEM_PROP_TYPE,
                                 DBUSMENU_CLIENT_TYPES_SEPARATOR);
  dbusmenu_menuitem_child_append(self->root_menu, separator);

  /* Quit menu item */
  DbusmenuMenuitem *item_quit = dbusmenu_menuitem_new();
  dbusmenu_menuitem_property_set(item_quit,
                                 DBUSMENU_MENUITEM_PROP_LABEL,
                                 "Quit");
  dbusmenu_menuitem_child_append(self->root_menu, item_quit);
  g_signal_connect(item_quit, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                   G_CALLBACK(on_menu_quit), self);

  /* Create DBus menu server and attach to indicator */
  self->menu_server = dbusmenu_server_new("/org/gnostr/client/menu");
  dbusmenu_server_set_root(self->menu_server, self->root_menu);

  /* Get the DBus object path for the menu and set it on the indicator */
  gchar *menu_path = NULL;
  g_object_get(self->menu_server, DBUSMENU_SERVER_PROP_DBUS_OBJECT, &menu_path, NULL);
  if (menu_path) {
    /* AppIndicator will connect to our DBus menu server */
    g_debug("tray-icon: Menu server at %s", menu_path);
    g_free(menu_path);
  }

  /* Make indicator active (visible) */
  app_indicator_set_status(self->indicator, APP_INDICATOR_STATUS_ACTIVE);

  g_message("tray-icon: System tray icon created successfully");

  return self;
#endif
}

void
gnostr_tray_icon_set_window(GnostrTrayIcon *self, GtkWindow *window)
{
  g_return_if_fail(GNOSTR_IS_TRAY_ICON(self));
  g_return_if_fail(window == NULL || GTK_IS_WINDOW(window));

  self->window = window;  /* weak reference */

#ifdef HAVE_APPINDICATOR
  if (window && self->item_show_hide) {
    /* Update label based on current visibility */
    gboolean visible = gtk_widget_get_visible(GTK_WIDGET(window));
    dbusmenu_menuitem_property_set(self->item_show_hide,
                                   DBUSMENU_MENUITEM_PROP_LABEL,
                                   visible ? "Hide Window" : "Show Window");
  }
#endif
}

/* --- Private helpers --- */

#ifdef HAVE_APPINDICATOR

static void
update_show_hide_label(GnostrTrayIcon *self)
{
  if (!self->item_show_hide || !self->window)
    return;

  gboolean visible = gtk_widget_get_visible(GTK_WIDGET(self->window));
  dbusmenu_menuitem_property_set(self->item_show_hide,
                                 DBUSMENU_MENUITEM_PROP_LABEL,
                                 visible ? "Hide Window" : "Show Window");
}

static void
on_menu_show_hide(DbusmenuMenuitem *item, guint timestamp, gpointer user_data)
{
  (void)item;
  (void)timestamp;
  GnostrTrayIcon *self = GNOSTR_TRAY_ICON(user_data);

  if (!self->window)
    return;

  if (gtk_widget_get_visible(GTK_WIDGET(self->window))) {
    gtk_widget_set_visible(GTK_WIDGET(self->window), FALSE);
  } else {
    gtk_widget_set_visible(GTK_WIDGET(self->window), TRUE);
    gtk_window_present(self->window);
  }

  update_show_hide_label(self);
}

static void
on_menu_quit(DbusmenuMenuitem *item, guint timestamp, gpointer user_data)
{
  (void)item;
  (void)timestamp;
  GnostrTrayIcon *self = GNOSTR_TRAY_ICON(user_data);

  if (self->app) {
    g_application_quit(G_APPLICATION(self->app));
  }
}

#endif /* HAVE_APPINDICATOR */

void
gnostr_tray_icon_set_unread_count(GnostrTrayIcon *self, int count)
{
  g_return_if_fail(GNOSTR_IS_TRAY_ICON(self));

#ifdef HAVE_APPINDICATOR
  if (!self->indicator) return;

  if (count > 0) {
    /* Format count (99+ for large numbers) */
    gchar *label;
    if (count > 99) {
      label = g_strdup("99+");
    } else {
      label = g_strdup_printf("%d", count);
    }

    /* Set label on indicator (shown next to icon on some DEs) */
    app_indicator_set_label(self->indicator, label, label);

    /* Set status to attention to potentially use attention icon */
    app_indicator_set_status(self->indicator, APP_INDICATOR_STATUS_ATTENTION);

    /* Update title/tooltip to show count */
    gchar *title = g_strdup_printf("GNostr (%d unread)", count > 99 ? 99 : count);
    app_indicator_set_title(self->indicator, title);
    g_free(title);
    g_free(label);
  } else {
    /* Clear badge */
    app_indicator_set_label(self->indicator, NULL, NULL);
    app_indicator_set_status(self->indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(self->indicator, "GNostr");
  }
#else
  (void)count;
#endif
}

#endif /* !__APPLE__ */
