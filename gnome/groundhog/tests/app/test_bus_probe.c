/* groundhog-bus-probe: verifies that Groundhog does not hang forever when
 * the session bus socket accepts connections but no dbus-daemon answers
 * (nostrc-v59q).
 *
 * The test creates a listening Unix socket that accepts one connection and
 * silently ignores the D-Bus AUTH handshake. It then runs the groundhog
 * binary with DBUS_SESSION_BUS_ADDRESS pointing to this stalled socket and
 * --smoke mode. The process must exit within a bounded timeout instead of
 * hanging in g_bus_get_sync(). */
#include <glib.h>
#include "nostrc-test-bus.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* A stall server: listens on a Unix socket, accepts one connection, then
 * reads and discards everything without ever responding. Runs in a child
 * process; the parent kills it when done. */
static int
stall_server(const char *socket_path)
{
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return 1;

  struct sockaddr_un sa;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  strncpy(sa.sun_path, socket_path, sizeof(sa.sun_path) - 1);

  unlink(socket_path);
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
      listen(fd, 1) != 0) {
    close(fd);
    return 1;
  }

  /* Accept one connection and stall. */
  int client = accept(fd, NULL, NULL);
  if (client >= 0) {
    char buf[256];
    /* Read the AUTH null byte and anything else, never respond. */
    while (read(client, buf, sizeof(buf)) > 0)
      ;
    close(client);
  }
  close(fd);
  return 0;
}

static void
test_stalled_bus(void)
{
  /* 1. Create a temporary socket path. */
  g_autofree gchar *tmpdir = g_dir_make_tmp("groundhog-bus-probe-XXXXXX", NULL);
  g_assert_nonnull(tmpdir);
  g_autofree gchar *socket_path = g_build_filename(tmpdir, "stall.sock", NULL);

  /* 2. Fork the stall server. */
  pid_t server_pid = fork();
  g_assert_cmpint(server_pid, >=, 0);
  if (server_pid == 0) {
    _exit(stall_server(socket_path));
  }

  /* Give the server a moment to start listening. */
  g_usleep(200 * 1000);

  /* 3. Find the groundhog binary. */
  const gchar *groundhog_bin = g_getenv("GROUNDHOG_BIN");
  if (!groundhog_bin)
    groundhog_bin = "groundhog";

  /* 4. Build the D-Bus address pointing to our stalled socket. */
  g_autofree gchar *addr = g_strdup_printf("unix:path=%s", socket_path);

  /* 5. Run groundhog --smoke with the stalled bus.
   * It must exit within 15 seconds (the probe timeout is 5 s, plus
   * some margin for process startup and shutdown). The test itself
   * times out at 30 s (set in CMakeLists.txt). */
  const gchar *schema_dir = g_getenv("GSETTINGS_SCHEMA_DIR");
  (void)schema_dir;

  GPid child_pid = 0;
  gint child_stdout = -1;
  gint child_stderr = -1;
  GError *error = NULL;

  gchar *child_argv[] = { (gchar *)groundhog_bin, "--smoke", NULL };
  /* Build envp from the current environment plus our overrides. */
  g_auto(GStrv) parent_env = g_get_environ();
  parent_env = g_environ_setenv(parent_env, "DBUS_SESSION_BUS_ADDRESS", addr, TRUE);
  parent_env = g_environ_setenv(parent_env, "DBUS_LAUNCHD_SESSION_BUS_SOCKET", "", TRUE);
  parent_env = g_environ_setenv(parent_env, "GSETTINGS_BACKEND", "memory", TRUE);
  /* On macOS, request the GUI smoke only if the current process has a display. */
#ifdef __APPLE__
  parent_env = g_environ_setenv(parent_env, "GROUNDHOG_RUN_GUI_SMOKE", "1", TRUE);
#endif

  gboolean spawned = g_spawn_async_with_pipes(
    NULL, child_argv, parent_env,
    G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
    NULL, NULL, &child_pid, NULL, &child_stdout, &child_stderr, &error);

  if (!spawned) {
    /* If the binary isn't found, skip (return 77). */
    g_test_message("Could not spawn groundhog: %s", error->message);
    g_clear_error(&error);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    g_unlink(socket_path);
    g_rmdir(tmpdir);
    /* Use g_test_skip if available, otherwise just pass. */
    g_test_skip("groundhog binary not found");
    return;
  }

  /* 6. Wait for the child with a 15-second timeout. */
  gint child_status = -1;
  gboolean timed_out = TRUE;
  for (int i = 0; i < 150; i++) {
    int r = waitpid(child_pid, &child_status, WNOHANG);
    if (r > 0) {
      timed_out = FALSE;
      break;
    }
    g_usleep(100 * 1000); /* 100 ms */
  }

  if (timed_out) {
    g_test_message("groundhog did not exit within 15 s — killing");
    kill(child_pid, SIGKILL);
    waitpid(child_pid, &child_status, 0);
  }

  /* Read stderr for diagnostic output and the honest fallback notice. */
  gboolean fallback_logged = FALSE;
  if (child_stderr >= 0) {
    char buf[4096];
    ssize_t n = read(child_stderr, buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      g_test_message("groundhog stderr: %s", buf);
      fallback_logged = strstr(buf, "session bus did not respond") != NULL;
    }
    close(child_stderr);
  }
  if (child_stdout >= 0)
    close(child_stdout);

  /* 7. Clean up the stall server. */
  kill(server_pid, SIGTERM);
  waitpid(server_pid, NULL, 0);
  g_unlink(socket_path);
  g_rmdir(tmpdir);

  /* 8. Assert: groundhog must not have timed out. */
  g_assert_false(timed_out);
  g_assert_true(fallback_logged);

  /* The smoke may exit 0 (display available, tree OK), 77 (no display,
   * skipped) or 1 (smoke failed). All are acceptable: the test only
   * verifies that the process does not hang. */
  if (WIFEXITED(child_status)) {
    int code = WEXITSTATUS(child_status);
    g_test_message("groundhog exited with status %d", code);
    g_assert_true(code == 0 || code == 1 || code == 77);
  }
}

/* Healthy bus: run groundhog --smoke against a real dbus-daemon (the one
 * that is already running for this test).  The probe must NOT trigger
 * the fallback — the smoke must get a bus connection.  We detect the
 * fallback by looking for the "session bus did not respond" log message
 * on stderr; its absence proves the healthy bus was correctly accepted. */
static void
test_healthy_bus(void)
{
  const gchar *groundhog_bin = g_getenv("GROUNDHOG_BIN");
  if (!groundhog_bin)
    groundhog_bin = "groundhog";

  /* Start an answering bus, rather than depending on the CI shell's bus. */
  if (!nostrc_test_bus_available()) {
    g_test_skip("dbus-daemon unavailable");
    return;
  }
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus);
  const gchar *addr = nostrc_test_bus_get_address(bus);

  gchar *child_argv[] = { (gchar *)groundhog_bin, "--smoke", NULL };
  g_auto(GStrv) env = g_get_environ();
  env = g_environ_setenv(env, "GSETTINGS_BACKEND", "memory", TRUE);
  env = g_environ_setenv(env, "DBUS_SESSION_BUS_ADDRESS", addr, TRUE);
#ifdef __APPLE__
  env = g_environ_setenv(env, "GROUNDHOG_RUN_GUI_SMOKE", "1", TRUE);
#endif

  GPid child_pid = 0;
  gint child_stderr = -1;
  GError *error = NULL;
  gboolean spawned = g_spawn_async_with_pipes(
    NULL, child_argv, env,
    G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
    NULL, NULL, &child_pid, NULL, NULL, &child_stderr, &error);
  if (!spawned) {
    g_test_skip("groundhog binary not found");
    g_clear_error(&error);
    nostrc_test_bus_down(bus);
    return;
  }

  /* Wait up to 15 s. */
  gint status = -1;
  gboolean timed_out = TRUE;
  for (int i = 0; i < 150; i++) {
    if (waitpid(child_pid, &status, WNOHANG) > 0) {
      timed_out = FALSE;
      break;
    }
    g_usleep(100 * 1000);
  }
  if (timed_out) {
    kill(child_pid, SIGKILL);
    waitpid(child_pid, &status, 0);
  }

  /* Read stderr and check for the fallback message. */
  gboolean probe_fired = FALSE;
  if (child_stderr >= 0) {
    char buf[8192];
    ssize_t n = read(child_stderr, buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      g_test_message("groundhog stderr: %s", buf);
      probe_fired = strstr(buf, "session bus did not respond") != NULL;
    }
    close(child_stderr);
  }

  nostrc_test_bus_down(bus);
  g_assert_false(timed_out);
  g_assert_true(WIFEXITED(status));
  g_assert_cmpint(WEXITSTATUS(status), ==, 0);
  g_assert_false(probe_fired);
  g_test_message("healthy bus correctly accepted by probe");
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/bus-probe/stalled", test_stalled_bus);
  g_test_add_func("/groundhog/bus-probe/healthy", test_healthy_bus);
  return g_test_run();
}
