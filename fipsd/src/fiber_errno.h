#ifndef FIPSD_FIBER_ERRNO_H
#define FIPSD_FIBER_ERRNO_H
#include <errno.h>
#include <libgo/fiber.h>

/* Reacquire TLS on the resumed worker; never cache errno's address across a
 * fiber park. On Windows gof_socket_errno reads WSAGetLastError for RAW socket
 * calls, not the C errno published by gof_* wrappers, so use a C TLS getter.
 * Only these non-suspending TLS accesses bypass TSan, as in libgo's io.c. */
#if defined(_WIN32)
__attribute__((unused, noinline, no_sanitize("thread")))
static int fipsd_io_errno(void) { return errno; }
#else
static inline int fipsd_io_errno(void) { return gof_socket_errno(); }
#endif
__attribute__((unused, noinline, no_sanitize("thread")))
static void fipsd_io_set_errno(int error) { errno = error; }
#endif
