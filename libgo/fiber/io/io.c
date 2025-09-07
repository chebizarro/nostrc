#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "../include/libgo/fiber.h"
#include "netpoll.h"
#include "../sched/sched.h"
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <pthread.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef ENOTSUP
#define ENOTSUP 45
#endif
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#endif

static int set_nonblock(int fd) {
#if defined(_WIN32)
  (void)fd; return 0;
#else
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  if (flags & O_NONBLOCK) return 0;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

static uint64_t now_ns(void) {
  struct timespec ts;
#if defined(CLOCK_MONOTONIC)
  clock_gettime(CLOCK_MONOTONIC, &ts);
#else
  clock_gettime(CLOCK_REALTIME, &ts);
#endif
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t deadline_from_ms(int timeout_ms) {
  if (timeout_ms < 0) return 0; /* 0 => no deadline */
  return now_ns() + (uint64_t)timeout_ms * 1000000ull;
}

/* ---------------- IO waiter registry (per-fd queues) ---------------- */
typedef struct gof_waiter { /* single waiter node */
  gof_fiber *f;                 /* waiting fiber */
  struct gof_waiter *next;
} gof_waiter;

typedef struct gof_fdwait {     /* fd entry with separate read/write queues */
  int fd;
  gof_waiter *rd_head, *rd_tail;
  gof_waiter *wr_head, *wr_tail;
  struct gof_fdwait *next;
} gof_fdwait;

/* Simple hash map fd -> queues */
#define FDWAIT_BUCKETS 128
static gof_fdwait *fd_buckets[FDWAIT_BUCKETS];
static int ready_cb_installed = 0;
static pthread_mutex_t io_mu = PTHREAD_MUTEX_INITIALIZER;

static unsigned fd_hash(int fd){ return ((unsigned)fd) & (FDWAIT_BUCKETS-1); }

static gof_fdwait* fdwait_get(int fd, int create){
  unsigned h = fd_hash(fd);
  gof_fdwait *e = fd_buckets[h];
  while (e) { if (e->fd == fd) return e; e = e->next; }
  if (!create) return NULL;
  e = (gof_fdwait*)calloc(1, sizeof(*e));
  if (!e) return NULL;
  e->fd = fd; e->next = fd_buckets[h]; fd_buckets[h] = e; return e;
}

static void waiter_push(gof_waiter **head, gof_waiter **tail, gof_fiber *f){
  gof_waiter *w = (gof_waiter*)malloc(sizeof(*w));
  if (!w) return;
  w->f = f;
  w->next = NULL;
  if (*tail) { (*tail)->next = w; *tail = w; }
  else { *head = *tail = w; }
}

static gof_fiber* waiter_pop(gof_waiter **head, gof_waiter **tail){
  gof_waiter *w = *head; if (!w) return NULL; *head = w->next; if (!*head) *tail = NULL; gof_fiber *f = w->f; free(w); return f;
}

static void io_waiter_add(int fd, int events, gof_fiber *f) {
  pthread_mutex_lock(&io_mu);
  /* Remove any existing waiter node for this fiber across all queues to avoid duplicates */
  for (unsigned i = 0; i < FDWAIT_BUCKETS; ++i) {
    for (gof_fdwait *e = fd_buckets[i]; e; e = e->next) {
      gof_waiter **pp = &e->rd_head; gof_waiter *prev = NULL; gof_waiter *cur = e->rd_head;
      while (cur) { if (cur->f == f) { if (prev) prev->next = cur->next; else *pp = cur->next; if (cur == e->rd_tail) e->rd_tail = prev; free(cur); break; } prev = cur; cur = cur->next; }
      pp = &e->wr_head; prev = NULL; cur = e->wr_head;
      while (cur) { if (cur->f == f) { if (prev) prev->next = cur->next; else *pp = cur->next; if (cur == e->wr_tail) e->wr_tail = prev; free(cur); break; } prev = cur; cur = cur->next; }
    }
  }
  gof_fdwait *e = fdwait_get(fd, 1); if (e) {
    if (events & GOF_POLL_READ) waiter_push(&e->rd_head, &e->rd_tail, f);
    if (events & GOF_POLL_WRITE) waiter_push(&e->wr_head, &e->wr_tail, f);
  }
  pthread_mutex_unlock(&io_mu);
}

static gof_fiber* io_waiter_take_one(int fd, int events) {
  pthread_mutex_lock(&io_mu);
  gof_fdwait *e = fdwait_get(fd, 0);
  gof_fiber *res = NULL;
  if (e) {
    if ((events & GOF_POLL_READ) && e->rd_head) {
      res = waiter_pop(&e->rd_head, &e->rd_tail);
    } else if ((events & GOF_POLL_WRITE) && e->wr_head) {
      res = waiter_pop(&e->wr_head, &e->wr_tail);
    }
  }
  pthread_mutex_unlock(&io_mu);
  return res;
}

static void io_waiter_remove_by_fiber(gof_fiber *f) {
  pthread_mutex_lock(&io_mu);
  for (unsigned i = 0; i < FDWAIT_BUCKETS; ++i) {
    for (gof_fdwait *e = fd_buckets[i]; e; e = e->next) {
      /* remove once from read */
      gof_waiter **pp = &e->rd_head; gof_waiter *prev = NULL; gof_waiter *cur = e->rd_head;
      while (cur) { if (cur->f == f) { if (prev) prev->next = cur->next; else *pp = cur->next; if (cur == e->rd_tail) e->rd_tail = prev; free(cur); break; } prev = cur; cur = cur->next; }
      /* remove once from write */
      pp = &e->wr_head; prev = NULL; cur = e->wr_head;
      while (cur) { if (cur->f == f) { if (prev) prev->next = cur->next; else *pp = cur->next; if (cur == e->wr_tail) e->wr_tail = prev; free(cur); break; } prev = cur; cur = cur->next; }
    }
  }
  pthread_mutex_unlock(&io_mu);
}

int gof_io_have_waiters(void) {
  int res = 0;
  pthread_mutex_lock(&io_mu);
  for (unsigned i = 0; i < FDWAIT_BUCKETS; ++i) {
    for (gof_fdwait *e = fd_buckets[i]; e; e = e->next) {
      if (e->rd_head || e->wr_head) { res = 1; goto out; }
    }
  }
out:
  pthread_mutex_unlock(&io_mu);
  return res;
}

static void on_ready(int fd, int events) {
  /* Called from netpoll backend when fd is ready; wake one matching fiber. */
  gof_fiber *f = io_waiter_take_one(fd, events);
  if (f) {
    int pidx = gof_sched_current_poller_index();
    if (pidx >= 0) {
      gof_sched_make_runnable_from_poller(f, pidx);
    } else {
      gof_sched_make_runnable(f);
    }
  }
}

static void ensure_ready_callback(void) {
  if (!ready_cb_installed) {
    gof_netpoll_set_ready_callback(on_ready);
    ready_cb_installed = 1;
  }
}

static int wait_event_with_deadline(int fd, int events, uint64_t deadline_ns) {
  /* Best-effort: arm interest and cooperatively park until deadline. */
  (void)gof_netpoll_init();
  ensure_ready_callback();
  (void)gof_netpoll_arm(fd, events, deadline_ns);
  /* Register as waiter so readiness can wake us. */
  gof_fiber *self = gof_sched_current();
  if (self) io_waiter_add(fd, events, self);
  if (deadline_ns == 0) {
    /* No timeout specified: park in small slices to stay responsive */
    for(;;) {
      uint64_t slice = now_ns() + 2 * 1000000ull; /* 2ms */
      gof_sched_park_until(slice);
      /* caller will re-attempt IO */
      /* Clean waiter entry if still present to avoid leaks */
      if (self) io_waiter_remove_by_fiber(self);
      return 0;
    }
  } else {
    /* Park until deadline. */
    gof_sched_park_until(deadline_ns);
    if (self) io_waiter_remove_by_fiber(self);
    return 0;
  }
}

ssize_t gof_read(int fd, void *buf, size_t n) {
#if defined(_WIN32)
  (void)fd;(void)buf;(void)n; errno = ENOTSUP; return -1;
#else
  (void)set_nonblock(fd);
  for(;;) {
    ssize_t r = read(fd, buf, n);
    if (r >= 0) return r;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      /* Park cooperatively and retry. No explicit timeout for read(). */
      (void)wait_event_with_deadline(fd, GOF_POLL_READ, 0);
      continue;
    }
    return -1;
  }
#endif
}

ssize_t gof_write(int fd, const void *buf, size_t n) {
#if defined(_WIN32)
  (void)fd;(void)buf;(void)n; errno = ENOTSUP; return -1;
#else
  (void)set_nonblock(fd);
  const char *p = (const char*)buf;
  size_t left = n;
  while (left > 0) {
    ssize_t w = write(fd, p, left);
    if (w >= 0) { left -= (size_t)w; p += w; continue; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      (void)wait_event_with_deadline(fd, GOF_POLL_WRITE, 0);
      continue;
    }
    return -1;
  }
  return (ssize_t)n;
#endif
}

int gof_connect(int fd, const struct sockaddr *sa, socklen_t slen, int timeout_ms) {
#if defined(_WIN32)
  (void)fd;(void)sa;(void)slen;(void)timeout_ms; errno = ENOTSUP; return -1;
#else
  (void)set_nonblock(fd);
  int r = connect(fd, sa, slen);
  if (r == 0) return 0;
  if (errno == EINPROGRESS) {
    uint64_t dl = deadline_from_ms(timeout_ms);
    for(;;) {
      (void)wait_event_with_deadline(fd, GOF_POLL_WRITE, dl);
      /* Check connect completion */
      int err = 0; socklen_t elen = sizeof(err);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&err, &elen) == 0) {
        if (err == 0) return 0;
        errno = err; return -1;
      }
      /* If deadline set and passed, timeout */
      if (dl != 0 && now_ns() >= dl) { errno = ETIMEDOUT; return -1; }
      /* Otherwise retry until completion */
    }
  }
  return -1;
#endif
}

int gof_accept(int fd, struct sockaddr *sa, socklen_t *slen, int timeout_ms) {
#if defined(_WIN32)
  (void)fd;(void)sa;(void)slen;(void)timeout_ms; errno = ENOTSUP; return -1;
#else
  (void)set_nonblock(fd);
  uint64_t dl = deadline_from_ms(timeout_ms);
  for(;;) {
    int c = accept(fd, sa, slen);
    if (c >= 0) return c;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      (void)wait_event_with_deadline(fd, GOF_POLL_READ, dl);
      if (dl != 0 && now_ns() >= dl) { errno = ETIMEDOUT; return -1; }
      continue;
    }
    return -1;
  }
#endif
}
