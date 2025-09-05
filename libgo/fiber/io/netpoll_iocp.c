#include "netpoll.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int gof_netpoll_init(void){return 0;}
int gof_netpoll_arm(int fd, int events, uint64_t deadline_ns){(void)fd;(void)events;(void)deadline_ns;return 0;}
int gof_netpoll_wait(int timeout_ms){(void)timeout_ms;return 0;}
void gof_netpoll_close(int fd){(void)fd;}
#else
int gof_netpoll_init(void){return 0;}
int gof_netpoll_arm(int fd, int events, uint64_t deadline_ns){(void)fd;(void)events;(void)deadline_ns;return 0;}
int gof_netpoll_wait(int timeout_ms){(void)timeout_ms;return 0;}
void gof_netpoll_close(int fd){(void)fd;}
#endif
