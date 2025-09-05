#pragma once
#include <stdint.h>

/* Event flags */
#define GOF_POLL_READ  0x1
#define GOF_POLL_WRITE 0x2

int  gof_netpoll_init(void);
int  gof_netpoll_arm(int fd, int events, uint64_t deadline_ns);
int  gof_netpoll_wait(int timeout_ms);
void gof_netpoll_close(int fd);

/* Optional: register a callback invoked from gof_netpoll_wait when an fd becomes ready. */
typedef void (*gof_netpoll_ready_cb)(int fd, int events);
void gof_netpoll_set_ready_callback(gof_netpoll_ready_cb cb);
