/* Per-CTest accessibility services, independent of the test's own bus teardown. */
#include "nostrc-test-bus.h"
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>

static gboolean ready;
static void appeared(GDBusConnection *bus, const gchar *name, const gchar *owner, gpointer data) {
  (void)bus; (void)name; (void)owner; (void)data; ready = TRUE;
}
static gboolean expired(gpointer data) {
  (void)data; g_error("private accessibility bus did not become ready");
  return G_SOURCE_REMOVE;
}
static void child_setup(gpointer data) {
  pid_t parent = GPOINTER_TO_INT(data);
  if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
}
int main(int argc, char **argv) {
  if (argc < 2) return 125;
  /* Setup fixtures are launched too, but only isolated test cases own services. */
  if (g_strcmp0(g_getenv("NOSTRC_TEST_PRIVATE_SESSION"), "1")) {
    execvp(argv[1], &argv[1]);
    g_printerr("cannot execute test fixture %s\n", argv[1]); return 125;
  }
  /* Activated accessibility grandchildren must be reaped by this owner,
   * including in containers whose PID 1 is not a service manager. */
  if (prctl(PR_SET_CHILD_SUBREAPER, 1)) g_error("cannot own accessibility descendants");
  g_unsetenv("AT_SPI_BUS_ADDRESS");
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_PRIVATE_RUNTIME);
  nostrc_test_bus_up(bus);
  GDBusConnection *connection = nostrc_test_bus_connect(bus);
  guint watch = g_bus_watch_name_on_connection(connection, "org.a11y.Bus", 0, appeared, NULL, NULL, NULL);
  const gchar *launcher[] = {NOSTRC_A11Y_LAUNCHER, "--launch-immediately", NULL};
  nostrc_test_bus_spawn_supervised(bus, "accessibility.log", NULL, launcher);
  guint watchdog = g_timeout_add_seconds(20, expired, NULL);
  while (!ready) g_main_context_iteration(NULL, TRUE);
  g_source_remove(watchdog); g_bus_unwatch_name(watch);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) result = g_dbus_connection_call_sync(connection, "org.a11y.Bus", "/org/a11y/bus",
      "org.a11y.Bus", "GetAddress", NULL, G_VARIANT_TYPE("(s)"), 0, -1, NULL, &error);
  if (!result) g_error("private accessibility address: %s", error->message);
  const gchar *address; g_variant_get(result, "(&s)", &address);
  /* GetAddress only starts the bus. Activate the registry and wait for a real
   * reply before GTK sends its first Embed call. Keep real AT-SPI enabled. */
  g_autoptr(GDBusConnection) accessibility = g_dbus_connection_new_for_address_sync(address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, &error);
  if (!accessibility) g_error("private accessibility connection: %s", error->message);
  /* Concurrent cases share Xvfb. Without an explicit address the activated
   * registry can read another launcher's AT_SPI_BUS property from X11. Pin
   * activation on this private bus, not just the test child's environment. */
  GVariantBuilder activation_env;
  g_variant_builder_init(&activation_env, G_VARIANT_TYPE("a{ss}"));
  g_variant_builder_add(&activation_env, "{ss}", "AT_SPI_BUS_ADDRESS", address);
  g_autoptr(GVariant) updated = g_dbus_connection_call_sync(accessibility, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "UpdateActivationEnvironment",
      g_variant_new("(a{ss})", &activation_env), G_VARIANT_TYPE_UNIT,
      G_DBUS_CALL_FLAGS_NONE, 20000, NULL, &error);
  if (!updated) g_error("private accessibility activation environment: %s", error->message);
  g_autoptr(GVariant) children = g_dbus_connection_call_sync(accessibility, "org.a11y.atspi.Registry",
      "/org/a11y/atspi/accessible/root", "org.a11y.atspi.Accessible", "GetChildren", NULL,
      G_VARIANT_TYPE("(a(so))"), G_DBUS_CALL_FLAGS_NONE, 20000, NULL, &error);
  if (!children) {
    nostrc_test_bus_dump_log(bus, "accessibility.log");
    g_error("private accessibility registry: %s", error->message);
  }
  if (!g_dbus_connection_close_sync(accessibility, NULL, &error))
    g_error("private accessibility readiness connection close: %s", error->message);
  g_setenv("AT_SPI_BUS_ADDRESS", address, TRUE);
  g_autoptr(GSubprocessLauncher) spawn = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_set_child_setup(spawn, child_setup, GINT_TO_POINTER(getpid()), NULL);
  g_autoptr(GSubprocess) child = g_subprocess_launcher_spawnv(spawn, (const gchar *const *)&argv[1], &error);
  int status = 125, child_signal = 0;
  if (!child || !g_subprocess_wait(child, NULL, &error)) g_printerr("private test session: %s\n", error->message);
  else if (g_subprocess_get_if_exited(child)) status = g_subprocess_get_exit_status(child);
  else { child_signal = g_subprocess_get_term_sig(child); status = 128 + child_signal; }
  if (status) nostrc_test_bus_dump_log(bus, "accessibility.log");
  nostrc_test_bus_down(bus);
  for (;;) {
    if (waitpid(-1, NULL, 0) > 0 || errno == EINTR) continue;
    if (errno != ECHILD) g_error("cannot reap accessibility descendants");
    break;
  }
  /* CTest must still recognize a signal (not invert it via WILL_FAIL). */
  if (child_signal) { signal(child_signal, SIG_DFL); raise(child_signal); }
  return status;
}
