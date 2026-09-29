/* nostrc-test-bus.h — a private D-Bus bus for GLib tests that can neither
 * abort on macOS nor outlive the test. Use it instead of GTestDBus.
 *
 * Why not GTestDBus (nostrc-doif, nostrc-ic36, nostrc-qp24.8.5):
 *
 *  - On macOS GLib polls with select(). When GDBus closes a connection it can
 *    close the socket while the worker's read source is still attached; the
 *    next select() fails with EBADF and g_test_init() makes the resulting
 *    "poll(2) failed due to: Bad file descriptor" warning fatal (Linux poll()
 *    only reports POLLNVAL). GLib keeps the session bus singleton as a weak
 *    ref, so a library that calls g_bus_get() reconnects and disconnects on
 *    every use and hits this about once in ten runs under load. GTestDBus
 *    teardown closes connections the same way.
 *  - The GTestDBus dbus-daemon inherits the test's stdout/stderr, and its
 *    crash watcher never kills the daemon on macOS. After an abort the daemon
 *    holds ctest's output pipe, so ctest reports a 30 s timeout instead of the
 *    failure, and the daemon is orphaned.
 *
 * What this helper does instead:
 *
 *  - Every daemon (the bus, plus anything started with
 *    nostrc_test_bus_spawn_supervised()) runs under a small sh "lifeline"
 *    supervisor. The supervisor's fd 3 is the read end of a pipe whose only
 *    write end this process holds (close-on-exec, so no other child gets it).
 *    However the test ends (nostrc_test_bus_down(), an assertion abort,
 *    SIGKILL), the read returns EOF, the supervisor terminates its daemon, and
 *    the bus's supervisor then removes the bus's throwaway directories (one
 *    under TMPDIR, plus a short one under /tmp for the socket, since a
 *    socket path must fit in 104 bytes on macOS).
 *  - Daemon output goes to log files in that directory, never to the test's
 *    stdout or stderr, so nothing that could outlive the test holds ctest's
 *    pipes.
 *  - In session mode (the default) the bus is exported as
 *    DBUS_SESSION_BUS_ADDRESS and one session-bus connection (the GLib
 *    singleton that g_bus_get() returns) is held until nostrc_test_bus_down(),
 *    so libraries never connect and disconnect it per call.
 *  - The test never closes a connection to a live bus. nostrc_test_bus_down()
 *    stops the daemon first; each connection then only sees its peer vanish
 *    (a failed read, which is clean on every platform) and is released once
 *    it reports itself closed.
 *
 * Rules for tests:
 *
 *  - Get bus connections from nostrc_test_bus_connect() (or g_bus_get() in
 *    session mode). Never g_dbus_connection_close() them and never drop the
 *    last reference while the bus is up; the bus owns them.
 *  - Prefer one bus per test binary (up before g_test_run(), down after).
 *    A bus per test case also works, but the session-bus singleton must not
 *    be referenced from anywhere else across cases: nostrc_test_bus_up()
 *    fails if a stale one from an earlier bus is still alive.
 *  - Code under test that closes its own connections still exercises the
 *    GDBus close path. On macOS that can produce one transient
 *    "poll(2) failed due to: Bad file descriptor" warning per close (Linux's
 *    poll() reports POLLNVAL instead and GLib stays silent), so from the first
 *    nostrc_test_bus_up() on, a bounded number of exactly that GLib warning
 *    are logged but not fatal. This wraps the default log handler and installs
 *    g_test_log_set_fatal_handler(); a test must not install its own.
 *
 * Typical use:
 *
 *   int main(int argc, char **argv) {
 *     g_test_init(&argc, &argv, NULL);
 *     NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
 *     nostrc_test_bus_up(bus);            // exports DBUS_SESSION_BUS_ADDRESS
 *     GDBusConnection *service = nostrc_test_bus_connect(bus);
 *     ... register objects on service, g_test_add_func(...) ...
 *     int status = g_test_run();
 *     nostrc_test_bus_down(bus);
 *     return status;
 *   }
 *
 * Unix only (it depends on fork/exec, /bin/sh and gio-unix). Not thread-safe
 * apart from nostrc_test_bus_connect(), which any thread may call.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NOSTRC_TEST_BUS_H
#define NOSTRC_TEST_BUS_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NostrcTestBus NostrcTestBus;

typedef enum {
  NOSTRC_TEST_BUS_FLAGS_NONE = 0,
  /* Leave DBUS_SESSION_BUS_ADDRESS and the session-bus singleton alone: the
   * bus is reachable only through nostrc_test_bus_connect() and its address. */
  NOSTRC_TEST_BUS_FLAGS_NOT_SESSION = 1 << 0,
} NostrcTestBusFlags;

/* TRUE when dbus-daemon is installed, so a test can skip (exit 77) cleanly
 * instead of failing in nostrc_test_bus_up(). */
gboolean nostrc_test_bus_available(void);

NostrcTestBus *nostrc_test_bus_new(NostrcTestBusFlags flags);

/* Makes the .service files in path activatable on the bus (a <servicedir>).
 * Call before nostrc_test_bus_up(); path stays owned by the caller.
 * Activated services inherit the daemon's log, not the test's pipes. */
void nostrc_test_bus_add_service_dir(NostrcTestBus *bus, const gchar *path);

/* Starts the bus and returns once it accepts connections. In session mode it
 * also exports DBUS_SESSION_BUS_ADDRESS (unsetting DBUS_STARTER_ADDRESS and
 * DBUS_STARTER_BUS_TYPE, as GTestDBus does) and takes the session-bus
 * connection it holds until down. Aborts the test with the daemon's log if
 * the daemon cannot start. */
void nostrc_test_bus_up(NostrcTestBus *bus);

const gchar *nostrc_test_bus_get_address(NostrcTestBus *bus);

/* The bus's throwaway directory under TMPDIR (config, logs; free for the
 * test's own files). Removed by the supervisor once every supervised daemon
 * has stopped, including after an abort. */
const gchar *nostrc_test_bus_get_dir(NostrcTestBus *bus);

/* A new message-bus connection. (transfer none): the bus owns it until
 * nostrc_test_bus_down(); do not close or unref it. Callers may take their
 * own reference if they drop it only after down. Its signals and method
 * calls dispatch in the calling thread's thread-default main context, as
 * with g_dbus_connection_new_for_address_sync(). */
GDBusConnection *nostrc_test_bus_connect(NostrcTestBus *bus);

/* Runs argv under the bus's lifeline, with stdin read from stdin_path (NULL
 * for /dev/null) and stdout+stderr written to log_name in the bus
 * directory. It is stopped with SIGTERM by nostrc_test_bus_down() or when
 * the test dies. (transfer none): the supervisor process, owned by the bus.
 * Call after nostrc_test_bus_up(). */
GSubprocess *nostrc_test_bus_spawn_supervised(NostrcTestBus *bus, const gchar *log_name,
                                              const gchar *stdin_path,
                                              const gchar *const *argv);

/* Prints a log from the bus directory to stderr (for failure diagnostics). */
void nostrc_test_bus_dump_log(NostrcTestBus *bus, const gchar *log_name);

/* Flushes every connection the bus owns, stops all supervised daemons and
 * the bus, waits (bounded) until those connections report closed, releases
 * them and frees bus. In session mode it unsets DBUS_SESSION_BUS_ADDRESS
 * again, as g_test_dbus_down() does. */
void nostrc_test_bus_down(NostrcTestBus *bus);

G_END_DECLS

#endif /* NOSTRC_TEST_BUS_H */
