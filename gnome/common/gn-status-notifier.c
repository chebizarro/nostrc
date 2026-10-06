#include "gn-status-notifier.h"
#include <glib/gi18n.h>

#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define ITEM_PATH    "/StatusNotifierItem"
#define MENU_PATH    "/MenuBar"

enum { SIGNAL_ACTIVATE, SIGNAL_QUIT, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _GnStatusNotifier {
  GObject parent_instance;
  GApplication *app;       /* weak */
  gchar *icon_name;
  gchar *title;
  GDBusConnection *bus;
  guint item_reg;
  guint menu_reg;
  guint watch_id;
  gboolean registered;
  guint menu_revision;
};
G_DEFINE_FINAL_TYPE(GnStatusNotifier, gn_status_notifier, G_TYPE_OBJECT)

static const gchar item_xml[] =
  "<node>"
  "  <interface name='org.kde.StatusNotifierItem'>"
  "    <property name='Category' type='s' access='read'/>"
  "    <property name='Id' type='s' access='read'/>"
  "    <property name='Title' type='s' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "    <property name='IconName' type='s' access='read'/>"
  "    <property name='IconThemePath' type='s' access='read'/>"
  "    <property name='ItemIsMenu' type='b' access='read'/>"
  "    <property name='Menu' type='o' access='read'/>"
  "    <method name='Activate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
  "    <method name='SecondaryActivate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
  "    <method name='ContextMenu'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
  "    <method name='Scroll'><arg name='delta' type='i' direction='in'/><arg name='orientation' type='s' direction='in'/></method>"
  "    <signal name='NewIcon'/><signal name='NewTitle'/><signal name='NewStatus'><arg type='s'/></signal>"
  "  </interface>"
  "</node>";

static const gchar menu_xml[] =
  "<node>"
  "  <interface name='com.canonical.dbusmenu'>"
  "    <property name='Version' type='u' access='read'/>"
  "    <property name='Status' type='s' access='read'/>"
  "    <property name='TextDirection' type='s' access='read'/>"
  "    <property name='IconThemePath' type='as' access='read'/>"
  "    <method name='GetLayout'>"
  "      <arg name='parentId' type='i' direction='in'/><arg name='recursionDepth' type='i' direction='in'/>"
  "      <arg name='propertyNames' type='as' direction='in'/>"
  "      <arg name='revision' type='u' direction='out'/><arg name='layout' type='(ia{sv}av)' direction='out'/>"
  "    </method>"
  "    <method name='GetGroupProperties'>"
  "      <arg name='ids' type='ai' direction='in'/><arg name='propertyNames' type='as' direction='in'/>"
  "      <arg name='properties' type='a(ia{sv})' direction='out'/>"
  "    </method>"
  "    <method name='GetProperty'>"
  "      <arg name='id' type='i' direction='in'/><arg name='name' type='s' direction='in'/><arg name='value' type='v' direction='out'/>"
  "    </method>"
  "    <method name='Event'>"
  "      <arg name='id' type='i' direction='in'/><arg name='eventId' type='s' direction='in'/>"
  "      <arg name='data' type='v' direction='in'/><arg name='timestamp' type='u' direction='in'/>"
  "    </method>"
  "    <method name='EventGroup'>"
  "      <arg name='events' type='a(isvu)' direction='in'/><arg name='idErrors' type='ai' direction='out'/>"
  "    </method>"
  "    <method name='AboutToShow'><arg name='id' type='i' direction='in'/><arg name='needUpdate' type='b' direction='out'/></method>"
  "    <method name='AboutToShowGroup'><arg name='ids' type='ai' direction='in'/><arg name='updatesNeeded' type='ai' direction='out'/><arg name='idErrors' type='ai' direction='out'/></method>"
  "    <signal name='ItemsPropertiesUpdated'><arg type='a(ia{sv})'/><arg type='a(ias)'/></signal>"
  "    <signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
  "  </interface>"
  "</node>";

enum { MENU_ROOT = 0, MENU_OPEN = 1, MENU_QUIT = 2 };

static GVariant *
menu_item_props(GnStatusNotifier *self, gint id)
{
  GVariantBuilder props;
  g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
  if (id == MENU_ROOT) {
    g_variant_builder_add(&props, "{sv}", "children-display", g_variant_new_string("submenu"));
  } else {
    g_autofree gchar *open = g_strdup_printf(_("Open %s"), self->title);
    g_variant_builder_add(&props, "{sv}", "label",
                          g_variant_new_string(id == MENU_OPEN ? open : _("Quit")));
    g_variant_builder_add(&props, "{sv}", "enabled", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
  }
  return g_variant_builder_end(&props);
}

static GVariant *
menu_layout(GnStatusNotifier *self, gint parent)
{
  GVariantBuilder children;
  g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
  if (parent == MENU_ROOT)
    for (gint id = MENU_OPEN; id <= MENU_QUIT; id++)
      g_variant_builder_add(&children, "v",
        g_variant_new("(i@a{sv}@av)", id, menu_item_props(self, id), g_variant_new_array(G_VARIANT_TYPE_VARIANT, NULL, 0)));
  return g_variant_new("(i@a{sv}@av)", parent, menu_item_props(self, parent), g_variant_builder_end(&children));
}

static void
menu_method(GDBusConnection *bus, const gchar *sender, const gchar *path, const gchar *iface,
            const gchar *method, GVariant *params, GDBusMethodInvocation *invocation, gpointer data)
{
  GnStatusNotifier *self = data;
  (void)bus; (void)sender; (void)path; (void)iface;
  if (g_str_equal(method, "GetLayout")) {
    gint parent = 0;
    g_variant_get_child(params, 0, "i", &parent);
    g_dbus_method_invocation_return_value(invocation,
      g_variant_new("(u@(ia{sv}av))", self->menu_revision, menu_layout(self, parent)));
  } else if (g_str_equal(method, "GetGroupProperties")) {
    GVariantBuilder out;
    g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
    g_autoptr(GVariant) ids = g_variant_get_child_value(params, 0);
    gsize n = g_variant_n_children(ids);
    for (gsize i = 0; i < n; i++) {
      gint id = 0;
      g_variant_get_child(ids, i, "i", &id);
      g_variant_builder_add(&out, "(i@a{sv})", id, menu_item_props(self, id));
    }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ia{sv}))", &out));
  } else if (g_str_equal(method, "GetProperty")) {
    gint id = 0; const gchar *name = NULL;
    g_variant_get(params, "(i&s)", &id, &name);
    g_autoptr(GVariant) props = g_variant_ref_sink(menu_item_props(self, id));
    g_autoptr(GVariant) value = g_variant_lookup_value(props, name, NULL);
    if (value) g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
    else g_dbus_method_invocation_return_dbus_error(invocation, "com.canonical.dbusmenu.Error", "no such property");
  } else if (g_str_equal(method, "Event")) {
    gint id = 0; const gchar *event = NULL;
    g_variant_get(params, "(i&svu)", &id, &event, NULL, NULL);
    if (g_str_equal(event, "clicked")) {
      if (id == MENU_OPEN) g_signal_emit(self, signals[SIGNAL_ACTIVATE], 0);
      else if (id == MENU_QUIT) g_signal_emit(self, signals[SIGNAL_QUIT], 0);
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
  } else if (g_str_equal(method, "EventGroup")) {
    g_autoptr(GVariant) events = g_variant_get_child_value(params, 0);
    gsize n = g_variant_n_children(events);
    for (gsize i = 0; i < n; i++) {
      gint id = 0; const gchar *event = NULL;
      g_autoptr(GVariant) one = g_variant_get_child_value(events, i);
      g_variant_get(one, "(i&svu)", &id, &event, NULL, NULL);
      if (g_str_equal(event, "clicked")) {
        if (id == MENU_OPEN) g_signal_emit(self, signals[SIGNAL_ACTIVATE], 0);
        else if (id == MENU_QUIT) g_signal_emit(self, signals[SIGNAL_QUIT], 0);
      }
    }
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(ai)", NULL));
  } else if (g_str_equal(method, "AboutToShow")) {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
  } else if (g_str_equal(method, "AboutToShowGroup")) {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(aiai)", NULL, NULL));
  } else {
    g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
  }
}

static GVariant *
menu_property(GDBusConnection *bus, const gchar *sender, const gchar *path, const gchar *iface,
              const gchar *name, GError **error, gpointer data)
{
  (void)bus; (void)sender; (void)path; (void)iface; (void)error; (void)data;
  if (g_str_equal(name, "Version")) return g_variant_new_uint32(3);
  if (g_str_equal(name, "Status")) return g_variant_new_string("normal");
  if (g_str_equal(name, "TextDirection")) return g_variant_new_string("ltr");
  if (g_str_equal(name, "IconThemePath")) return g_variant_new_strv(NULL, 0);
  return NULL;
}

static void
item_method(GDBusConnection *bus, const gchar *sender, const gchar *path, const gchar *iface,
            const gchar *method, GVariant *params, GDBusMethodInvocation *invocation, gpointer data)
{
  GnStatusNotifier *self = data;
  (void)bus; (void)sender; (void)path; (void)iface; (void)params;
  if (g_str_equal(method, "Activate") || g_str_equal(method, "SecondaryActivate"))
    g_signal_emit(self, signals[SIGNAL_ACTIVATE], 0);
  /* ContextMenu: the host shows our dbusmenu itself; Scroll: nothing. */
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *
item_property(GDBusConnection *bus, const gchar *sender, const gchar *path, const gchar *iface,
              const gchar *name, GError **error, gpointer data)
{
  GnStatusNotifier *self = data;
  (void)bus; (void)sender; (void)path; (void)iface; (void)error;
  if (g_str_equal(name, "Category")) return g_variant_new_string("Communications");
  if (g_str_equal(name, "Id")) return g_variant_new_string(g_application_get_application_id(self->app));
  if (g_str_equal(name, "Title")) return g_variant_new_string(self->title);
  if (g_str_equal(name, "Status")) return g_variant_new_string("Active");
  if (g_str_equal(name, "IconName")) return g_variant_new_string(self->icon_name);
  if (g_str_equal(name, "IconThemePath")) return g_variant_new_string("");
  if (g_str_equal(name, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
  if (g_str_equal(name, "Menu")) return g_variant_new_object_path(MENU_PATH);
  return NULL;
}

static const GDBusInterfaceVTable item_vtable = { item_method, item_property, NULL, { 0 } };
static const GDBusInterfaceVTable menu_vtable = { menu_method, menu_property, NULL, { 0 } };

static void
on_registered(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GnStatusNotifier) self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (!r) {
    g_debug("status notifier: not registered: %s", error ? error->message : "?");
    return;
  }
  self->registered = TRUE;
}

static void
on_watcher_appeared(GDBusConnection *bus, const gchar *name, const gchar *owner, gpointer data)
{
  GnStatusNotifier *self = data;
  (void)name; (void)owner;
  if (!self->bus) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusNodeInfo) item = g_dbus_node_info_new_for_xml(item_xml, NULL);
    g_autoptr(GDBusNodeInfo) menu = g_dbus_node_info_new_for_xml(menu_xml, NULL);
    self->bus = g_object_ref(bus);
    self->item_reg = g_dbus_connection_register_object(bus, ITEM_PATH, item->interfaces[0],
                                                       &item_vtable, self, NULL, &error);
    if (!self->item_reg) { g_warning("status notifier: %s", error->message); return; }
    self->menu_reg = g_dbus_connection_register_object(bus, MENU_PATH, menu->interfaces[0],
                                                       &menu_vtable, self, NULL, &error);
    if (!self->menu_reg) { g_warning("status notifier: %s", error->message); return; }
  }
  /* Registered by our unique bus name: the watcher resolves the item path. */
  g_dbus_connection_call(bus, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME, "RegisterStatusNotifierItem",
                         g_variant_new("(s)", g_dbus_connection_get_unique_name(bus)), NULL,
                         G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_registered, g_object_ref(self));
}

static void
on_watcher_vanished(GDBusConnection *bus, const gchar *name, gpointer data)
{
  GnStatusNotifier *self = data;
  (void)bus; (void)name;
  self->registered = FALSE;
}

gboolean
gn_status_notifier_is_registered(GnStatusNotifier *self)
{
  g_return_val_if_fail(GN_IS_STATUS_NOTIFIER(self), FALSE);
  return self->registered;
}

static void
gn_status_notifier_dispose(GObject *object)
{
  GnStatusNotifier *self = GN_STATUS_NOTIFIER(object);
  if (self->watch_id) { g_bus_unwatch_name(self->watch_id); self->watch_id = 0; }
  if (self->bus) {
    if (self->item_reg) g_dbus_connection_unregister_object(self->bus, self->item_reg);
    if (self->menu_reg) g_dbus_connection_unregister_object(self->bus, self->menu_reg);
    self->item_reg = self->menu_reg = 0;
    g_clear_object(&self->bus);
  }
  G_OBJECT_CLASS(gn_status_notifier_parent_class)->dispose(object);
}

static void
gn_status_notifier_finalize(GObject *object)
{
  GnStatusNotifier *self = GN_STATUS_NOTIFIER(object);
  g_free(self->icon_name);
  g_free(self->title);
  G_OBJECT_CLASS(gn_status_notifier_parent_class)->finalize(object);
}

static void
gn_status_notifier_class_init(GnStatusNotifierClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gn_status_notifier_dispose;
  G_OBJECT_CLASS(klass)->finalize = gn_status_notifier_finalize;
  signals[SIGNAL_ACTIVATE] = g_signal_new("activate", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                          0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_QUIT] = g_signal_new("quit", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                      0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gn_status_notifier_init(GnStatusNotifier *self)
{
  self->menu_revision = 1;
}

GnStatusNotifier *
gn_status_notifier_new(GApplication *app, const gchar *icon_name, const gchar *title)
{
  g_return_val_if_fail(G_IS_APPLICATION(app), NULL);
  GnStatusNotifier *self = g_object_new(GN_TYPE_STATUS_NOTIFIER, NULL);
  self->app = app;
  self->icon_name = g_strdup(icon_name);
  self->title = g_strdup(title);
  self->watch_id = g_bus_watch_name(G_BUS_TYPE_SESSION, WATCHER_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                    on_watcher_appeared, on_watcher_vanished, self, NULL);
  return self;
}
