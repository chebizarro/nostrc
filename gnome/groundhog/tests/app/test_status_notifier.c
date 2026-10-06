/* GnStatusNotifier (W32): with a StatusNotifierWatcher on the bus the item
 * registers, its menu lists Open and Quit, Open and a click activate, Quit
 * quits; with no watcher nothing is registered or exported. */
#include <gtk/gtk.h>
#include "gn-status-notifier.h"
#include "nostrc-test-bus.h"

static NostrcTestBus *bus;
static GDBusConnection *watcher;   /* the fake watcher's connection */
static gchar *registered_service;  /* what RegisterStatusNotifierItem got */
static guint activations, quits;

static const gchar watcher_xml[] =
  "<node><interface name='org.kde.StatusNotifierWatcher'>"
  "<method name='RegisterStatusNotifierItem'><arg name='service' type='s' direction='in'/></method>"
  "<property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
  "<property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
  "</interface></node>";

static void
watcher_method(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
               const gchar *method, GVariant *params, GDBusMethodInvocation *invocation, gpointer data)
{
  (void)c; (void)sender; (void)path; (void)iface; (void)data;
  if (g_str_equal(method, "RegisterStatusNotifierItem")) {
    g_free(registered_service);
    g_variant_get(params, "(s)", &registered_service);
  }
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *
watcher_property(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                 const gchar *name, GError **error, gpointer data)
{
  (void)c; (void)sender; (void)path; (void)iface; (void)error; (void)data;
  if (g_str_equal(name, "IsStatusNotifierHostRegistered")) return g_variant_new_boolean(TRUE);
  return g_variant_new_strv(NULL, 0);
}

static const GDBusInterfaceVTable watcher_vtable = { watcher_method, watcher_property, NULL, { 0 } };

static void on_activate(GnStatusNotifier *n, gpointer data) { (void)n; (void)data; activations++; }
static void on_quit(GnStatusNotifier *n, gpointer data) { (void)n; (void)data; quits++; }

/* A call whose reply arrives while the main context keeps serving the
 * item (a synchronous call from this thread would wait on itself). */
typedef struct { GVariant *result; GError *error; gboolean done; } Reply;
static void on_reply(GObject *source, GAsyncResult *res, gpointer data)
{
  Reply *reply = data;
  reply->result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &reply->error);
  reply->done = TRUE;
}
static GVariant *
call(GDBusConnection *c, const gchar *dest, const gchar *path, const gchar *iface, const gchar *method,
     GVariant *params, const GVariantType *reply_type)
{
  Reply reply = { 0 };
  g_dbus_connection_call(c, dest, path, iface, method, params, reply_type, G_DBUS_CALL_FLAGS_NONE,
                         5000, NULL, on_reply, &reply);
  gint64 deadline = g_get_monotonic_time() + 6 * G_USEC_PER_SEC;
  while (!reply.done) {
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    g_main_context_iteration(NULL, TRUE);
  }
  if (!reply.result) g_printerr("%s.%s: %s\n", iface, method, reply.error ? reply.error->message : "?");
  g_clear_error(&reply.error);
  return reply.result;
}

static gboolean registered(gpointer data) { return gn_status_notifier_is_registered(data); }
static gboolean has_service(gpointer data) { (void)data; return registered_service != NULL; }

static void
spin_until(gboolean (*done)(gpointer), gpointer data)
{
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (!done(data)) {
    g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    g_main_context_iteration(NULL, TRUE);
  }
}

static void
test_with_watcher(void)
{
  g_autoptr(GApplication) app = g_application_new("org.nostr.GroundhogTest", G_APPLICATION_NON_UNIQUE);
  g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(watcher_xml, NULL);
  guint reg = g_dbus_connection_register_object(watcher, "/StatusNotifierWatcher", info->interfaces[0],
                                                &watcher_vtable, NULL, NULL, NULL);
  g_assert_cmpuint(reg, >, 0);
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(watcher, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", "org.kde.StatusNotifierWatcher", 4u), NULL, G_DBUS_CALL_FLAGS_NONE,
      5000, NULL, NULL);
  g_assert_nonnull(r);

  g_autoptr(GnStatusNotifier) item = gn_status_notifier_new(app, "org.nostr.Groundhog", "Groundhog");
  g_signal_connect(item, "activate", G_CALLBACK(on_activate), NULL);
  g_signal_connect(item, "quit", G_CALLBACK(on_quit), NULL);
  spin_until(has_service, NULL);
  spin_until(registered, item);

  /* The item's properties and menu, as a host would read them. */
  g_autoptr(GDBusConnection) host = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  g_autoptr(GVariant) title = call(host, registered_service, "/StatusNotifierItem",
      "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", "org.kde.StatusNotifierItem", "Title"), NULL);
  g_assert_nonnull(title);
  g_autoptr(GVariant) inner = g_variant_get_child_value(title, 0);
  g_autoptr(GVariant) value = g_variant_get_variant(inner);
  g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "Groundhog");

  g_autoptr(GVariant) layout = call(host, registered_service, "/MenuBar",
      "com.canonical.dbusmenu", "GetLayout", g_variant_new("(iias)", 0, -1, NULL),
      G_VARIANT_TYPE("(u(ia{sv}av))"));
  g_assert_nonnull(layout);
  g_autoptr(GVariant) root = g_variant_get_child_value(layout, 1);
  g_autoptr(GVariant) children = g_variant_get_child_value(root, 2);
  g_assert_cmpuint(g_variant_n_children(children), ==, 2);
  g_autoptr(GVariant) first = g_variant_get_child_value(children, 0);
  g_autoptr(GVariant) open = g_variant_get_variant(first);
  g_autoptr(GVariant) open_props = g_variant_get_child_value(open, 1);
  const gchar *label = NULL;
  g_assert_true(g_variant_lookup(open_props, "label", "&s", &label));
  g_assert_cmpstr(label, ==, "Open Groundhog");

  g_autoptr(GVariant) e1 = call(host, registered_service, "/MenuBar",
      "com.canonical.dbusmenu", "Event", g_variant_new("(isvu)", 1, "clicked", g_variant_new_int32(0), 0u), NULL);
  g_assert_nonnull(e1);
  g_autoptr(GVariant) e2 = call(host, registered_service, "/StatusNotifierItem",
      "org.kde.StatusNotifierItem", "Activate", g_variant_new("(ii)", 0, 0), NULL);
  g_assert_nonnull(e2);
  g_autoptr(GVariant) e3 = call(host, registered_service, "/MenuBar",
      "com.canonical.dbusmenu", "Event", g_variant_new("(isvu)", 2, "clicked", g_variant_new_int32(0), 0u), NULL);
  g_assert_nonnull(e3);
  g_assert_cmpuint(activations, ==, 2);
  g_assert_cmpuint(quits, ==, 1);

  g_dbus_connection_unregister_object(watcher, reg);
  g_autoptr(GVariant) rel = g_dbus_connection_call_sync(watcher, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName",
      g_variant_new("(s)", "org.kde.StatusNotifierWatcher"), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
  g_assert_nonnull(rel);
  g_clear_pointer(&registered_service, g_free);
}

static gboolean
item_exported(gpointer data)
{
  GDBusConnection *host = data;
  g_autoptr(GVariant) r = call(host, g_dbus_connection_get_unique_name(host),
      "/StatusNotifierItem", "org.freedesktop.DBus.Introspectable", "Introspect", NULL, NULL);
  const gchar *xml = NULL;
  if (r) g_variant_get(r, "(&s)", &xml);
  return xml && strstr(xml, "org.kde.StatusNotifierItem") != NULL;
}

static void
test_without_watcher(void)
{
  g_autoptr(GApplication) app = g_application_new("org.nostr.GroundhogTest", G_APPLICATION_NON_UNIQUE);
  g_autoptr(GnStatusNotifier) item = gn_status_notifier_new(app, "org.nostr.Groundhog", "Groundhog");
  for (guint i = 0; i < 20; i++) g_main_context_iteration(NULL, FALSE);
  g_assert_false(gn_status_notifier_is_registered(item));
  g_autoptr(GDBusConnection) host = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  g_assert_false(item_exported(host));
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus);
  watcher = nostrc_test_bus_connect(bus);
  nostrc_test_bus_add_func("/groundhog/status-notifier/without-watcher", test_without_watcher);
  nostrc_test_bus_add_func("/groundhog/status-notifier/with-watcher", test_with_watcher);
  int status = g_test_run();
  nostrc_test_bus_down(bus);
  return status;
}
