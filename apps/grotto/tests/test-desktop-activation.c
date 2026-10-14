#include "main_app.h"
#include "background.h"
#include "approval-controller.h"
#include "ui/signer-window.h"
#include "nostrc-test-bus.h"
#include "nostrc-test-gdk-frame.h"
#include <glib/gstdio.h>

static GApplication *app;
static GDBusConnection *connection;
static GSettings *settings;
static gboolean paused, done;
static guint pause_calls, status_calls, indicator_registrations;
static gchar *indicator_status;
static gboolean app_paused(void) {
  g_autoptr(GVariant) value = g_action_group_get_action_state(G_ACTION_GROUP(app), "pause-signing");
  return value && g_variant_get_boolean(value);
}
static void property_done(GObject *source, GAsyncResult *result, gpointer data) {
  (void)data;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, NULL);
  g_assert_nonnull(reply);
  g_autoptr(GVariant) value = NULL; g_variant_get(reply, "(v)", &value);
  indicator_status = g_variant_dup_string(value, NULL);
}
static void indicator_changed(GDBusConnection *bus, const char *sender, const char *path,
    const char *interface, const char *signal, GVariant *args, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)signal; (void)data;
  g_free(indicator_status); g_variant_get(args, "(s)", &indicator_status);
}
static void read_indicator(void) {
  g_clear_pointer(&indicator_status, g_free);
  g_dbus_connection_call(connection, g_dbus_connection_get_unique_name(connection), "/StatusNotifierItem",
      "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", "org.kde.StatusNotifierItem", "Status"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, property_done, NULL);
}
static const char *mode;
static gboolean bound(gpointer data) { g_error("Desktop activation readiness timed out at line %d", GPOINTER_TO_INT(data)); return G_SOURCE_REMOVE; }
#define WAIT(condition) do { guint id = g_timeout_add_seconds(10, bound, GINT_TO_POINTER(__LINE__)); \
  while (!(condition)) { g_main_context_iteration(NULL, TRUE); } g_source_remove(id); } while (0)
static void name(const char *value, gboolean own) {
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", own ? "RequestName" : "ReleaseName",
      own ? g_variant_new("(su)", value, 0) : g_variant_new("(s)", value),
      NULL, 0, 2000, NULL, NULL); g_assert_nonnull(r);
}
static void method(GDBusConnection *bus, const char *sender, const char *path, const char *interface,
    const char *method_name, GVariant *args, GDBusMethodInvocation *invocation, gpointer data) {
  (void)bus; (void)sender; (void)path; (void)interface; (void)data;
  if (!strcmp(method_name, "GetStatus")) {
    GVariantBuilder values; g_variant_builder_init(&values, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&values, "{sv}", "control_version", g_variant_new_uint32(1));
    g_variant_builder_add(&values, "{sv}", "paused", g_variant_new_boolean(paused));
    status_calls++;
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(a{sv})", &values));
  } else if (!strcmp(method_name, "ListRequests") || !strcmp(method_name, "ListClients")) {
    GVariantBuilder rows; g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(aa{sv})", &rows));
  } else if (!strcmp(method_name, "SetPaused")) {
    g_variant_get(args, "(b)", &paused); pause_calls++;
    g_dbus_method_invocation_return_value(invocation, NULL);
    g_dbus_connection_emit_signal(connection, NULL, "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1",
        "StatusChanged", g_variant_new("(t)", (guint64)pause_calls), NULL);
  } else if (!strcmp(method_name, "RegisterStatusNotifierItem")) {
    indicator_registrations++; g_dbus_method_invocation_return_value(invocation, NULL);
  } else if (!strcmp(method_name, "GetActive")) {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
  } else g_assert_not_reached();
}
static gboolean exercise(gpointer data) {
  (void)data;
  WAIT(g_object_get_data(G_OBJECT(app), "grotto-background") && status_calls >= 2);
  GrottoBackground *background = g_object_get_data(G_OBJECT(app), "grotto-background");
  WAIT(grotto_background_get_state(background) == GROTTO_BACKGROUND_UNAVAILABLE);
  WAIT(indicator_registrations != 0);
  read_indicator(); WAIT(indicator_status != NULL);
  g_assert_cmpstr(indicator_status, ==, "Passive");
  guint indicator_subscription = g_dbus_connection_signal_subscribe(connection, NULL,
      "org.kde.StatusNotifierItem", "NewStatus", "/StatusNotifierItem", NULL, 0, indicator_changed, NULL, NULL);
  name("org.nostr.Shell.Signer1", FALSE);
  WAIT(!g_strcmp0(indicator_status, "Active"));
  read_indicator(); WAIT(indicator_status != NULL);
  g_assert_cmpstr(indicator_status, ==, "Active");
  g_dbus_connection_signal_unsubscribe(connection, indicator_subscription);
  if (!strcmp(mode, "service")) g_assert_null(gtk_application_get_windows(GTK_APPLICATION(app)));
  else g_assert_nonnull(gtk_application_get_windows(GTK_APPLICATION(app)));
  g_application_activate(app);
  GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(app));
  g_assert_true(SIGNER_IS_WINDOW(win));
  g_application_activate(app);
  g_assert_cmpuint(g_list_length(gtk_application_get_windows(GTK_APPLICATION(app))), ==, 1);
  g_action_group_activate_action(G_ACTION_GROUP(app), "settings", NULL);
  g_assert_true(gtk_application_get_active_window(GTK_APPLICATION(app)) == win);
  const char *action_names[] = { "new-identity", "settings", "review-request", "pause-signing", "close-window", "quit" };
  for (guint i = 0; i < G_N_ELEMENTS(action_names); i++) g_assert_true(g_action_group_has_action(G_ACTION_GROUP(app), action_names[i]));
  g_auto(GStrv) accel = gtk_application_get_accels_for_action(GTK_APPLICATION(app), "app.close-window");
  g_assert_cmpstr(accel[0], ==, "<Control>w");
  g_autoptr(GFile) uri = g_file_new_for_uri("bunker://test?secret=never-log-this");
  GFile *files[] = { uri }; g_application_open(app, files, 1, "");
  g_assert_cmpuint(g_list_length(gtk_application_get_windows(GTK_APPLICATION(app))), ==, 1);
  g_clear_object(&uri); uri = g_file_new_for_uri("nostrconnect://test?secret=never-log-this");
  files[0] = uri; g_application_open(app, files, 1, "");
  g_assert_cmpuint(g_list_length(gtk_application_get_windows(GTK_APPLICATION(app))), ==, 1);
  g_clear_object(&uri); uri = g_file_new_for_uri("nostr:test"); files[0] = uri;
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*does not handle this URI scheme*");
  g_application_open(app, files, 1, ""); g_test_assert_expected_messages();
  GrottoApprovalController *controller = g_object_get_data(G_OBJECT(app), "grotto-approval-controller");
  g_settings_set_boolean(settings, "pause-on-screen-lock", TRUE);
  g_dbus_connection_emit_signal(connection, NULL, "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver",
      "ActiveChanged", g_variant_new("(b)", TRUE), NULL);
  WAIT(pause_calls == 1 && app_paused()); g_assert_true(paused);
  WAIT(grotto_approval_controller_get_holds(controller) == 0);
  guint status_before = status_calls;
  g_dbus_connection_emit_signal(connection, NULL, "/org/gnome/ScreenSaver", "org.gnome.ScreenSaver",
      "ActiveChanged", g_variant_new("(b)", FALSE), NULL);
  /* A GetStatus request is the asynchronous barrier, not a sleep. */
  g_signal_emit_by_name(controller, "screen-lock-changed", FALSE);
  g_assert_cmpuint(pause_calls, ==, 1);
  g_assert_true(app_paused());
  g_action_group_activate_action(G_ACTION_GROUP(app), "pause-signing", NULL);
  WAIT(pause_calls == 2 && status_calls > status_before && !app_paused()); g_assert_false(paused);
  g_action_group_activate_action(G_ACTION_GROUP(app), "close-window", NULL);
  /* Dialog close can deny an open review; this fixture has no requests. */
  WAIT(gtk_application_get_windows(GTK_APPLICATION(app)) == NULL);
  g_assert_cmpuint(grotto_background_get_holds(background), ==, 1);
  g_application_activate(app);
  g_assert_cmpuint(g_list_length(gtk_application_get_windows(GTK_APPLICATION(app))), ==, 1);
  done = TRUE;
  g_action_group_activate_action(G_ACTION_GROUP(app), "quit", NULL);
  return G_SOURCE_REMOVE;
}
static int worker(const char *value) {
  int argc = 1;
  char *args[] = { "desktop-worker", NULL };
  char **init_argv = args;
  g_test_init(&argc, &init_argv, NULL);
  mode = value;
  nostrc_test_tolerate_gdk_frame_warning(); gtk_init(); adw_init();
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
  settings = g_settings_new("org.nostr.Grotto");
  g_settings_set_boolean(settings, "onboarding-completed", TRUE);
  g_settings_set_boolean(settings, "background-disclosure-accepted", TRUE);
  connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  const char *xml = "<node><interface name='org.nostr.Grotto.Control1'>"
      "<method name='GetStatus'><arg type='a{sv}' direction='out'/></method>"
      "<method name='ListRequests'><arg type='aa{sv}' direction='out'/></method>"
      "<method name='ListClients'><arg type='aa{sv}' direction='out'/></method>"
      "<method name='SetPaused'><arg type='b' direction='in'/></method></interface></node>";
  g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(xml, NULL);
  static const GDBusInterfaceVTable table = { .method_call = method };
  g_assert_cmpuint(g_dbus_connection_register_object(connection, "/org/nostr/signer/grotto", info->interfaces[0], &table, NULL, NULL, NULL), >, 0);
  g_autoptr(GDBusNodeInfo) screen = g_dbus_node_info_new_for_xml(
      "<node><interface name='org.gnome.ScreenSaver'><method name='GetActive'><arg type='b' direction='out'/></method></interface></node>", NULL);
  g_assert_cmpuint(g_dbus_connection_register_object(connection, "/org/gnome/ScreenSaver", screen->interfaces[0], &table, NULL, NULL, NULL), >, 0);
  g_autoptr(GDBusNodeInfo) watcher = g_dbus_node_info_new_for_xml(
      "<node><interface name='org.kde.StatusNotifierWatcher'><method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method></interface></node>", NULL);
  g_assert_cmpuint(g_dbus_connection_register_object(connection, "/StatusNotifierWatcher", watcher->interfaces[0], &table, NULL, NULL, NULL), >, 0);
  name("org.nostr.Signer", TRUE); name("org.gnome.ScreenSaver", TRUE);
  name("org.kde.StatusNotifierWatcher", TRUE); name("org.nostr.Shell.Signer1", TRUE);
  app = G_APPLICATION(grotto_application_new(G_APPLICATION_DEFAULT_FLAGS));
  g_idle_add(exercise, NULL);
  char *argv[] = { "desktop-worker", !strcmp(mode, "service") ? "--gapplication-service" :
                                      !strcmp(mode, "settings") ? "--settings" : "--new-identity", NULL };
  int status = g_application_run(app, 2, argv);
  g_assert_true(done);
  GrottoBackground *bg = g_object_get_data(G_OBJECT(app), "grotto-background");
  g_assert_cmpuint(grotto_background_get_holds(bg), ==, 0);
  GrottoApprovalController *controller = g_object_get_data(G_OBJECT(app), "grotto-approval-controller");
  g_assert_cmpuint(grotto_approval_controller_get_holds(controller), ==, 0);
  while (gtk_application_get_windows(GTK_APPLICATION(app))) gtk_window_destroy(gtk_application_get_windows(GTK_APPLICATION(app))->data);
  g_object_unref(app); g_object_unref(settings); g_object_unref(connection);
  g_free(indicator_status);
  return status;
}
static void completed(GObject *source, GAsyncResult *result, gpointer data) {
  gboolean *finished = data;
  g_autoptr(GError) error = NULL;
  g_assert_true(g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error));
  g_assert_no_error(error); *finished = TRUE;
}
int main(int argc, char **argv) {
  gboolean child = argc == 3 && !strcmp(argv[1], "--worker");
  const char *value = child ? argv[2] : NULL;
  g_assert_cmpstr(g_getenv("NOSTRC_TEST_ISOLATION"), ==, "1");
  if (child) return worker(value);
  int test_argc = 1; g_test_init(&test_argc, &argv, NULL);
  if (!nostrc_test_bus_available() || !g_getenv("DISPLAY")) return 77;
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE); nostrc_test_bus_up(bus);
  const char *modes[] = { "service", "settings", "new-identity" };
  for (guint i = 0; i < G_N_ELEMENTS(modes); i++) {
    gboolean finished = FALSE;
    g_autoptr(GSubprocess) process = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, NULL, argv[0], "--worker", modes[i], NULL);
    g_assert_nonnull(process);
    g_subprocess_wait_check_async(process, NULL, completed, &finished);
    /* Let the worker's own per-operation bound provide the failing source line. */
    guint timeout = g_timeout_add_seconds(30, bound, GINT_TO_POINTER(__LINE__));
    while (!finished) g_main_context_iteration(NULL, TRUE);
    g_source_remove(timeout);
  }
  nostrc_test_bus_down(bus); return 0;
}
