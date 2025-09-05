#include "netpoll.h"
#ifdef __APPLE__
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
static int kq = -1;
static gof_netpoll_ready_cb ready_cb = NULL;

void gof_netpoll_set_ready_callback(gof_netpoll_ready_cb cb){ ready_cb = cb; }

int gof_netpoll_init(void){ if(kq!=-1) return 0; kq = kqueue(); return kq==-1?-1:0; }
int gof_netpoll_arm(int fd, int events, uint64_t deadline_ns){ (void)deadline_ns; if(kq==-1) if(gof_netpoll_init()!=0) return -1; struct kevent kev[2]; int n=0; if(events & GOF_POLL_READ){ EV_SET(&kev[n++], fd, EVFILT_READ, EV_ADD|EV_ENABLE|EV_CLEAR, 0, 0, NULL);} if(events & GOF_POLL_WRITE){ EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_ADD|EV_ENABLE|EV_CLEAR, 0, 0, NULL);} if(n==0) return 0; return kevent(kq, kev, n, NULL, 0, NULL);
}
int gof_netpoll_wait(int timeout_ms){ if(kq==-1) return -1; struct kevent kev; struct timespec ts; struct timespec *tsp = NULL; if(timeout_ms>=0){ ts.tv_sec = timeout_ms/1000; ts.tv_nsec = (timeout_ms%1000)*1000000; tsp = &ts; } int n = kevent(kq, NULL, 0, &kev, 1, tsp); if(n > 0 && ready_cb){ int events = 0; if(kev.filter == EVFILT_READ) events |= GOF_POLL_READ; if(kev.filter == EVFILT_WRITE) events |= GOF_POLL_WRITE; if(events){ ready_cb((int)kev.ident, events); } } return n; }
void gof_netpoll_close(int fd){ if(kq!=-1){ struct kevent kev; EV_SET(&kev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL); kevent(kq, &kev, 1, NULL, 0, NULL); EV_SET(&kev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL); kevent(kq, &kev, 1, NULL, 0, NULL);} }
#else
int gof_netpoll_init(void){return 0;}
int gof_netpoll_arm(int fd, int events, uint64_t deadline_ns){(void)fd;(void)events;(void)deadline_ns;return 0;}
int gof_netpoll_wait(int timeout_ms){(void)timeout_ms;return 0;}
void gof_netpoll_close(int fd){(void)fd;}
void gof_netpoll_set_ready_callback(gof_netpoll_ready_cb cb){(void)cb;}
#endif
