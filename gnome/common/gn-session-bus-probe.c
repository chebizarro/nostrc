#include "gn-session-bus-probe.h"

#ifdef G_OS_UNIX
#include <gio/gio.h>
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* TRUE: the child exited (status filled in). FALSE: the deadline passed.
 * Only the child holds the write end of the pipe, so it reads as hung up
 * the moment the child exits. */
static gboolean
wait_for_exit(int fd, pid_t pid, guint timeout_ms, int *status)
{
  gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  struct pollfd p = { .fd = fd, .events = POLLIN };
  int r;
  do {
    gint64 left_ms = MAX(deadline - g_get_monotonic_time(), 0) / 1000;
    r = poll(&p, 1, (int)left_ms);
  } while (r < 0 && errno == EINTR);
  if (r <= 0)
    return FALSE;
  while (waitpid(pid, status, 0) < 0 && errno == EINTR)
    ;
  return TRUE;
}

gboolean
gn_session_bus_fallback_if_unresponsive(guint timeout_ms)
{
  int fds[2];
#if GLIB_CHECK_VERSION(2, 78, 0)
  if (!g_unix_open_pipe(fds, O_CLOEXEC, NULL))
#else
  if (!g_unix_open_pipe(fds, FD_CLOEXEC, NULL))
#endif
    return FALSE;

  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return FALSE;
  }
  if (pid == 0) {
    /* Its own process group, so a hung dbus-launch it spawns dies with it. */
    setpgid(0, 0);
    close(fds[0]);
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    _exit(bus ? 0 : 1);
  }
  setpgid(pid, pid);
  close(fds[1]);

  int status = 0;
  gboolean exited = wait_for_exit(fds[0], pid, timeout_ms, &status);
  close(fds[0]);
  if (exited)
    return FALSE;

  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;

  /* connect() on a non-socket fails with ENOTSOCK. An empty value would
   * send GLib back to $XDG_RUNTIME_DIR/bus or dbus-launch. */
  g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/dev/null", TRUE);
  return TRUE;
}
#else
gboolean
gn_session_bus_fallback_if_unresponsive(guint timeout_ms)
{
  (void)timeout_ms;
  return FALSE;
}
#endif
