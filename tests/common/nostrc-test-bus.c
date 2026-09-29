/* nostrc-test-bus.c — see nostrc-test-bus.h for the why and the rules.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "nostrc-test-bus.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gunixinputstream.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

struct _NostrcTestBus {
  NostrcTestBusFlags flags;
  GPtrArray *service_dirs;  /* gchar* */
  gchar *dir;
  gchar *socket_dir;        /* short, under /tmp: see nostrc_test_bus_up() */
  gchar *address;
  gint lifeline;            /* write end; only this process holds it */
  gint lifeline_read;       /* duplicated into each supervisor as its fd 3 */
  gint done;                /* write end of the "done" pipe, see LIFELINE_SH */
  GPtrArray *supervisors;   /* GSubprocess; [0] runs the bus */
  GDBusConnection *session; /* the session singleton, session mode only */
  GMutex lock;
  GPtrArray *connections;   /* GDBusConnection, guarded by lock */
  gboolean up;
};

/* The supervisor. Arguments: two directories to remove when done ("" for
 * none), the daemon's stdin, then the daemon's argv. fd 3 is the lifeline's
 * read end: its EOF (the test closed it or died) stops the daemon. fd 4, if
 * open, belongs to the daemon alone. fd 5 is the "done" pipe: the bus's
 * supervisor holds its read end, every other supervisor and the test hold
 * write ends, so its EOF means all other supervised daemons have exited and
 * the bus directories (which may hold their HOME, logs, ...) can go. */
static const gchar LIFELINE_SH[] =
  "dir=$1 sock=$2 in=$3\n"
  "shift 3\n"
  "\"$@\" <\"$in\" 3<&- 5<&- &\n"
  "child=$!\n"
  "exec 4>&-\n"
  "read -r _ <&3\n"
  "kill -TERM \"$child\" 2>/dev/null\n"
  "wait \"$child\"\n"
  "if [ -n \"$dir\" ]; then\n"
  "  read -r _ <&5\n"
  "  rm -rf \"$dir\" \"$sock\"\n"
  "fi\n";

/* Seconds nostrc_test_bus_down() waits for connections to notice the bus
 * stopped; a failure bound, never a source of progress. */
#define NOSTRC_TEST_BUS_CLOSE_TIMEOUT 20

#ifdef __APPLE__
/* ---- The macOS select() EBADF race in code under test ----------------------
 *
 * The test's own teardown never closes a live connection, but code under test
 * may (Groundhog's GhSigner closes one private connection per signer call to
 * revoke its approval). GLib's select()-based poll on macOS then reports
 * EBADF for one iteration, until the cancelled read source is dispatched
 * and removed; Linux's poll() reports POLLNVAL for that fd instead, which
 * GLib handles silently. Only the warning's being fatal in tests makes it an
 * abort. So exactly that GLib warning is logged as a plain warning and not
 * treated as fatal, a bounded number of times per test case: repeated EBADF
 * would mean a closed fd stuck in the poll set, a real bug, and is fatal
 * again. GTest clears the fatal handler before every test case, so it is
 * armed again (and the count reset) by nostrc_test_bus_tolerate_ebadf(),
 * which nostrc_test_bus_up() and nostrc_test_bus_add_func() call. The bound
 * is NOSTRC_TEST_BUS_EBADF_TOLERANCE. */
static gint ebadf_tolerated;

static gboolean
is_select_ebadf(const gchar *domain, const gchar *message)
{
  return g_strcmp0(domain, "GLib") == 0 && message != NULL &&
         g_str_has_prefix(message, "poll(2) failed due to: Bad file descriptor");
}

/* The default log handler before ours (g_test_init() installs gtest's). */
static GLogFunc previous_default_handler;

/* g_logv() calls the log handler first and then asks the fatal handler, so
 * the handler counts and the fatal handler only reads the count. */
static void
log_forgiven_as_warning(const gchar *domain, GLogLevelFlags level, const gchar *message,
                        gpointer data)
{
  /* A forgiven EBADF goes out as an ordinary warning, not "Bail out!";
   * every other message reaches the previous handler unchanged. */
  if (is_select_ebadf(domain, message) &&
      g_atomic_int_add(&ebadf_tolerated, 1) < NOSTRC_TEST_BUS_EBADF_TOLERANCE)
    level &= ~G_LOG_FLAG_FATAL;
  previous_default_handler(domain, level, message, data);
}

static gboolean
fatal_unless_forgiven(const gchar *domain, GLogLevelFlags level, const gchar *message,
                      gpointer data)
{
  (void)level;
  (void)data;
  return !(is_select_ebadf(domain, message) &&
           g_atomic_int_get(&ebadf_tolerated) <= NOSTRC_TEST_BUS_EBADF_TOLERANCE);
}

#endif

void
nostrc_test_bus_tolerate_ebadf(void)
{
#ifdef __APPLE__
  static gsize installed;
  /* The default handler wrap is process-wide and stays. */
  if (g_once_init_enter(&installed)) {
    previous_default_handler = g_log_set_default_handler(log_forgiven_as_warning, NULL);
    g_once_init_leave(&installed, 1);
  }
  /* The fatal handler lasts only until the next test case starts. */
  g_atomic_int_set(&ebadf_tolerated, 0);
  g_test_log_set_fatal_handler(fatal_unless_forgiven, NULL);
#endif
}

static void
run_tolerant(gconstpointer data)
{
  nostrc_test_bus_tolerate_ebadf();
  (*(const GTestFunc *)data)();
}

void
nostrc_test_bus_add_func(const gchar *testpath, GTestFunc test_func)
{
  g_return_if_fail(testpath != NULL && test_func != NULL);
  GTestFunc *boxed = g_new(GTestFunc, 1);
  *boxed = test_func;
  g_test_add_data_func_full(testpath, boxed, run_tolerant, g_free);
}

gboolean
nostrc_test_bus_available(void)
{
  g_autofree gchar *daemon = g_find_program_in_path("dbus-daemon");
  return daemon != NULL;
}

NostrcTestBus *
nostrc_test_bus_new(NostrcTestBusFlags flags)
{
  NostrcTestBus *bus = g_new0(NostrcTestBus, 1);
  bus->flags = flags;
  bus->service_dirs = g_ptr_array_new_with_free_func(g_free);
  bus->supervisors = g_ptr_array_new_with_free_func(g_object_unref);
  bus->connections = g_ptr_array_new_with_free_func(g_object_unref);
  bus->lifeline = bus->lifeline_read = bus->done = -1;
  g_mutex_init(&bus->lock);
  return bus;
}

void
nostrc_test_bus_add_service_dir(NostrcTestBus *bus, const gchar *path)
{
  g_return_if_fail(bus != NULL && !bus->up && path != NULL);
  g_ptr_array_add(bus->service_dirs, g_strdup(path));
}

static GSubprocess *
spawn_supervised(NostrcTestBus *bus, gboolean remove_dirs, const gchar *log_name,
                 const gchar *stdin_path, gint daemon_fd, gint done_fd,
                 const gchar *const *daemon_argv)
{
  GError *error = NULL;
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDERR_MERGE);
  g_autofree gchar *log = g_build_filename(bus->dir, log_name, NULL);
  g_subprocess_launcher_set_stdin_file_path(launcher, "/dev/null");
  g_subprocess_launcher_set_stdout_file_path(launcher, log);
  /* The launcher owns these copies and closes them when unreffed. */
  gint lifeline_copy = fcntl(bus->lifeline_read, F_DUPFD_CLOEXEC, 3);
  if (lifeline_copy < 0)
    g_error("nostrc-test-bus: dup: %s", g_strerror(errno));
  g_subprocess_launcher_take_fd(launcher, lifeline_copy, 3);
  if (daemon_fd >= 0)
    g_subprocess_launcher_take_fd(launcher, daemon_fd, 4);
  g_subprocess_launcher_take_fd(launcher, done_fd, 5);

  GPtrArray *argv = g_ptr_array_new();
  g_ptr_array_add(argv, (gpointer)"/bin/sh");
  g_ptr_array_add(argv, (gpointer)"-c");
  g_ptr_array_add(argv, (gpointer)LIFELINE_SH);
  g_ptr_array_add(argv, (gpointer)"nostrc-test-bus-lifeline");
  g_ptr_array_add(argv, (gpointer)(remove_dirs ? bus->dir : ""));
  g_ptr_array_add(argv, (gpointer)(remove_dirs ? bus->socket_dir : ""));
  g_ptr_array_add(argv, (gpointer)(stdin_path ? stdin_path : "/dev/null"));
  for (guint i = 0; daemon_argv[i]; i++)
    g_ptr_array_add(argv, (gpointer)daemon_argv[i]);
  g_ptr_array_add(argv, NULL);
  GSubprocess *supervisor =
    g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
  if (!supervisor)
    g_error("nostrc-test-bus: cannot start %s: %s", daemon_argv[0], error->message);
  g_ptr_array_unref(argv);
  g_object_unref(launcher);
  g_ptr_array_add(bus->supervisors, supervisor);
  return supervisor;
}

static gchar *
write_config(NostrcTestBus *bus)
{
  GError *error = NULL;
  /* The GTestDBus policy: anyone may own, send and eavesdrop. */
  GString *xml = g_string_new("<busconfig>\n  <type>session</type>\n");
  g_autofree gchar *listen = g_markup_escape_text(bus->socket_dir, -1);
  g_string_append_printf(xml, "  <listen>unix:dir=%s</listen>\n", listen);
  for (guint i = 0; i < bus->service_dirs->len; i++) {
    g_autofree gchar *dir = g_markup_escape_text(g_ptr_array_index(bus->service_dirs, i), -1);
    g_string_append_printf(xml, "  <servicedir>%s</servicedir>\n", dir);
  }
  g_string_append(xml,
                  "  <policy context=\"default\">\n"
                  "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
                  "    <allow eavesdrop=\"true\"/>\n"
                  "    <allow own=\"*\"/>\n"
                  "  </policy>\n"
                  "</busconfig>\n");
  gchar *path = g_build_filename(bus->dir, "bus.conf", NULL);
  if (!g_file_set_contents(path, xml->str, -1, &error))
    g_error("nostrc-test-bus: %s", error->message);
  g_string_free(xml, TRUE);
  return path;
}

/* The server GUID in a dbus-daemon address ("...,guid=<hex>"). */
static gchar *
address_guid(const gchar *address)
{
  const gchar *guid = strstr(address, "guid=");
  if (!guid)
    return NULL;
  guid += strlen("guid=");
  return g_strndup(guid, strcspn(guid, ",;"));
}

void
nostrc_test_bus_up(NostrcTestBus *bus)
{
  g_return_if_fail(bus != NULL && !bus->up);
  GError *error = NULL;
  /* For the test case running now (see nostrc_test_bus_tolerate_ebadf()). */
  nostrc_test_bus_tolerate_ebadf();
  g_autofree gchar *daemon = g_find_program_in_path("dbus-daemon");
  if (!daemon)
    g_error("nostrc-test-bus: dbus-daemon is not installed "
            "(check nostrc_test_bus_available() to skip instead)");

  bus->dir = g_dir_make_tmp("nostrc-test-bus-XXXXXX", &error);
  if (!bus->dir)
    g_error("nostrc-test-bus: %s", error->message);
  /* The socket lives apart from bus->dir: a socket path must fit sun_path
   * (104 bytes on macOS), which a long TMPDIR would exceed. */
  bus->socket_dir = g_strdup("/tmp/nostrc-bus-XXXXXX");
  if (!g_mkdtemp_full(bus->socket_dir, 0700))
    g_error("nostrc-test-bus: cannot create a socket directory in /tmp: %s", g_strerror(errno));
  gint fds[2];
  if (!g_unix_open_pipe(fds, FD_CLOEXEC, &error))
    g_error("nostrc-test-bus: %s", error->message);
  bus->lifeline_read = fds[0];
  bus->lifeline = fds[1];
  gint done[2];
  if (!g_unix_open_pipe(done, FD_CLOEXEC, &error))
    g_error("nostrc-test-bus: %s", error->message);
  bus->done = done[1];

  g_autofree gchar *config = write_config(bus);
  g_autofree gchar *config_arg = g_strconcat("--config-file=", config, NULL);
  gint address_pipe[2];
  if (!g_unix_open_pipe(address_pipe, FD_CLOEXEC, &error))
    g_error("nostrc-test-bus: %s", error->message);
  const gchar *argv[] = { daemon, "--nofork", "--nopidfile", "--print-address=4", config_arg,
                          NULL };
  /* The bus's supervisor takes the read end of "done" and removes the dirs. */
  spawn_supervised(bus, TRUE, "dbus-daemon.log", NULL, address_pipe[1], done[0], argv);

  /* The daemon prints one line, its address; EOF means it exited first. */
  GInputStream *raw = g_unix_input_stream_new(address_pipe[0], TRUE);
  GDataInputStream *lines = g_data_input_stream_new(raw);
  bus->address = g_data_input_stream_read_line(lines, NULL, NULL, &error);
  g_object_unref(lines);
  g_object_unref(raw);
  if (!bus->address) {
    nostrc_test_bus_dump_log(bus, "dbus-daemon.log");
    g_error("nostrc-test-bus: dbus-daemon exited before printing its address%s%s",
            error ? ": " : "", error ? error->message : "");
  }
  bus->up = TRUE;

  if (bus->flags & NOSTRC_TEST_BUS_FLAGS_NOT_SESSION)
    return;
  g_setenv("DBUS_SESSION_BUS_ADDRESS", bus->address, TRUE);
  g_unsetenv("DBUS_STARTER_ADDRESS");
  g_unsetenv("DBUS_STARTER_BUS_TYPE");
  /* Held until down, so g_bus_get() users share one connection that is
   * never closed while the bus runs. */
  bus->session = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!bus->session)
    g_error("nostrc-test-bus: cannot connect to the session bus: %s", error->message);
  g_dbus_connection_set_exit_on_close(bus->session, FALSE);
  g_autofree gchar *guid = address_guid(bus->address);
  if (g_dbus_connection_is_closed(bus->session) ||
      (guid && g_strcmp0(g_dbus_connection_get_guid(bus->session), guid) != 0))
    g_error("nostrc-test-bus: the session-bus connection belongs to another bus; something "
            "still references a session connection made before this bus came up");
}

const gchar *
nostrc_test_bus_get_address(NostrcTestBus *bus)
{
  g_return_val_if_fail(bus != NULL && bus->up, NULL);
  return bus->address;
}

const gchar *
nostrc_test_bus_get_dir(NostrcTestBus *bus)
{
  g_return_val_if_fail(bus != NULL && bus->up, NULL);
  return bus->dir;
}

GDBusConnection *
nostrc_test_bus_connect(NostrcTestBus *bus)
{
  g_return_val_if_fail(bus != NULL && bus->up, NULL);
  GError *error = NULL;
  GDBusConnection *connection = g_dbus_connection_new_for_address_sync(
    bus->address,
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
    NULL, NULL, &error);
  if (!connection)
    g_error("nostrc-test-bus: cannot connect to %s: %s", bus->address, error->message);
  g_mutex_lock(&bus->lock);
  g_ptr_array_add(bus->connections, connection);
  g_mutex_unlock(&bus->lock);
  return connection;
}

GSubprocess *
nostrc_test_bus_spawn_supervised(NostrcTestBus *bus, const gchar *log_name,
                                 const gchar *stdin_path, const gchar *const *argv)
{
  g_return_val_if_fail(bus != NULL && bus->up && log_name != NULL && argv && argv[0], NULL);
  gint done_copy = fcntl(bus->done, F_DUPFD_CLOEXEC, 3);
  if (done_copy < 0)
    g_error("nostrc-test-bus: dup: %s", g_strerror(errno));
  return spawn_supervised(bus, FALSE, log_name, stdin_path, -1, done_copy, argv);
}

void
nostrc_test_bus_dump_log(NostrcTestBus *bus, const gchar *log_name)
{
  g_return_if_fail(bus != NULL && bus->dir != NULL);
  g_autofree gchar *path = g_build_filename(bus->dir, log_name, NULL);
  g_autofree gchar *contents = NULL;
  if (g_file_get_contents(path, &contents, NULL, NULL) && *contents)
    g_printerr("--- %s ---\n%s\n", log_name, contents);
}

static gboolean
all_closed(GPtrArray *connections)
{
  for (guint i = 0; i < connections->len; i++)
    if (!g_dbus_connection_is_closed(g_ptr_array_index(connections, i)))
      return FALSE;
  return TRUE;
}

static gboolean
flag_expired(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static gboolean
keep_polling(gpointer data)
{
  (void)data;
  return G_SOURCE_CONTINUE;
}

void
nostrc_test_bus_down(NostrcTestBus *bus)
{
  g_return_if_fail(bus != NULL && bus->up);
  /* No thread may connect any more; the bus is ours alone from here. */
  g_mutex_lock(&bus->lock);
  GPtrArray *connections = g_steal_pointer(&bus->connections);
  g_mutex_unlock(&bus->lock);
  gpointer session = bus->session;
  if (bus->session) {
    g_object_add_weak_pointer(G_OBJECT(bus->session), &session);
    g_ptr_array_add(connections, g_steal_pointer(&bus->session));
  }

  /* Everything queued reaches the bus before it goes away; a connection
   * the code under test already lost just reports an error here. */
  for (guint i = 0; i < connections->len; i++)
    g_dbus_connection_flush_sync(g_ptr_array_index(connections, i), NULL, NULL);

  /* The supervisors read EOF and stop their daemons; the bus's supervisor
   * then removes the directory once the others have exited. */
  close(bus->lifeline);
  close(bus->done);
  bus->lifeline = bus->done = -1;
  for (guint i = bus->supervisors->len; i > 0; i--) {
    GError *error = NULL;
    if (!g_subprocess_wait(g_ptr_array_index(bus->supervisors, i - 1), NULL, &error))
      g_error("nostrc-test-bus: waiting for a supervisor: %s", error->message);
  }
  close(bus->lifeline_read);
  bus->lifeline_read = -1;

  /* Every connection now sees its peer vanish (a failed read, not a local
   * close) and only then is released. The flag flips on the GDBus worker
   * thread and connections made on other threads signal other contexts, so
   * a short tick keeps re-checking it. */
  gboolean expired = FALSE;
  guint deadline = g_timeout_add_seconds(NOSTRC_TEST_BUS_CLOSE_TIMEOUT, flag_expired, &expired);
  guint tick = g_timeout_add(10, keep_polling, NULL);
  while (!all_closed(connections) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (expired)
    g_error("nostrc-test-bus: a connection did not notice the bus stop within %d s",
            NOSTRC_TEST_BUS_CLOSE_TIMEOUT);
  g_ptr_array_unref(connections);
  /* GLib keeps the session singleton only as a weak ref: once its pending
   * "closed" emission has run it is finalized, and the next bus (or
   * g_bus_get()) gets a fresh one. Like g_test_dbus_down(), fail if the code
   * under test still holds it. */
  while (session && !expired)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(tick);
  if (expired)
    g_error("nostrc-test-bus: the session-bus connection is still referenced %d s after its "
            "bus stopped (leaked by the code under test?)", NOSTRC_TEST_BUS_CLOSE_TIMEOUT);
  g_source_remove(deadline);
  if (!(bus->flags & NOSTRC_TEST_BUS_FLAGS_NOT_SESSION))
    g_unsetenv("DBUS_SESSION_BUS_ADDRESS");

  g_ptr_array_unref(bus->supervisors);
  g_ptr_array_unref(bus->service_dirs);
  g_mutex_clear(&bus->lock);
  g_free(bus->address);
  g_free(bus->socket_dir);
  g_free(bus->dir);
  g_free(bus);
}
