#ifndef LIBGO_FIBER_IO_NETPOLL_H
#define LIBGO_FIBER_IO_NETPOLL_H
#include <stdint.h>

/* Event flags */
#define GOF_POLL_READ  0x1
#define GOF_POLL_WRITE 0x2

int  gof_netpoll_init(void);
int  gof_netpoll_arm(int fd, int events, uint64_t deadline_ns);
#if defined(__APPLE__)
/* kqueue only: gof_netpoll_arm with the write filter registered with
 * NOTE_LOWAT = write_lowat (> 0), so it fires only once that much send space
 * is available. On a connected AF_UNIX socket XNU counts the peer's receive
 * space there (filt_sowrite_common), which is how a datagram writer parks
 * until its datagram fits (nostrc-e3ps5). */
int  gof_netpoll_arm_lowat(int fd, int events, int64_t write_lowat);
#endif
int  gof_netpoll_wait(int timeout_ms);
void gof_netpoll_close(int fd);

/* Returns 1 if any fibers are waiting on I/O, 0 otherwise. (Provided by io.c) */
int  gof_io_have_waiters(void);
/* Wake pending I/O on background stop; registrations survive until consumed. */
void gof_io_cancel_waiters(void);

/* Optional: register a callback invoked from gof_netpoll_wait when an fd becomes ready. */
typedef void (*gof_netpoll_ready_cb)(int fd, int events);
void gof_netpoll_set_ready_callback(gof_netpoll_ready_cb cb);

#if defined(_WIN32)
/* Windows is completion-based, not readiness-based: gof_netpoll_arm() fails
 * with ENOTSUP and io_win32.c issues overlapped operations instead. Sockets
 * are associated with the single process-wide I/O completion port that the
 * poller threads drain in gof_netpoll_wait(), which hands every dequeued
 * OVERLAPPED to the completion callback (NULL-overlapped wake packets are
 * skipped). gof_netpoll_init() also performs the process's WSAStartup. */
/* 0 on success: *newly = 1 when this call bound the handle to the port (a
 * socket the port has not seen, including a recycled SOCKET value), 0 when it
 * already had a completion port. */
int  gof_netpoll_iocp_associate(uintptr_t sock, int *newly);
/* 1 when inline completions skip the port (FILE_SKIP_COMPLETION_PORT_ON_SUCCESS). */
int  gof_netpoll_iocp_skip_mode(uintptr_t sock);
typedef void (*gof_netpoll_completion_cb)(void *overlapped, unsigned long bytes);
void gof_netpoll_set_completion_callback(gof_netpoll_completion_cb cb);
#endif
#endif /* LIBGO_FIBER_IO_NETPOLL_H */
