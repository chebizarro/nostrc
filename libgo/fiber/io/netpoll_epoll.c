#include "netpoll.h"
#ifdef __linux__
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>
static int epfd = -1;
static gof_netpoll_ready_cb ready_cb = NULL;

void gof_netpoll_set_ready_callback(gof_netpoll_ready_cb cb){ ready_cb = cb; }

int gof_netpoll_init(void){ if(epfd!=-1) return 0; epfd = epoll_create1(EPOLL_CLOEXEC); return epfd==-1?-1:0; }
int gof_netpoll_arm(int fd, int events, uint64_t deadline_ns){ (void)deadline_ns; if(epfd==-1) if(gof_netpoll_init()!=0) return -1; struct epoll_event ev={0}; if(events & GOF_POLL_READ) ev.events|=EPOLLIN; if(events & GOF_POLL_WRITE) ev.events|=EPOLLOUT; ev.events |= EPOLLET | EPOLLONESHOT; ev.data.fd=fd; if(epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev)!=0){ if(errno==ENOENT) return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev); } return 0; }
int gof_netpoll_wait(int timeout_ms){ if(epfd==-1) return -1; struct epoll_event ev; int n = epoll_wait(epfd, &ev, 1, timeout_ms); if(n > 0 && ready_cb){ int events = 0; if(ev.events & EPOLLIN) events |= GOF_POLL_READ; if(ev.events & EPOLLOUT) events |= GOF_POLL_WRITE; if(events){ ready_cb(ev.data.fd, events); } } return n; }
void gof_netpoll_close(int fd){ if(epfd!=-1) epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL); }
#endif
