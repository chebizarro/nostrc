/* groundhog-bus-probe: Groundhog shows its window when the session bus
 * accepts connections but never answers the D-Bus AUTH handshake
 * (nostrc-v59q, nostrc-py0hg).
 *
 * A stall server listens on a Unix socket, accepts every connection and
 * never writes a byte: a launchd socket whose dbus-daemon agent cannot start
 * behaves exactly like this. The groundhog binary runs --smoke (it builds and
 * presents the real window, checks the widget tree and exits 0) with the
 * stalled socket reachable through each way GLib finds the session bus:
 *
 *   address     DBUS_SESSION_BUS_ADDRESS
 *   xdg         no address; $XDG_RUNTIME_DIR/bus (Linux user bus)
 *   autolaunch  no address, no runtime bus, $DISPLAY set (XQuartz sets it
 *               on macOS): GLib runs `dbus-launch --autolaunch`. Homebrew's
 *               launchd-enabled dbus-launch answers with the socket launchd
 *               holds under DBUS_LAUNCHD_SESSION_BUS_SOCKET, a key that lives
 *               in launchd (`launchctl getenv`), not in the process
 *               environment. A stand-in dbus-launch on PATH answers the same
 *               way without touching the user's launchd. This is the owner's
 *               nostrc-py0hg hang: the old probe saw no variable and skipped.
 *
 * Each case must reach the stall server, log the fallback and exit 0 within
 * the probe's 5 s plus startup. A healthy bus must not trigger the fallback. */
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <string.h>

#include "nostrc-test-bus.h"

#define FALLBACK_LOG "session bus did not respond"
/* The probe deadline is 5 s; the window must be up well before the 30 s
 * ctest timeout. */
#define LAUNCH_LIMIT_US (12 * G_USEC_PER_SEC)

typedef struct {
  gchar *dir;          /* short, under /tmp: Unix socket paths are short */
  gchar *socket_path;
  GSocketService *service;
  GPtrArray *held;     /* accepted connections, never answered */
} Stall;

static gboolean
on_incoming(GSocketService *service, GSocketConnection *conn, GObject *source, gpointer data)
{
  (void)service;
  (void)source;
  Stall *stall = data;
  g_ptr_array_add(stall->held, g_object_ref(conn));
  return TRUE;
}

static Stall *
stall_new(void)
{
  Stall *stall = g_new0(Stall, 1);
  g_autoptr(GError) error = NULL;
  stall->dir = g_strdup("/tmp/gh-bus-XXXXXX");
  g_assert_nonnull(g_mkdtemp(stall->dir));
  stall->socket_path = g_build_filename(stall->dir, "bus", NULL);
  stall->held = g_ptr_array_new_with_free_func(g_object_unref);
  stall->service = g_socket_service_new();
  g_autoptr(GSocketAddress) address = g_unix_socket_address_new(stall->socket_path);
  g_assert_true(g_socket_listener_add_address(G_SOCKET_LISTENER(stall->service), address,
                                              G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
                                              NULL, NULL, &error));
  g_assert_no_error(error);
  g_signal_connect(stall->service, "incoming", G_CALLBACK(on_incoming), stall);
  g_socket_service_start(stall->service);
  return stall;
}

static void
stall_free(Stall *stall)
{
  g_socket_service_stop(stall->service);
  g_socket_listener_close(G_SOCKET_LISTENER(stall->service));
  g_clear_object(&stall->service);
  g_ptr_array_unref(stall->held);
  g_unlink(stall->socket_path);
  g_rmdir(stall->dir);
  g_free(stall->socket_path);
  g_free(stall->dir);
  g_free(stall);
}

typedef struct {
  GMainLoop *loop;
  gboolean done;
  gboolean timed_out;
  gchar *err;
  GError *error;
} Run;

static void
on_communicated(GObject *source, GAsyncResult *res, gpointer data)
{
  Run *run = data;
  g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, NULL, &run->err, &run->error);
  g_subprocess_wait_async(G_SUBPROCESS(source), NULL, NULL, NULL);
  run->done = TRUE;
  g_main_loop_quit(run->loop);
}

static gboolean
on_deadline(gpointer data)
{
  Run *run = data;
  run->timed_out = TRUE;
  g_main_loop_quit(run->loop);
  return G_SOURCE_REMOVE;
}

/* Runs groundhog --smoke in env, iterating the main loop (which also serves
 * the stall server). Returns the exit status; *stderr_out gets its stderr and
 * *elapsed_us the time to exit. */
static gint
run_groundhog(GStrv env, gchar **stderr_out, gint64 *elapsed_us)
{
  const gchar *bin = g_getenv("GROUNDHOG_BIN");
  g_assert_nonnull(bin);
  g_autoptr(GSubprocessLauncher) launcher =
    g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
  g_subprocess_launcher_set_environ(launcher, env);
  g_autoptr(GError) error = NULL;
  gint64 start = g_get_monotonic_time();
  g_autoptr(GSubprocess) proc = g_subprocess_launcher_spawn(launcher, &error, bin, "--smoke", NULL);
  g_assert_no_error(error);

  Run run = { .loop = g_main_loop_new(NULL, FALSE) };
  g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_communicated, &run);
  guint deadline = g_timeout_add(LAUNCH_LIMIT_US / 1000, on_deadline, &run);
  g_main_loop_run(run.loop);
  if (run.timed_out) {
    g_subprocess_force_exit(proc);
    while (!run.done)
      g_main_context_iteration(NULL, TRUE);
  } else {
    g_source_remove(deadline);
  }
  *elapsed_us = g_get_monotonic_time() - start;
  g_main_loop_unref(run.loop);
  g_assert_no_error(run.error);
  g_test_message("groundhog stderr:\n%s", run.err ? run.err : "");
  *stderr_out = run.err;
  g_assert_false(run.timed_out);
  g_assert_true(g_subprocess_wait(proc, NULL, NULL));
  g_assert_true(g_subprocess_get_if_exited(proc));
  return g_subprocess_get_exit_status(proc);
}

static GStrv
base_env(void)
{
  GStrv env = g_get_environ();
  env = g_environ_setenv(env, "GSETTINGS_BACKEND", "memory", TRUE);
  env = g_environ_unsetenv(env, "DBUS_SESSION_BUS_ADDRESS");
  env = g_environ_unsetenv(env, "DBUS_LAUNCHD_SESSION_BUS_SOCKET");
  env = g_environ_unsetenv(env, "DBUS_STARTER_BUS_TYPE");
  env = g_environ_unsetenv(env, "DBUS_STARTER_ADDRESS");
#ifdef __APPLE__
  env = g_environ_setenv(env, "GROUNDHOG_RUN_GUI_SMOKE", "1", TRUE);
#endif
  return env;
}

static void
assert_window_despite_stall(Stall *stall, GStrv env)
{
  g_autofree gchar *err = NULL;
  gint64 elapsed = 0;
  gint status = run_groundhog(env, &err, &elapsed);
  g_test_message("groundhog exited %d after %" G_GINT64_FORMAT " ms", status, elapsed / 1000);
  /* The stalled socket was the bus GLib chose, and it was given up on. */
  g_assert_cmpuint(stall->held->len, >=, 1);
  g_assert_nonnull(strstr(err, FALLBACK_LOG));
  /* The smoke presented the window and found the whole tree. */
  g_assert_cmpint(status, ==, 0);
  g_assert_cmpint(elapsed, <, LAUNCH_LIMIT_US);
}

static void
test_stalled_address(void)
{
  Stall *stall = stall_new();
  g_autofree gchar *addr = g_strdup_printf("unix:path=%s", stall->socket_path);
  g_auto(GStrv) env = base_env();
  env = g_environ_setenv(env, "DBUS_SESSION_BUS_ADDRESS", addr, TRUE);
  assert_window_despite_stall(stall, env);
  stall_free(stall);
}

static void
test_stalled_runtime_dir(void)
{
  Stall *stall = stall_new();
  g_assert_cmpint(g_chmod(stall->dir, 0700), ==, 0);
  g_auto(GStrv) env = base_env();
  /* stall->socket_path is <dir>/bus. */
  env = g_environ_setenv(env, "XDG_RUNTIME_DIR", stall->dir, TRUE);
  assert_window_despite_stall(stall, env);
  stall_free(stall);
}

static void
test_stalled_autolaunch(void)
{
  Stall *stall = stall_new();
  g_autofree gchar *runtime = g_dir_make_tmp("gh-runtime-XXXXXX", NULL);
  g_autofree gchar *bin = g_dir_make_tmp("gh-bin-XXXXXX", NULL);
  g_autofree gchar *marker = g_build_filename(bin, "ran", NULL);
  g_autofree gchar *launch = g_build_filename(bin, "dbus-launch", NULL);
  /* --binary-syntax: NUL-terminated address, then pid and window id. */
  g_autofree gchar *script = g_strdup_printf(
    "#!/bin/sh\n: > '%s'\n"
    "printf 'unix:path=%%s\\000' '%s'\n"
    "printf '\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000\\000'\n",
    marker, stall->socket_path);
  g_assert_true(g_file_set_contents(launch, script, -1, NULL));
  g_assert_cmpint(g_chmod(launch, 0755), ==, 0);

  g_auto(GStrv) env = base_env();
  g_autofree gchar *path = g_strdup_printf("%s:%s", bin, g_getenv("PATH"));
  env = g_environ_setenv(env, "PATH", path, TRUE);
  env = g_environ_setenv(env, "XDG_RUNTIME_DIR", runtime, TRUE);
  /* GLib only autolaunches with $DISPLAY set; keep a real one for GTK. */
  env = g_environ_setenv(env, "DISPLAY", ":97", FALSE);
  assert_window_despite_stall(stall, env);
  /* GLib reached the bus through dbus-launch. */
  g_assert_true(g_file_test(marker, G_FILE_TEST_EXISTS));

  g_unlink(marker);
  g_unlink(launch);
  g_rmdir(bin);
  g_rmdir(runtime);
  stall_free(stall);
}

/* A bus that answers must keep the unique, bus-backed application. */
static void
test_healthy_bus(void)
{
  if (!nostrc_test_bus_available()) {
    g_test_skip("dbus-daemon unavailable");
    return;
  }
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus);
  g_auto(GStrv) env = base_env();
  env = g_environ_setenv(env, "DBUS_SESSION_BUS_ADDRESS", nostrc_test_bus_get_address(bus), TRUE);
  g_autofree gchar *err = NULL;
  gint64 elapsed = 0;
  gint status = run_groundhog(env, &err, &elapsed);
  nostrc_test_bus_down(bus);
  g_assert_cmpint(status, ==, 0);
  g_assert_null(strstr(err, FALLBACK_LOG));
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/bus-probe/stalled/address", test_stalled_address);
  g_test_add_func("/groundhog/bus-probe/stalled/runtime-dir", test_stalled_runtime_dir);
  g_test_add_func("/groundhog/bus-probe/stalled/autolaunch", test_stalled_autolaunch);
  g_test_add_func("/groundhog/bus-probe/healthy", test_healthy_bus);
  return g_test_run();
}
