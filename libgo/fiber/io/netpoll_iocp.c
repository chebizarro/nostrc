/* Windows netpoll backend: one I/O completion port for the process.
 *
 * io_win32.c associates each socket on first use and issues overlapped
 * operations whose OVERLAPPED lives in a per-call op record; the poller
 * threads (sched.c poller_main) block here in GetQueuedCompletionStatusEx and
 * pass every completion to io_win32.c's callback, which wakes the parked
 * fiber. Several poller threads may drain the same port. */
#include "netpoll.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <errno.h>
#include <stdatomic.h>

static HANDLE g_port = NULL;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static _Atomic gof_netpoll_ready_cb ready_cb = NULL;
static _Atomic gof_netpoll_completion_cb completion_cb = NULL;

static BOOL CALLBACK iocp_init_once(PINIT_ONCE once, PVOID param, PVOID *context) {
  (void)once; (void)param; (void)context;
  /* Once per process, before any socket the runtime touches. WSAStartup is
   * reference counted, so applications may also call it themselves. */
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return FALSE;
  g_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
  return g_port != NULL;
}

int gof_netpoll_init(void) {
  if (!InitOnceExecuteOnce(&g_once, iocp_init_once, NULL, NULL)) {
    errno = ENOMEM;
    return -1;
  }
  return 0;
}

void gof_netpoll_set_ready_callback(gof_netpoll_ready_cb cb) {
  atomic_store_explicit(&ready_cb, cb, memory_order_release);
}

void gof_netpoll_set_completion_callback(gof_netpoll_completion_cb cb) {
  atomic_store_explicit(&completion_cb, cb, memory_order_release);
}

int gof_netpoll_iocp_associate(uintptr_t sock, int *newly) {
  if (gof_netpoll_init() != 0) return -1;
  *newly = 0;
  if (CreateIoCompletionPort((HANDLE)sock, g_port, 0, 0)) {
    *newly = 1;
    return 0;
  }
  DWORD error = GetLastError();
  /* ERROR_INVALID_PARAMETER: the handle already has a completion port (a
   * handle cannot be detached). Closed handles report ERROR_INVALID_HANDLE. */
  if (error == ERROR_INVALID_PARAMETER) return 0;
  errno = error == ERROR_INVALID_HANDLE ? EBADF : ENOTSOCK;
  return -1;
}

int gof_netpoll_iocp_skip_mode(uintptr_t sock) {
  /* Immediate successes then complete inline without a queued packet, and
   * the handle's event is never signalled (nothing waits on it). Without the
   * mode every operation, even an inline success, waits for its packet. */
  return SetFileCompletionNotificationModes(
      (HANDLE)sock, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE) ? 1 : 0;
}

int gof_netpoll_arm(int fd, int events, uint64_t deadline_ns) {
  (void)fd; (void)events; (void)deadline_ns;
  errno = ENOTSUP; /* completion-based: see io_win32.c */
  return -1;
}

int gof_netpoll_wait(int timeout_ms) {
  if (gof_netpoll_init() != 0) return -1;
  OVERLAPPED_ENTRY entries[64];
  ULONG n = 0;
  DWORD wait = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
  if (!GetQueuedCompletionStatusEx(g_port, entries, 64, &n, wait, FALSE))
    return GetLastError() == WAIT_TIMEOUT ? 0 : -1;
  gof_netpoll_completion_cb cb = atomic_load_explicit(&completion_cb, memory_order_acquire);
  for (ULONG i = 0; i < n; ++i) {
    /* Failed operations still dequeue their entry; the op reads its status
     * from the OVERLAPPED itself (WSAGetOverlappedResult). */
    if (entries[i].lpOverlapped && cb)
      cb(entries[i].lpOverlapped, (unsigned long)entries[i].dwNumberOfBytesTransferred);
  }
  return (int)n;
}

void gof_netpoll_close(int fd) {
  (void)fd; /* a handle's association ends when the socket is closed */
}
#endif /* _WIN32 */
