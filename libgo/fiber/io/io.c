/* POSIX readiness I/O (epoll/kqueue). Windows uses io_win32.c instead. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "../include/libgo/fiber.h"
#if !defined(_WIN32)
#include "netpoll.h"
#include "../sched/sched.h"
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  if (flags & O_NONBLOCK) return 0;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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

/* errno is per-OS-thread TLS, and its accessor (__errno_location on glibc,
 * __error on macOS) is declared __attribute__((const)): the compiler may
 * cache the address across a call. A fiber that parks inside
 * wait_event_with_deadline() can resume on a DIFFERENT OS thread, so an
 * errno read or write after the park must not reuse a cached address —
 * GoFiberIoTimeoutTestW4 saw errno==0 after a timeout because the
 * ETIMEDOUT assignment landed in the previous thread's cell while the
 * caller read the new thread's (nostrc-ci). Value-capture immediately
 * after each syscall, and go through noinline helpers so every errno
 * access is evaluated on the thread that executes it. */
/* errno belongs to the OS thread, not the TSan fiber. These accessors never
 * suspend, so different fibers on one worker cannot access it concurrently.
 * Exclude ONLY these TLS loads/stores from instrumentation; adding switch or
 * errno happens-before edges would hide real application races between fibers. */
__attribute__((noinline, no_sanitize("thread"))) static int fiber_get_errno(void) { return errno; }
__attribute__((noinline, no_sanitize("thread"))) static void fiber_set_errno(int e) { errno = e; }

/* ---------------- IO waiter registry (per-fd queues) ---------------- */
typedef struct gof_waiter { /* single waiter node */
  gof_fiber *f;                 /* waiting fiber */
  struct gof_waiter *next;
} gof_waiter;

typedef struct gof_fdwait {     /* fd entry with separate read/write queues */
  int fd;
  int cancelled;
  gof_waiter *rd_head, *rd_tail;
  gof_waiter *wr_head, *wr_tail;
  struct gof_fdwait *next;
} gof_fdwait;

/* Simple hash map fd -> queues */
#define FDWAIT_BUCKETS 128
static gof_fdwait *fd_buckets[FDWAIT_BUCKETS];
static _Atomic int ready_cb_installed = 0;
static pthread_mutex_t io_mu = PTHREAD_MUTEX_INITIALIZER;
/* Active waits, including readiness already claimed but not yet consumed.
 * Read under S.mu without io_mu (the wake path locks in the opposite order).
 * Counting through consumption also prevents stop from retiring a worker while
 * on_ready has removed the last queue node but has not yet enqueued its fiber. */
static _Atomic int g_io_waiter_count = 0;

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

static int waiter_push(gof_waiter **head, gof_waiter **tail, gof_fiber *f){
  gof_waiter *w = (gof_waiter*)malloc(sizeof(*w));
  if (!w) return -1;
  w->f = f;
  w->next = NULL;
  if (*tail) { (*tail)->next = w; *tail = w; }
  else { *head = *tail = w; }
  return 0;
}

static gof_fiber* waiter_pop(gof_waiter **head, gof_waiter **tail){
  gof_waiter *w = *head; if (!w) return NULL; *head = w->next; if (!*head) *tail = NULL; gof_fiber *f = w->f; free(w);
  return f;
}

/* Remove fiber's existing waiter entry using its tracked wait_fd (O(1) bucket lookup). */
static void io_waiter_remove_fiber_locked(gof_fiber *f) {
  if (!f || f->wait_fd < 0) return;
  gof_fdwait *e = fdwait_get(f->wait_fd, 0);
  if (!e) { f->wait_fd = -1; f->wait_events = 0; return; }
  if (f->wait_events & GOF_POLL_READ) {
    gof_waiter *prev = NULL, *cur = e->rd_head;
    while (cur) { if (cur->f == f) { if (prev) prev->next = cur->next; else e->rd_head = cur->next; if (cur == e->rd_tail) e->rd_tail = prev; free(cur); break; } prev = cur; cur = cur->next; }
  }
  if (f->wait_events & GOF_POLL_WRITE) {
    gof_waiter *prev = NULL, *cur = e->wr_head;
    while (cur) { if (cur->f == f) { if (prev) prev->next = cur->next; else e->wr_head = cur->next; if (cur == e->wr_tail) e->wr_tail = prev; free(cur); break; } prev = cur; cur = cur->next; }
  }
  f->wait_fd = -1;
  f->wait_events = 0;
}

static int fdwait_events(gof_fdwait *e) {
  return (e->rd_head ? GOF_POLL_READ : 0) | (e->wr_head ? GOF_POLL_WRITE : 0);
}

int gof_io_reset_fd(gof_socket_t fd) {
  if (fd < 0) { fiber_set_errno(EINVAL); return -1; }
  pthread_mutex_lock(&io_mu);
  gof_fdwait *e = fdwait_get(fd, 1);
  int error = !e ? ENOMEM : fdwait_events(e) ? EBUSY : 0;
  if (!error) e->cancelled = 0;
  pthread_mutex_unlock(&io_mu);
  if (error) { fiber_set_errno(error); return -1; }
  return 0;
}

int gof_io_cancel_fd(gof_socket_t fd) {
  if (fd < 0) { fiber_set_errno(EINVAL); return -1; }
  pthread_mutex_lock(&io_mu);
  gof_fdwait *e = fdwait_get(fd, 1);
  if (e) {
    e->cancelled = 1;
    /* Retain queue membership until each user consumes cancellation, just
     * like scheduler stop. A readiness wake already in flight sees the latch
     * when it consumes its wait. */
    for (gof_waiter *w = e->rd_head; w; w = w->next) gof_sched_make_runnable(w->f);
    for (gof_waiter *w = e->wr_head; w; w = w->next) gof_sched_make_runnable(w->f);
  }
  pthread_mutex_unlock(&io_mu);
  if (!e) { fiber_set_errno(ENOMEM); return -1; }
  return 0;
}

/* io_mu serializes publication/arming with readiness and stop cancellation.
 * Register first: a ready one-shot event must never find an empty queue. */
static int io_waiter_add_and_arm(int fd, int events, uint64_t deadline_ns,
                                 gof_fiber *f) {
  int error = 0;
  pthread_mutex_lock(&io_mu);
  if (gof_bg_stop_requested()) {
    error = ECANCELED;
  } else {
    gof_fdwait *e = fdwait_get(fd, 1);
    if (e && e->cancelled) {
      pthread_mutex_unlock(&io_mu);
      return ECANCELED;
    }
    f->wait_fd = fd;
    f->wait_events = events;
    if (!e || ((events & GOF_POLL_READ) && waiter_push(&e->rd_head, &e->rd_tail, f)) ||
        ((events & GOF_POLL_WRITE) && waiter_push(&e->wr_head, &e->wr_tail, f))) {
      error = ENOMEM;
    } else if (gof_netpoll_arm(fd, fdwait_events(e), deadline_ns) != 0) {
      error = fiber_get_errno();
    }
    if (error) io_waiter_remove_fiber_locked(f);
    else atomic_fetch_add_explicit(&g_io_waiter_count, 1, memory_order_release);
  }
  pthread_mutex_unlock(&io_mu);
  return error;
}

/* Keep registrations until their fibers consume cancellation. Workers must not
 * retire between claiming the wake and enqueueing the cancelled I/O fiber. */
void gof_io_cancel_waiters(void) {
  pthread_mutex_lock(&io_mu);
  for (unsigned i = 0; i < FDWAIT_BUCKETS; ++i) {
    for (gof_fdwait *e = fd_buckets[i]; e; e = e->next) {
      for (gof_waiter *w = e->rd_head; w; w = w->next) gof_sched_make_runnable(w->f);
      for (gof_waiter *w = e->wr_head; w; w = w->next) gof_sched_make_runnable(w->f);
    }
  }
  pthread_mutex_unlock(&io_mu);
}

int gof_socket_errno(void) { return fiber_get_errno(); }

int gof_io_have_waiters(void) {
  /* Lock-free (nostrc-bme1g review): the scheduler idle loop calls this
   * while holding S.mu, and on_ready holds io_mu while waking (which takes
   * S.mu) — taking io_mu here would close an ABBA deadlock. */
  return atomic_load_explicit(&g_io_waiter_count, memory_order_acquire) > 0;
}

static void wake_queue(gof_waiter **head, gof_waiter **tail) {
  gof_fiber *f;
  while ((f = waiter_pop(head, tail)) != NULL) {
    int pidx = gof_sched_current_poller_index();
    if (pidx >= 0) gof_sched_make_runnable_from_poller(f, pidx);
    else gof_sched_make_runnable(f);
  }
}

static void on_ready(int fd, int events) {
  /* Pop AND wake under io_mu: a timeout may otherwise resume/free the fiber
   * before the callback touches it. Lock order remains io_mu -> scheduler. */
  pthread_mutex_lock(&io_mu);
  gof_fdwait *e = fdwait_get(fd, 0);
  if (e) {
    /* An edge belongs to every matching waiter, not just the first reader.
     * Losers of the syscall race register again before rearming. */
    if (events & GOF_POLL_READ) wake_queue(&e->rd_head, &e->rd_tail);
    if (events & GOF_POLL_WRITE) wake_queue(&e->wr_head, &e->wr_tail);
    int remaining = fdwait_events(e);
    if (remaining && gof_netpoll_arm(fd, remaining, 0) != 0) {
      wake_queue(&e->rd_head, &e->rd_tail);
      wake_queue(&e->wr_head, &e->wr_tail);
    }
  }
  pthread_mutex_unlock(&io_mu);
}

static void ensure_ready_callback(void) {
  if (!atomic_load_explicit(&ready_cb_installed, memory_order_acquire)) {
    gof_netpoll_set_ready_callback(on_ready);
    atomic_store_explicit(&ready_cb_installed, 1, memory_order_release);
  }
}

static int wait_event_with_deadline(int fd, int events, uint64_t deadline_ns) {
  gof_fiber *self = gof_sched_current();
  if (!self) { fiber_set_errno(EINVAL); return -1; }
  if (deadline_ns && now_ns() >= deadline_ns) {
    fiber_set_errno(ETIMEDOUT);
    return -1;
  }
  ensure_ready_callback();
  int error = io_waiter_add_and_arm(fd, events, deadline_ns, self);
  if (error) { fiber_set_errno(error); return -1; }
  gof_trace_on_io_armed(fd, events);

  /* Readiness between arm and park claims RUNNABLE/PARKING -> WOKEN;
   * the scheduler then consumes that claim without losing the wake. */
  if (deadline_ns) gof_sched_park_until(deadline_ns);
  else gof_sched_block_current();
  pthread_mutex_lock(&io_mu);
  io_waiter_remove_fiber_locked(self);
  atomic_fetch_sub_explicit(&g_io_waiter_count, 1, memory_order_release);
  gof_fdwait *entry = fdwait_get(fd, 0);
  error = gof_bg_stop_requested() || (entry && entry->cancelled) ? ECANCELED : 0;
  pthread_mutex_unlock(&io_mu);
  if (error == ECANCELED) gof_sched_wake_all();
  gof_trace_on_unblock(fd, events);
  if (!error && deadline_ns && now_ns() >= deadline_ns) error = ETIMEDOUT;
  if (error) { fiber_set_errno(error); return -1; }
  return 0;
}

ssize_t gof_read(gof_socket_t fd, void *buf, size_t n) {
  (void)set_nonblock(fd);
  for(;;) {
    ssize_t r = read(fd, buf, n);
    if (r >= 0) return r;
    /* Like the message wrappers, reacquire errno on this worker after each
     * syscall. A cached TLS address cannot survive the park below. */
    int error = fiber_get_errno();
    if (error == EAGAIN || error == EWOULDBLOCK) {
      /* Park cooperatively and retry. No explicit timeout for read(). */
      if (wait_event_with_deadline(fd, GOF_POLL_READ, 0) != 0) return -1;
      continue;
    }
    fiber_set_errno(error);
    return -1;
  }
}

ssize_t gof_write(gof_socket_t fd, const void *buf, size_t n) {
  (void)set_nonblock(fd);
  const char *p = (const char*)buf;
  size_t left = n;
  while (left > 0) {
    ssize_t w = write(fd, p, left);
    if (w >= 0) { left -= (size_t)w; p += w; continue; }
    int error = fiber_get_errno();
    if (error == EAGAIN || error == EWOULDBLOCK) {
      if (wait_event_with_deadline(fd, GOF_POLL_WRITE, 0) != 0) return -1;
      continue;
    }
    return -1;
  }
  return (ssize_t)n;
}

int gof_connect(gof_socket_t fd, const struct sockaddr *sa, socklen_t slen, int timeout_ms) {
  (void)set_nonblock(fd);
  int r = connect(fd, sa, slen);
  if (r == 0) return 0;
  if (fiber_get_errno() == EINPROGRESS) {
    uint64_t dl = deadline_from_ms(timeout_ms);
    for(;;) {
      if (wait_event_with_deadline(fd, GOF_POLL_WRITE, dl) != 0) return -1;
      /* Check connect completion */
      int err = 0; socklen_t elen = sizeof(err);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&err, &elen) == 0) {
        if (err != 0) { fiber_set_errno(err); return -1; }
        /* A readiness wake is a hint, not a completion. The poller may hand an
         * event it harvested for a previous owner of this fd number to this
         * fresh waiter (gof_netpoll_wait runs the callbacks after kevent or
         * epoll_wait returns), and SO_ERROR is 0 while a connect is still in
         * progress. Only a peer name proves it finished; ENOTCONN means wait
         * again, as Go's netpoll does. Returning early made the caller's first
         * send fail with ENOTCONN on macOS. */
        struct sockaddr_storage peer; socklen_t plen = sizeof(peer);
        if (getpeername(fd, (struct sockaddr*)&peer, &plen) == 0) return 0;
        int peer_errno = fiber_get_errno();
        if (peer_errno != ENOTCONN) { fiber_set_errno(peer_errno); return -1; }
        continue;
      }
      /* If deadline set and passed, timeout */
      if (dl != 0 && now_ns() >= dl) { fiber_set_errno(ETIMEDOUT); return -1; }
      /* Otherwise retry until completion */
    }
  }
  return -1;
}

gof_socket_t gof_accept(gof_socket_t fd, struct sockaddr *sa, socklen_t *slen, int timeout_ms) {
  (void)set_nonblock(fd);
  uint64_t dl = deadline_from_ms(timeout_ms);
  for(;;) {
    int c = accept(fd, sa, slen);
    if (c >= 0) return c;
    /* Value capture: the syscall ran on this thread, so the errno value is
     * correct; a later park may migrate the fiber to another thread. */
    int accept_errno = fiber_get_errno();
    if (accept_errno == EAGAIN || accept_errno == EWOULDBLOCK) {
      if (wait_event_with_deadline(fd, GOF_POLL_READ, dl) != 0) return -1;
      if (dl != 0 && now_ns() >= dl) { fiber_set_errno(ETIMEDOUT); return -1; }
      continue;
    }
    fiber_set_errno(accept_errno);
    return -1;
  }
}

/* A single deadline covers every readiness retry, not each park separately. */
static int wait_message_ready(int fd, int events, uint64_t deadline_ns) {
  if (deadline_ns != 0 && now_ns() >= deadline_ns) {
    fiber_set_errno(ETIMEDOUT);
    return -1;
  }
  if (wait_event_with_deadline(fd, events, deadline_ns) != 0) return -1;
  if (deadline_ns != 0 && now_ns() >= deadline_ns) {
    fiber_set_errno(ETIMEDOUT);
    return -1;
  }
  return 0;
}

ssize_t gof_recvfrom(gof_socket_t fd, void *buf, size_t n, int flags,
                     struct sockaddr *sa, socklen_t *slen, int timeout_ms) {
  if (set_nonblock(fd) != 0) return -1;
  uint64_t dl = deadline_from_ms(timeout_ms);
  for (;;) {
    ssize_t r = recvfrom(fd, buf, n, flags, sa, slen);
    if (r >= 0) return r;
    int error = fiber_get_errno();
    if (error == EAGAIN || error == EWOULDBLOCK) {
      if (wait_message_ready(fd, GOF_POLL_READ, dl) != 0) return -1;
      continue;
    }
    fiber_set_errno(error);
    return -1;
  }
}

ssize_t gof_sendto(gof_socket_t fd, const void *buf, size_t n, int flags,
                   const struct sockaddr *sa, socklen_t slen, int timeout_ms) {
  if (set_nonblock(fd) != 0) return -1;
  uint64_t dl = deadline_from_ms(timeout_ms);
  for (;;) {
    ssize_t r = sendto(fd, buf, n, flags, sa, slen);
    if (r >= 0) return r;
    int error = fiber_get_errno();
    if (error == EAGAIN || error == EWOULDBLOCK) {
      if (wait_message_ready(fd, GOF_POLL_WRITE, dl) != 0) return -1;
      continue;
    }
    fiber_set_errno(error);
    return -1;
  }
}

ssize_t gof_recvmsg(gof_socket_t fd, struct msghdr *msg, int flags, int timeout_ms) {
  if (set_nonblock(fd) != 0) return -1;
  uint64_t dl = deadline_from_ms(timeout_ms);
  for (;;) {
    ssize_t r = recvmsg(fd, msg, flags);
    if (r >= 0) return r;
    int error = fiber_get_errno();
    if (error == EAGAIN || error == EWOULDBLOCK) {
      if (wait_message_ready(fd, GOF_POLL_READ, dl) != 0) return -1;
      continue;
    }
    fiber_set_errno(error);
    return -1;
  }
}

ssize_t gof_sendmsg(gof_socket_t fd, const struct msghdr *msg, int flags, int timeout_ms) {
  if (set_nonblock(fd) != 0) return -1;
  uint64_t dl = deadline_from_ms(timeout_ms);
  for (;;) {
    ssize_t r = sendmsg(fd, msg, flags);
    if (r >= 0) return r;
    int error = fiber_get_errno();
    if (error == EAGAIN || error == EWOULDBLOCK) {
      if (wait_message_ready(fd, GOF_POLL_WRITE, dl) != 0) return -1;
      continue;
    }
    fiber_set_errno(error);
    return -1;
  }
}
#endif /* !_WIN32 */
