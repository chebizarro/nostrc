/* Socket handles across hosts. gof_socket_t is an int descriptor on POSIX
 * and a Winsock SOCKET on Windows, where sockets are closed with closesocket,
 * raw socket calls report through WSAGetLastError (gof_socket_errno) and
 * libgo's overlapped I/O never blocks, so no non-blocking mode is needed. */
#ifndef FIPSD_TRANSPORT_SOCKOS_H
#define FIPSD_TRANSPORT_SOCKOS_H
#include "libgo/fiber.h"
#include <errno.h>
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* Close a socket, keeping errno for the caller's error path. */
static inline void fipsd_socket_close(gof_socket_t s)
{
    if (s == GOF_INVALID_SOCKET) return;
    int saved = errno;
#if defined(_WIN32)
    (void)closesocket(s);
#else
    (void)close(s);
#endif
    errno = saved;
}

/* Non-blocking and close-on-exec on POSIX; not inherited by child processes
 * on Windows. 0 or -1 with errno. */
static inline int fipsd_socket_prepare(gof_socket_t s)
{
#if defined(_WIN32)
    if (SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0)) return 0;
    errno = EBADF;
    return -1;
#else
    return fcntl(s, F_SETFL, O_NONBLOCK) || fcntl(s, F_SETFD, FD_CLOEXEC) ? -1 : 0;
#endif
}
#endif
