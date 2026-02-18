/*
 * gnostr-tray-icon.c - System tray icon support for Linux (GTK4 compatible)
 *
 * Implementation using StatusNotifierItem D-Bus protocol directly.
 * This avoids libayatana-appindicator3 which requires GTK3 headers.
 *
 * The StatusNotifierItem (SNI) specification is the standard protocol
 * for system tray icons on modern Linux desktops (KDE, GNOME with extensions,
 * XFCE, etc.). We implement it directly via GDBus.
 *
 * D-Bus interfaces implemented:
 *   - org.kde.StatusNotifierItem (main icon interface)
 *   - com.canonical.dbusmenu (menu interface via libdbusmenu-glib)
 *
 * This file is only compiled on Linux - macOS uses gnostr-menubar-macos.m instead.
 */

#ifndef __APPLE__

#include "gnostr-tray-icon.h"
#include <gio/gio.h>

#ifdef HAVE_DBUSMENU
#include <libdbusmenu-glib/dbusmenu-glib.h>
#endif

/* StatusNotifierItem D-Bus interface XML */
static const gchar *sni_introspection_xml =
  "<node>"
  "  <interface name='org.kde.StatusNotifierItem'>"
  "    <property name='Category' type='s' access='read'/>"
  "    <property name='Id' type='s' access='read'/>"
  "    <property name='Title' type='s' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "    <property name='IconName' type='s' access='read'/>"
  "    <property name='AttentionIconName' type='s' access='read'/>"
  "    <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
  "    <property name='IconThemePath' type='s' access='read'/>"
  "    <property name='ItemIsMenu' type='b' access='read'/>"
  "    <property name='Menu' type='o' access='read'/>"
  "    <signal name='NewTitle'/>"
  "    <signal name='NewIcon'/>"
  "    <signal name='NewAttentionIcon'/>"
  "    <signal name='NewStatus'>"
  "      <arg name='status' type='s'/>"
  "    </signal>"
  "    <signal name='NewToolTip'/>"
  "    <method name='Activate'>"
  "      <arg name='x' type='i' direction='in'/>"
  "      <arg name='y' type='i' direction='in'/>"
  "    </method>"
  "    <method name='SecondaryActivate'>"
  "      <arg name='x' type='i' direction='in'/>"
  "      <arg name='y' type='i' direction='in'/>"
  "    </method>"
  "    <method name='Scroll'>"
  "      <arg name='delta' type='i' direction='in'/>"
  "      <arg name='orientation' type='s' direction='in'/>"
  "    </method>"
  "  </interface>"
  "</node>";

/* StatusNotifierWatcher interface for registration */
static const gchar *snw_introspection_xml =
  "<node>"
  "  <interface name='org.kde.StatusNotifierWatcher'>"
  "    <method name='RegisterStatusNotifierItem'>"
  "      <arg name='service' type='s' direction='in'/>"
  "    </method>"
  "    <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
  "  </interface>"
  "</node>";

struct _GnostrTrayIcon {
  GObject parent_instance;

  GtkApplication *app;     /* weak ref */
  GtkWindow *window;       /* weak ref */

  /* D-Bus connection and registration */
  GDBusConnection *connection;
  guint sni_registration_id;
  guint bus_name_id;
  gchar *bus_name;
  gchar *object_path;

  /* StatusNotifierItem properties */
  gchar *title;
  gchar *status;           /* "Active", "Passive", or "Attention" */
  gchar *icon_name;
  gchar *attention_icon_name;
  gchar *icon_theme_path;
  int unread_count;

#ifdef HAVE_DBUSMENU
  DbusmenuServer *menu_server;
  DbusmenuMenuitem *root_menu;
  DbusmenuMenuitem *item_show_hide;
  DbusmenuMenuitem *item_relay_status;
#endif

  /* Relay status */
  int relay_connected_count;
  int relay_total_count;
  GnostrTrayRelayState relay_state;

  GDBusNodeInfo *sni_introspection_data;
  gboolean registered;
};

G_DEFINE_TYPE(GnostrTrayIcon, gnostr_tray_icon, G_TYPE_OBJECT)

/* Forward declarations */
static void register_with_watcher(GnostrTrayIcon *self);
static void setup_dbus_interface(GnostrTrayIcon *self);
#ifdef HAVE_DBUSMENU
static void on_menu_show_hide(DbusmenuMenuitem *item, guint timestamp, gpointer user_data);
static void on_menu_quit(DbusmenuMenuitem *item, guint timestamp, gpointer user_data);
static void update_show_hide_label(GnostrTrayIcon *self);
static void update_relay_status_label(GnostrTrayIcon *self);
#endif

/* D-Bus method call handler */
static void
handle_method_call(GDBusConnection       *connection,
                   const gchar           *sender,
                   const gchar           *object_path,
                   const gchar           *interface_name,
                   const gchar           *method_name,
                   GVariant              *parameters,
                   GDBusMethodInvocation *invocation,
                   gpointer               user_data)
{
  GnostrTrayIcon *self = GNOSTR_TRAY_ICON(user_data);
  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;

  if (g_strcmp0(method_name, "Activate") == 0) {
    /* Left-click: toggle window visibility */
    if (self->window) {
      if (gtk_widget_get_visible(GTK_WIDGET(self->window))) {
        gtk_widget_set_visible(GTK_WIDGET(self->window), FALSE);
      } else {
        gtk_widget_set_visible(GTK_WIDGET(self->window), TRUE);
        /* Present and activate application for proper focus */
        gtk_window_present(self->window);
        if (self->app) {
          g_application_activate(G_APPLICATION(self->app));
        }
      }
#ifdef HAVE_DBUSMENU
      update_show_hide_label(self);
#endif
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
  } else if (g_strcmp0(method_name, "SecondaryActivate") == 0) {
    /* Right-click: handled by menu, nothing special needed */
    g_dbus_method_invocation_return_value(invocation, NULL);
  } else if (g_strcmp0(method_name, "Scroll") == 0) {
    /* Scroll: could be used for volume or similar, ignored for now */
    g_dbus_method_invocation_return_value(invocation, NULL);
  } else {
    g_dbus_method_invocation_return_error(invocation,
                                          G_DBUS_ERROR,
                                          G_DBUS_ERROR_UNKNOWN_METHOD,
                                          "Unknown method: %s", method_name);
  }
}

/* D-Bus property getter */
static GVariant *
handle_get_property(GDBusConnection  *connection,
                    const gchar      *sender,
                    const gchar      *object_path,
                    const gchar      *interface_name,
                    const gchar      *property_name,
                    GError          **error,
                    gpointer          user_data)
{
  GnostrTrayIcon *self = GNOSTR_TRAY_ICON(user_data);
  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;
  (void)error;

  if (g_strcmp0(property_name, "Category") == 0) {
    return g_variant_new_string("ApplicationStatus");
  } else if (g_strcmp0(property_name, "Id") == 0) {
    return g_variant_new_string("gnostr-client");
  } else if (g_strcmp0(property_name, "Title") == 0) {
    return g_variant_new_string(self->title ? self->title : "GNostr");
  } else if (g_strcmp0(property_name, "Status") == 0) {
    return g_variant_new_string(self->status ? self->status : "Active");
  } else if (g_strcmp0(property_name, "IconName") == 0) {
    return g_variant_new_string(self->icon_name ? self->icon_name : "org.gnostr.gnostr");
  } else if (g_strcmp0(property_name, "AttentionIconName") == 0) {
    return g_variant_new_string(self->attention_icon_name ? self->attention_icon_name : "org.gnostr.gnostr");
  } else if (g_strcmp0(property_name, "IconThemePath") == 0) {
    return g_variant_new_string(self->icon_theme_path ? self->icon_theme_path : "");
  } else if (g_strcmp0(property_name, "ToolTip") == 0) {
    /* ToolTip is a struct: (icon_name, icon_pixmap[], title, description) */
    GVariantBuilder pixmap_builder;
    g_variant_builder_init(&pixmap_builder, G_VARIANT_TYPE("a(iiay)"));
    /* Empty pixmap array - we use named icons */

    gchar *tooltip_title = self->title ? self->title : "GNostr";
    gchar *tooltip_desc = NULL;
    gboolean need_free = FALSE;

    /* Build tooltip description with unread count and relay status */
    GString *desc = g_string_new(NULL);
    if (self->unread_count > 0) {
      g_string_append_printf(desc, "%d unread", self->unread_count);
    }
    if (self->relay_total_count > 0) {
      if (desc->len > 0) g_string_append(desc, " | ");
      g_string_append_printf(desc, "Relays: %d/%d",
                             self->relay_connected_count,
                             self->relay_total_count);
    }
    if (desc->len > 0) {
      tooltip_desc = g_string_free(desc, FALSE);
      need_free = TRUE;
    } else {
      g_string_free(desc, TRUE);
      tooltip_desc = "";
    }

    GVariant *result = g_variant_new("(sa(iiay)ss)",
                                     "",  /* icon name (empty, use IconName) */
                                     &pixmap_builder,
                                     tooltip_title,
                                     tooltip_desc);

    if (need_free) {
      g_free(tooltip_desc);
    }

    return result;
  } else if (g_strcmp0(property_name, "ItemIsMenu") == 0) {
    return g_variant_new_boolean(FALSE);  /* We support Activate method */
  } else if (g_strcmp0(property_name, "Menu") == 0) {
#ifdef HAVE_DBUSMENU
    return g_variant_new_object_path("/org/gnostr/client/menu");
#else
    return g_variant_new_object_path("/");
#endif
  }

  return NULL;
}

static const GDBusInterfaceVTable sni_vtable = {
  handle_method_call,
  handle_get_property,
  NULL  /* set_property not needed */
};

static void
on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  GnostrTrayIcon *self = GNOSTR_TRAY_ICON(user_data);
  (void)name;

  self->connection = g_object_ref(connection);
  setup_dbus_interface(self);
}

static void
on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  GnostrTrayIcon *self = GNOSTR_TRAY_ICON(user_data);
  (void)connection;
  (void)name;

  /* Now register with the StatusNotifierWatcher */
  register_with_watcher(self);
}

static void
on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  GnostrTrayIcon *self = GNOSTR_TRAY_ICON(user_data);
  (void)connection;
  (void)name;

  g_warning("tray-icon: Lost D-Bus name ownership");
  self->registered = FALSE;
}

static void
setup_dbus_interface(GnostrTrayIcon *self)
{
  GError *error = NULL;

  /* Parse introspection data */
  self->sni_introspection_data = g_dbus_node_info_new_for_xml(sni_introspection_xml, &error);
  if (error) {
    g_warning("tray-icon: Failed to parse introspection data: %s", error->message);
    g_error_free(error);
    return;
  }

  /* Register the StatusNotifierItem interface */
  self->sni_registration_id = g_dbus_connection_register_object(
    self->connection,
    self->object_path,
    self->sni_introspection_data->interfaces[0],
    &sni_vtable,
    self,
    NULL,
    &error);

  if (error) {
    g_warning("tray-icon: Failed to register D-Bus object: %s", error->message);
    g_error_free(error);
    return;
  }

  g_debug("tray-icon: Registered StatusNotifierItem at %s", self->object_path);
}

static void
on_register_reply(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GnostrTrayIcon *self = GNOSTR_TRAY_ICON(user_data);
  GError *error = NULL;

  GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

  if (error) {
    /* StatusNotifierWatcher might not be running - this is not fatal */
    if (!g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)) {
      g_warning("tray-icon: Failed to register with StatusNotifierWatcher: %s", error->message);
    } else {
      g_debug("tray-icon: StatusNotifierWatcher not available (no system tray host)");
    }
    g_error_free(error);
    return;
  }

  if (reply) {
    g_variant_unref(reply);
  }

  self->registered = TRUE;
  g_message("tray-icon: Successfully registered with StatusNotifierWatcher");
}

static void
register_with_watcher(GnostrTrayIcon *self)
{
  if (!self->connection)
    return;

  /* Register with org.kde.StatusNotifierWatcher */
  g_dbus_connection_call(
    self->connection,
    "org.kde.StatusNotifierWatcher",
    "/StatusNotifierWatcher",
    "org.kde.StatusNotifierWatcher",
    "RegisterStatusNotifierItem",
    g_variant_new("(s)", self->bus_name),
    NULL,
    G_DBUS_CALL_FLAGS_NONE,
    -1,
    NULL,
    on_register_reply,
    self);
}

static void
emit_signal(GnostrTrayIcon *self, const gchar *signal_name, GVariant *parameters)
{
  if (!self->connection || !self->registered)
    return;

  GError *error = NULL;
  g_dbus_connection_emit_signal(
    self->connection,
    NULL,  /* destination (broadcast) */
    self->object_path,
    "org.kde.StatusNotifierItem",
    signal_name,
    parameters,
    &error);

  if (error) {
    g_warning("tray-icon: Failed to emit %s signal: %s", signal_name, error->message);
    g_error_free(error);
  }
}

static void
gnostr_tray_icon_dispose(GObject *object)
{
  GnostrTrayIcon *self = GNOSTR_TRAY_ICON(object);

  if (self->bus_name_id > 0) {
    g_bus_unown_name(self->bus_name_id);
    self->bus_name_id = 0;
  }

  if (self->sni_registration_id > 0 && self->connection) {
    g_dbus_connection_unregister_object(self->connection, self->sni_registration_id);
    self->sni_registration_id = 0;
  }

#ifdef HAVE_DBUSMENU
  g_clear_object(&self->menu_server);
  self->root_menu = NULL;
  self->item_show_hide = NULL;
#endif

  g_clear_object(&self->connection);

  if (self->sni_introspection_data) {
    g_dbus_node_info_unref(self->sni_introspection_data);
    self->sni_introspection_data = NULL;
  }

  g_clear_pointer(&self->bus_name, g_free);
  g_clear_pointer(&self->object_path, g_free);
  g_clear_pointer(&self->title, g_free);
  g_clear_pointer(&self->status, g_free);
  g_clear_pointer(&self->icon_name, g_free);
  g_clear_pointer(&self->attention_icon_name, g_free);
  g_clear_pointer(&self->icon_theme_path, g_free);

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
  self->connection = NULL;
  self->sni_registration_id = 0;
  self->bus_name_id = 0;
  self->bus_name = NULL;
  self->object_path = NULL;
  self->title = g_strdup("GNostr");
  self->status = g_strdup("Active");
  self->icon_name = g_strdup("org.gnostr.gnostr");
  self->attention_icon_name = g_strdup("org.gnostr.gnostr");
  self->icon_theme_path = NULL;
  self->unread_count = 0;
#ifdef HAVE_DBUSMENU
  self->menu_server = NULL;
  self->root_menu = NULL;
  self->item_show_hide = NULL;
#endif
  self->sni_introspection_data = NULL;
  self->registered = FALSE;
  self->app = NULL;
  self->window = NULL;
}

gboolean
gnostr_tray_icon_is_available(void)
{
  /* StatusNotifierItem is widely supported on Linux desktops */
  /* Runtime check could verify if org.kde.StatusNotifierWatcher exists */
  return TRUE;
}

GnostrTrayIcon *
gnostr_tray_icon_new(GtkApplication *app)
{
  g_return_val_if_fail(GTK_IS_APPLICATION(app), NULL);

  GnostrTrayIcon *self = g_object_new(GNOSTR_TYPE_TRAY_ICON, NULL);
  self->app = app;  /* weak reference, app outlives tray icon */

  /* Resolve icon theme path so the SNI host can find our app icon.
   * Prefer installed location; fall back to source tree for dev builds. */
#ifdef GNOSTR_ICON_THEME_DIR
  if (g_file_test(GNOSTR_ICON_THEME_DIR "/hicolor/scalable/apps/org.gnostr.gnostr.svg",
                  G_FILE_TEST_EXISTS)) {
    self->icon_theme_path = g_strdup(GNOSTR_ICON_THEME_DIR);
  }
#endif
#ifdef GNOSTR_ICON_THEME_DIR_DEV
  if (!self->icon_theme_path &&
      g_file_test(GNOSTR_ICON_THEME_DIR_DEV "/hicolor/scalable/apps/org.gnostr.gnostr.svg",
                  G_FILE_TEST_EXISTS)) {
    self->icon_theme_path = g_strdup(GNOSTR_ICON_THEME_DIR_DEV);
  }
#endif

  /* Generate unique bus name and object path using PID */
  self->bus_name = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", getpid());
  self->object_path = g_strdup("/StatusNotifierItem");

#ifdef HAVE_DBUSMENU
  /* Create menu using libdbusmenu-glib (GTK-independent) */
  self->root_menu = dbusmenu_menuitem_new();
  
  if (!self->root_menu) {
    g_warning("tray-icon: Failed to create root menu item, continuing without menu");
  } else {
    /* Show/Hide Window menu item */
    self->item_show_hide = dbusmenu_menuitem_new();
    if (self->item_show_hide) {
      dbusmenu_menuitem_property_set(self->item_show_hide,
                                     DBUSMENU_MENUITEM_PROP_LABEL,
                                     "Hide Window");
      dbusmenu_menuitem_child_append(self->root_menu, self->item_show_hide);
      g_signal_connect(self->item_show_hide, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                       G_CALLBACK(on_menu_show_hide), self);
    }

    /* Separator before relay status */
    DbusmenuMenuitem *separator1 = dbusmenu_menuitem_new();
    if (separator1) {
      dbusmenu_menuitem_property_set(separator1,
                                     DBUSMENU_MENUITEM_PROP_TYPE,
                                     DBUSMENU_CLIENT_TYPES_SEPARATOR);
      dbusmenu_menuitem_child_append(self->root_menu, separator1);
    }

    /* Relay status menu item (not clickable, just informational) */
    self->item_relay_status = dbusmenu_menuitem_new();
    if (self->item_relay_status) {
      dbusmenu_menuitem_property_set(self->item_relay_status,
                                     DBUSMENU_MENUITEM_PROP_LABEL,
                                     "Relays: Disconnected");
      dbusmenu_menuitem_property_set_bool(self->item_relay_status,
                                          DBUSMENU_MENUITEM_PROP_ENABLED,
                                          FALSE);
      dbusmenu_menuitem_child_append(self->root_menu, self->item_relay_status);
    }

    /* Separator before Quit */
    DbusmenuMenuitem *separator2 = dbusmenu_menuitem_new();
    if (separator2) {
      dbusmenu_menuitem_property_set(separator2,
                                     DBUSMENU_MENUITEM_PROP_TYPE,
                                     DBUSMENU_CLIENT_TYPES_SEPARATOR);
      dbusmenu_menuitem_child_append(self->root_menu, separator2);
    }

    /* Quit menu item */
    DbusmenuMenuitem *item_quit = dbusmenu_menuitem_new();
    if (item_quit) {
      dbusmenu_menuitem_property_set(item_quit,
                                     DBUSMENU_MENUITEM_PROP_LABEL,
                                     "Quit");
      dbusmenu_menuitem_child_append(self->root_menu, item_quit);
      g_signal_connect(item_quit, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                       G_CALLBACK(on_menu_quit), self);
    }

    /* Create DBus menu server */
    self->menu_server = dbusmenu_server_new("/org/gnostr/client/menu");
    if (self->menu_server && self->root_menu) {
      dbusmenu_server_set_root(self->menu_server, self->root_menu);
      g_debug("tray-icon: Menu server created at /org/gnostr/client/menu");
    } else {
      g_warning("tray-icon: Failed to create menu server or root menu");
      g_clear_object(&self->menu_server);
      g_clear_object(&self->root_menu);
    }
  }
#endif

  /* Own a unique bus name for this instance */
  self->bus_name_id = g_bus_own_name(
    G_BUS_TYPE_SESSION,
    self->bus_name,
    G_BUS_NAME_OWNER_FLAGS_NONE,
    on_bus_acquired,
    on_name_acquired,
    on_name_lost,
    self,
    NULL);

  g_message("tray-icon: StatusNotifierItem created (bus: %s)", self->bus_name);

  return self;
}

void
gnostr_tray_icon_set_window(GnostrTrayIcon *self, GtkWindow *window)
{
  g_return_if_fail(GNOSTR_IS_TRAY_ICON(self));
  g_return_if_fail(window == NULL || GTK_IS_WINDOW(window));

  self->window = window;  /* weak reference */

#ifdef HAVE_DBUSMENU
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

#ifdef HAVE_DBUSMENU

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
update_relay_status_label(GnostrTrayIcon *self)
{
  if (!self->item_relay_status)
    return;

  g_autofree gchar *label = NULL;
  switch (self->relay_state) {
    case GNOSTR_TRAY_RELAY_CONNECTED:
      label = g_strdup_printf("Relays: %d/%d connected",
                              self->relay_connected_count,
                              self->relay_total_count);
      break;
    case GNOSTR_TRAY_RELAY_CONNECTING:
      label = g_strdup_printf("Relays: Connecting (%d/%d)",
                              self->relay_connected_count,
                              self->relay_total_count);
      break;
    case GNOSTR_TRAY_RELAY_DISCONNECTED:
    default:
      if (self->relay_total_count > 0) {
        label = g_strdup_printf("Relays: Disconnected (0/%d)",
                                self->relay_total_count);
      } else {
        label = g_strdup("Relays: Not configured");
      }
      break;
  }

  dbusmenu_menuitem_property_set(self->item_relay_status,
                                 DBUSMENU_MENUITEM_PROP_LABEL,
                                 label);
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
    /* Present and activate application for proper focus */
    gtk_window_present(self->window);
    if (self->app) {
      g_application_activate(G_APPLICATION(self->app));
    }
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

#endif /* HAVE_DBUSMENU */

void
gnostr_tray_icon_set_unread_count(GnostrTrayIcon *self, int count)
{
  g_return_if_fail(GNOSTR_IS_TRAY_ICON(self));

  self->unread_count = count;

  if (count > 0) {
    /* Set status to attention */
    g_free(self->status);
    self->status = g_strdup("NeedsAttention");

    /* Update title to show count */
    g_free(self->title);
    if (count > 99) {
      self->title = g_strdup("GNostr (99+ unread)");
    } else {
      self->title = g_strdup_printf("GNostr (%d unread)", count);
    }

    /* Emit signals for changes */
    emit_signal(self, "NewStatus", g_variant_new("(s)", self->status));
    emit_signal(self, "NewTitle", NULL);
    emit_signal(self, "NewToolTip", NULL);
  } else {
    /* Clear attention state */
    g_free(self->status);
    self->status = g_strdup("Active");

    g_free(self->title);
    self->title = g_strdup("GNostr");

    emit_signal(self, "NewStatus", g_variant_new("(s)", self->status));
    emit_signal(self, "NewTitle", NULL);
    emit_signal(self, "NewToolTip", NULL);
  }
}

void
gnostr_tray_icon_set_relay_status(GnostrTrayIcon *self,
                                   int connected_count,
                                   int total_count,
                                   GnostrTrayRelayState state)
{
  g_return_if_fail(GNOSTR_IS_TRAY_ICON(self));

  self->relay_connected_count = connected_count;
  self->relay_total_count = total_count;
  self->relay_state = state;

#ifdef HAVE_DBUSMENU
  update_relay_status_label(self);
#endif

  /* Update tooltip to include relay status */
  emit_signal(self, "NewToolTip", NULL);
}

#endif /* !__APPLE__ */
