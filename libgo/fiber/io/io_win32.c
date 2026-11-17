/* Windows fiber I/O: overlapped Winsock operations on libgo's completion port.
 *
 * Submission. Each call registers an op record (OVERLAPPED plus state, on the
 * calling fiber's stack) with its socket's registry entry under io_mu, issues
 * the overlapped call, and parks until the completion packet is dequeued.
 * Every operation first probes the socket's association: Windows recycles a
 * closed SOCKET's value at once, so per-socket state is trusted only while
 * CreateIoCompletionPort reports the handle as already bound; a newly bound
 * handle is a socket libgo has not prepared (fresh or recycled) and has its
 * type and FILE_SKIP_COMPLETION_PORT_ON_SUCCESS mode set up again. With that
 * mode an inline success returns without a packet or a park. A synchronous
 * failure other than WSA_IO_PENDING queues no packet either (probed on
 * Windows 11: OVERLAPPED.Internal stays STATUS_PENDING, so only the return
 * code decides).
 *
 * Completion. A poller thread (netpoll_iocp.c) dequeues the packet and, under
 * io_mu, unlinks the op, marks it done and wakes its fiber through the
 * scheduler's claim protocol; the fiber reads the outcome with
 * WSAGetOverlappedResult (status, bytes, flags).
 *
 * Cancellation. A deadline, gof_io_cancel_fd() and scheduler stop all use
 * CancelIoEx on the pending op. The kernel still delivers exactly one packet,
 * aborted or with the result if the operation won the race, and the fiber does
 * not return before that packet is dequeued, so no OVERLAPPED, WSAMSG, WSABUF
 * or data buffer outlives its call. A receive that completed despite a cancel
 * returns its data (the datagram is consumed); one that was aborted leaves the
 * datagram queued for the next receive. */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include "../include/libgo/fiber.h"
#include "netpoll.h"
#include "../sched/sched.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef WSA_FLAG_NO_HANDLE_INHERIT
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80
#endif

/* errno is per OS thread and a fiber may resume on another worker after a
 * park: capture and publish it only on the thread that runs the access
 * (see io.c). */
__attribute__((noinline)) static int fiber_get_errno(void) { return errno; }
__attribute__((noinline)) static void fiber_set_errno(int e) { errno = e; }

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t deadline_from_ms(int timeout_ms) {
  if (timeout_ms < 0) return 0; /* 0 => no deadline */
  return now_ns() + (uint64_t)timeout_ms * 1000000ull;
}

static int wsa_to_errno(int e) {
  switch (e) {
  case 0: return 0;
  case WSAEWOULDBLOCK: return EAGAIN;
  case WSAEINPROGRESS: return EINPROGRESS;
  case WSAEALREADY: return EALREADY;
  case WSAENOTSOCK: return ENOTSOCK;
  case WSAEDESTADDRREQ: return EDESTADDRREQ;
  case WSAEMSGSIZE: return EMSGSIZE;
  case WSAEPROTOTYPE: return EPROTOTYPE;
  case WSAENOPROTOOPT: return ENOPROTOOPT;
  case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
  case WSAEOPNOTSUPP: return EOPNOTSUPP;
  case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
  case WSAEADDRINUSE: return EADDRINUSE;
  case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
  case WSAENETDOWN: return ENETDOWN;
  case WSAENETUNREACH: case ERROR_NETWORK_UNREACHABLE: return ENETUNREACH;
  case WSAENETRESET: return ENETRESET;
  case WSAECONNABORTED: case ERROR_CONNECTION_ABORTED: return ECONNABORTED;
  case WSAECONNRESET: case ERROR_NETNAME_DELETED: return ECONNRESET;
  case WSAENOBUFS: return ENOBUFS;
  case WSAEISCONN: return EISCONN;
  case WSAENOTCONN: return ENOTCONN;
  case WSAESHUTDOWN: return EPIPE;
  case WSAETIMEDOUT: case ERROR_SEM_TIMEOUT: return ETIMEDOUT;
  case WSAECONNREFUSED: case ERROR_CONNECTION_REFUSED: case ERROR_PORT_UNREACHABLE: return ECONNREFUSED;
  case WSAEHOSTUNREACH: case WSAEHOSTDOWN: case ERROR_HOST_UNREACHABLE: return EHOSTUNREACH;
  case WSAEINVAL: return EINVAL;
  case WSAEFAULT: return EFAULT;
  case WSAEBADF: case WSA_INVALID_HANDLE: return EBADF;
  case WSAEACCES: return EACCES;
  case WSAEINTR: return EINTR;
  case WSAEMFILE: return EMFILE;
  case WSA_OPERATION_ABORTED: return ECANCELED;
  case ERROR_MORE_DATA: return EMSGSIZE;
  case WSA_NOT_ENOUGH_MEMORY: return ENOMEM;
  default: return EIO;
  }
}

/* ---------------- registry: SOCKET -> pending operations ---------------- */
typedef struct win_op win_op;

typedef struct sock_entry {
  SOCKET s;
  int cancelled;       /* gof_io_cancel_fd latch, cleared by reset (as on POSIX) */
  int prepared;        /* type and skip mode below describe the bound socket */
  int skip_on_success; /* inline successes queue no packet */
  int type;            /* SO_TYPE */
  int pending;         /* registered ops not yet consumed (reset -> EBUSY) */
  win_op *ops;         /* issued ops still waiting for their packet */
  struct sock_entry *next;
} sock_entry;

struct win_op {
  OVERLAPPED ov;
  win_op *next;
  sock_entry *e;
  gof_fiber *f;  /* parked fiber; NULL for a caller outside any fiber */
  HANDLE event;  /* non-fiber caller: signalled when the packet is dequeued */
  int queued;    /* on e->ops */
  int done;      /* packet dequeued */
};

#define SOCK_BUCKETS 256
static sock_entry *sock_buckets[SOCK_BUCKETS];
static pthread_mutex_t io_mu = PTHREAD_MUTEX_INITIALIZER;
/* Registered operations, through consumption (gof_io_have_waiters). */
static _Atomic int g_io_waiter_count = 0;
static _Atomic int completion_cb_installed = 0;

static unsigned sock_hash(SOCKET s) {
  uint64_t v = (uint64_t)s >> 2; /* handle values are multiples of 4 */
  v ^= v >> 9;
  return (unsigned)v & (SOCK_BUCKETS - 1);
}

static sock_entry *entry_get(SOCKET s, int create) {
  unsigned h = sock_hash(s);
  for (sock_entry *e = sock_buckets[h]; e; e = e->next)
    if (e->s == s) return e;
  if (!create) return NULL;
  sock_entry *e = (sock_entry *)calloc(1, sizeof(*e));
  if (!e) return NULL;
  e->s = s;
  e->next = sock_buckets[h];
  sock_buckets[h] = e;
  return e;
}

/* Probe the association and (re)learn the socket when libgo has not
 * prepared this handle: a first use, a recycled SOCKET value or a reset.
 * Under io_mu, once per operation. */
static int entry_prepare_locked(sock_entry *e) {
  int newly = 0;
  if (gof_netpoll_iocp_associate((uintptr_t)e->s, &newly) != 0) return fiber_get_errno();
  if (newly || !e->prepared) {
    int type = 0, len = (int)sizeof(type);
    if (getsockopt(e->s, SOL_SOCKET, SO_TYPE, (char *)&type, &len) != 0) {
      e->prepared = 0;
      return wsa_to_errno(WSAGetLastError());
    }
    e->type = type;
    e->skip_on_success = gof_netpoll_iocp_skip_mode((uintptr_t)e->s);
    e->prepared = 1;
  }
  return 0;
}

static void op_unlink_locked(win_op *op) {
  for (win_op **p = &op->e->ops; *p; p = &(*p)->next) {
    if (*p == op) { *p = op->next; break; }
  }
  op->queued = 0;
}

/* Poller thread: one call per dequeued packet. Wake under io_mu so the op
 * (on the fiber's stack) cannot be released before this returns. */
static void on_complete(void *overlapped, unsigned long bytes) {
  (void)bytes; /* the op reads its own status with WSAGetOverlappedResult */
  win_op *op = CONTAINING_RECORD((OVERLAPPED *)overlapped, win_op, ov);
  pthread_mutex_lock(&io_mu);
  if (op->queued) op_unlink_locked(op);
  op->done = 1;
  if (op->f) {
    int pidx = gof_sched_current_poller_index();
    if (pidx >= 0) gof_sched_make_runnable_from_poller(op->f, pidx);
    else gof_sched_make_runnable(op->f);
  } else {
    SetEvent(op->event);
  }
  pthread_mutex_unlock(&io_mu);
}

static int socket_prepare(SOCKET s, int *type) {
  if (s == INVALID_SOCKET) return EBADF;
  pthread_mutex_lock(&io_mu);
  sock_entry *e = entry_get(s, 1);
  int error = e ? entry_prepare_locked(e) : ENOMEM;
  if (!error && type) *type = e->type;
  pthread_mutex_unlock(&io_mu);
  return error;
}

typedef struct op_outcome {
  DWORD bytes;
  DWORD flags;     /* in: receive flags; out: completion flags */
  int wsa_error;   /* 0 on success */
  int error;       /* errno-style failure before the op was issued */
  int timed_out;   /* this call cancelled the op at its deadline */
  int cancelled;   /* cancel_fd latch or scheduler stop observed at the end */
  int not_fiber;   /* the op pended outside a fiber and was cancelled */
} op_outcome;

/* Issue the overlapped call; return 0 or SOCKET_ERROR with WSAGetLastError()
 * set (BOOL-returning extension functions are adapted by their callers). */
typedef int (*op_issue_fn)(void *ctx, SOCKET s, OVERLAPPED *ov, DWORD *bytes, DWORD *flags);

/* probe: 0 when the caller ran socket_prepare() for this very operation. */
static void run_op(SOCKET s, int events, uint64_t deadline_ns, op_issue_fn issue, void *ctx,
                   int probe, op_outcome *out) {
  out->bytes = 0;
  out->wsa_error = 0;
  out->error = 0;
  out->timed_out = 0;
  out->cancelled = 0;
  out->not_fiber = 0;
  if (s == INVALID_SOCKET) { out->error = EBADF; return; }
  if (!atomic_load_explicit(&completion_cb_installed, memory_order_acquire)) {
    gof_netpoll_set_completion_callback(on_complete);
    atomic_store_explicit(&completion_cb_installed, 1, memory_order_release);
  }
  win_op op;
  memset(&op, 0, sizeof(op));
  op.f = gof_sched_current();
  if (!op.f && !(op.event = CreateEventW(NULL, TRUE, FALSE, NULL))) { out->error = ENOMEM; return; }

  /* Register before issuing: a cancel or stop from here on sees the op. */
  pthread_mutex_lock(&io_mu);
  if (gof_bg_stop_requested()) {
    out->error = ECANCELED;
  } else {
    sock_entry *e = entry_get(s, 1);
    if (!e) out->error = ENOMEM;
    else if (e->cancelled) out->error = ECANCELED;
    else if ((out->error = (probe || !e->prepared) ? entry_prepare_locked(e) : 0) == 0) {
      op.e = e;
      e->pending++;
      atomic_fetch_add_explicit(&g_io_waiter_count, 1, memory_order_release);
    }
  }
  pthread_mutex_unlock(&io_mu);
  if (out->error) {
    if (op.event) CloseHandle(op.event);
    return;
  }

  DWORD bytes = 0, flags = out->flags;
  int rc = issue(ctx, s, &op.ov, &bytes, &flags);
  int wsa_error = rc ? WSAGetLastError() : 0;
  int packet = rc ? wsa_error == WSA_IO_PENDING : !op.e->skip_on_success;

  if (packet) {
    pthread_mutex_lock(&io_mu);
    if (!op.done) {
      op.queued = 1;
      op.next = op.e->ops;
      op.e->ops = &op;
      /* A cancel/stop that ran before the op was listed found nothing to
       * abort; honour its latch now. */
      if (op.e->cancelled || gof_bg_stop_requested()) (void)CancelIoEx((HANDLE)s, &op.ov);
      else if (!op.f) (void)CancelIoEx((HANDLE)s, &op.ov); /* cannot park here */
    }
    if (op.f) {
      atomic_store_explicit(&op.f->wait_fd, s, memory_order_relaxed);
      atomic_store_explicit(&op.f->wait_events, events, memory_order_relaxed);
    }
    pthread_mutex_unlock(&io_mu);

    if (op.f) {
      gof_trace_on_io_armed(s, events);
      for (;;) {
        pthread_mutex_lock(&io_mu);
        int done = op.done;
        if (!done && deadline_ns && !out->timed_out && now_ns() >= deadline_ns) {
          (void)CancelIoEx((HANDLE)s, &op.ov);
          out->timed_out = 1;
        }
        pthread_mutex_unlock(&io_mu);
        if (done) break;
        /* The completion claims the fiber through the scheduler protocol,
         * so a packet dequeued between the check and the park is not lost. */
        if (deadline_ns && !out->timed_out) gof_sched_park_until(deadline_ns);
        else gof_sched_block_current();
      }
      atomic_store_explicit(&op.f->wait_fd, GOF_INVALID_SOCKET, memory_order_relaxed);
      atomic_store_explicit(&op.f->wait_events, 0, memory_order_relaxed);
      gof_trace_on_unblock(s, events);
    } else {
      out->not_fiber = 1;
      WaitForSingleObject(op.event, INFINITE);
    }
    wsa_error = WSAGetOverlappedResult(s, &op.ov, &bytes, FALSE, &flags) ? 0 : WSAGetLastError();
  }

  pthread_mutex_lock(&io_mu);
  op.e->pending--;
  atomic_fetch_sub_explicit(&g_io_waiter_count, 1, memory_order_release);
  out->cancelled = op.e->cancelled || gof_bg_stop_requested();
  pthread_mutex_unlock(&io_mu);
  if (op.event) CloseHandle(op.event);
  out->bytes = bytes;
  out->flags = flags;
  out->wsa_error = wsa_error;
}

/* errno for a finished op (0 on success). trunc_ok: a truncated datagram
 * receive is a short success (MSG_TRUNC), as recvmsg/recvfrom report it. */
static int outcome_errno(const op_outcome *o, int trunc_ok) {
  if (o->error) return o->error;
  if (!o->wsa_error) return 0;
  if (trunc_ok && (o->wsa_error == WSAEMSGSIZE || o->wsa_error == ERROR_MORE_DATA)) return 0;
  if (o->wsa_error == WSA_OPERATION_ABORTED) {
    if (o->cancelled) return ECANCELED;
    if (o->timed_out) return ETIMEDOUT;
    if (o->not_fiber) return EINVAL; /* like POSIX: no park outside a fiber */
    return ECANCELED;
  }
  return wsa_to_errno(o->wsa_error);
}

static ssize_t finish(const op_outcome *o, int trunc_ok) {
  int error = outcome_errno(o, trunc_ok);
  if (error == ECANCELED) gof_sched_wake_all(); /* idle workers re-check stop */
  if (error) { fiber_set_errno(error); return -1; }
  return (ssize_t)o->bytes;
}

/* ---------------- extension functions (WSAIoctl) ---------------- */
#define GOF_EXT_LOADER(name, type, guid_init)                                         \
  static type name(SOCKET s) {                                                        \
    static _Atomic(type) cached = NULL;                                               \
    type fn = atomic_load_explicit(&cached, memory_order_acquire);                    \
    if (fn) return fn;                                                                \
    GUID guid = guid_init;                                                            \
    DWORD n = 0;                                                                      \
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &fn,     \
                 sizeof(fn), &n, NULL, NULL) != 0)                                    \
      return NULL;                                                                    \
    atomic_store_explicit(&cached, fn, memory_order_release);                         \
    return fn;                                                                        \
  }
GOF_EXT_LOADER(load_recvmsg, LPFN_WSARECVMSG, WSAID_WSARECVMSG)
GOF_EXT_LOADER(load_connectex, LPFN_CONNECTEX, WSAID_CONNECTEX)
GOF_EXT_LOADER(load_acceptex, LPFN_ACCEPTEX, WSAID_ACCEPTEX)
GOF_EXT_LOADER(load_acceptex_addrs, LPFN_GETACCEPTEXSOCKADDRS, WSAID_GETACCEPTEXSOCKADDRS)

/* ---------------- WSABUF translation ---------------- */
#define GOF_STACK_BUFS 16
typedef struct buf_list {
  WSABUF stack[GOF_STACK_BUFS];
  WSABUF *bufs;
  DWORD count;
} buf_list;

static int bufs_from_iov(buf_list *b, const struct iovec *iov, size_t n) {
  if (n > INT_MAX || (n && !iov)) return EINVAL;
  b->bufs = n <= GOF_STACK_BUFS ? b->stack : (WSABUF *)malloc(n * sizeof(WSABUF));
  if (!b->bufs) return ENOMEM;
  for (size_t i = 0; i < n; ++i) {
    if (iov[i].iov_len > ULONG_MAX) {
      if (b->bufs != b->stack) free(b->bufs);
      return EINVAL;
    }
    b->bufs[i].buf = (CHAR *)iov[i].iov_base;
    b->bufs[i].len = (ULONG)iov[i].iov_len;
  }
  b->count = (DWORD)n;
  return 0;
}

static void bufs_free(buf_list *b) {
  if (b->bufs && b->bufs != b->stack) free(b->bufs);
}

static ULONG clamp_len(size_t n) { return n > ULONG_MAX ? ULONG_MAX : (ULONG)n; }

/* ---------------- issue callbacks ---------------- */
typedef struct io_ctx {
  WSABUF *bufs;
  DWORD count;
  struct sockaddr *addr;   /* recvfrom source / sendto destination */
  INT addrlen;             /* written at completion for recvfrom */
  WSAMSG *msg;
  LPFN_WSARECVMSG recvmsg;
} io_ctx;

static int issue_recv(void *c, SOCKET s, OVERLAPPED *ov, DWORD *bytes, DWORD *flags) {
  io_ctx *x = c;
  return WSARecv(s, x->bufs, x->count, bytes, flags, ov, NULL);
}

static int issue_recvfrom(void *c, SOCKET s, OVERLAPPED *ov, DWORD *bytes, DWORD *flags) {
  io_ctx *x = c;
  return WSARecvFrom(s, x->bufs, x->count, bytes, flags, x->addr, &x->addrlen, ov, NULL);
}

static int issue_recvmsg(void *c, SOCKET s, OVERLAPPED *ov, DWORD *bytes, DWORD *flags) {
  io_ctx *x = c;
  x->msg->dwFlags = *flags;
  return x->recvmsg(s, x->msg, bytes, ov, NULL);
}

static int issue_send(void *c, SOCKET s, OVERLAPPED *ov, DWORD *bytes, DWORD *flags) {
  io_ctx *x = c;
  return WSASend(s, x->bufs, x->count, bytes, *flags, ov, NULL);
}

static int issue_sendto(void *c, SOCKET s, OVERLAPPED *ov, DWORD *bytes, DWORD *flags) {
  io_ctx *x = c;
  return WSASendTo(s, x->bufs, x->count, bytes, *flags, x->addr, x->addrlen, ov, NULL);
}

static int issue_sendmsg(void *c, SOCKET s, OVERLAPPED *ov, DWORD *bytes, DWORD *flags) {
  io_ctx *x = c;
  return WSASendMsg(s, x->msg, *flags, bytes, ov, NULL);
}

/* ---------------- public API ---------------- */
ssize_t gof_read(gof_socket_t fd, void *buf, size_t n) {
  /* A zero-byte WSARecv on a stream would wait for data, unlike read(). */
  if (n == 0 && fd != INVALID_SOCKET) return 0;
  WSABUF b = { clamp_len(n), (CHAR *)buf };
  io_ctx x = { .bufs = &b, .count = 1 };
  op_outcome o = { .flags = 0 };
  run_op(fd, GOF_POLL_READ, 0, issue_recv, &x, 1, &o);
  return finish(&o, 1);
}

ssize_t gof_write(gof_socket_t fd, const void *buf, size_t n) {
  const char *p = (const char *)buf;
  size_t left = n;
  while (left > 0) {
    WSABUF b = { clamp_len(left), (CHAR *)p };
    io_ctx x = { .bufs = &b, .count = 1 };
    op_outcome o = { .flags = 0 };
    run_op(fd, GOF_POLL_WRITE, 0, issue_send, &x, 1, &o);
    ssize_t w = finish(&o, 0);
    if (w < 0) return -1;
    left -= (size_t)w;
    p += w;
  }
  return (ssize_t)n;
}

ssize_t gof_recvfrom(gof_socket_t fd, void *buf, size_t n, int flags,
                     struct sockaddr *sa, socklen_t *slen, int timeout_ms) {
  int type = 0, error = socket_prepare(fd, &type);
  if (error) { fiber_set_errno(error); return -1; }
  if (n == 0 && type == SOCK_STREAM) return 0;
  uint64_t dl = deadline_from_ms(timeout_ms);
  WSABUF b = { clamp_len(n), (CHAR *)buf };
  io_ctx x = { .bufs = &b, .count = 1, .addr = sa, .addrlen = (sa && slen) ? *slen : 0 };
  op_outcome o = { .flags = (DWORD)flags };
  /* Stream sources are the connected peer: plain WSARecv, address untouched. */
  int from = type != SOCK_STREAM && sa && slen;
  run_op(fd, GOF_POLL_READ, dl, from ? issue_recvfrom : issue_recv, &x, 0, &o);
  ssize_t r = finish(&o, type != SOCK_STREAM);
  if (r >= 0 && from) *slen = x.addrlen;
  return r;
}

ssize_t gof_sendto(gof_socket_t fd, const void *buf, size_t n, int flags,
                   const struct sockaddr *sa, socklen_t slen, int timeout_ms) {
  int type = 0, error = socket_prepare(fd, &type);
  if (error) { fiber_set_errno(error); return -1; }
  uint64_t dl = deadline_from_ms(timeout_ms);
  if (n > ULONG_MAX) { fiber_set_errno(EMSGSIZE); return -1; }
  WSABUF b = { (ULONG)n, (CHAR *)buf };
  io_ctx x = { .bufs = &b, .count = 1, .addr = (struct sockaddr *)sa, .addrlen = slen };
  op_outcome o = { .flags = (DWORD)flags };
  run_op(fd, GOF_POLL_WRITE, dl, (sa && type != SOCK_STREAM) ? issue_sendto : issue_send, &x, 0, &o);
  return finish(&o, 0);
}

ssize_t gof_recvmsg(gof_socket_t fd, struct msghdr *msg, int flags, int timeout_ms) {
  int type = 0, error = socket_prepare(fd, &type);
  if (!error && !msg) error = EINVAL;
  if (!error && msg->msg_controllen > ULONG_MAX) error = EINVAL;
  buf_list b = { .bufs = NULL };
  if (!error) error = bufs_from_iov(&b, msg->msg_iov, msg->msg_iovlen);
  if (error) { fiber_set_errno(error); return -1; }
  uint64_t dl = deadline_from_ms(timeout_ms);
  op_outcome o = { .flags = (DWORD)flags };
  ssize_t r;
  if (type == SOCK_STREAM) {
    /* WSARecvMsg is datagram/raw only (WSAEINVAL on a stream socket). */
    io_ctx x = { .bufs = b.bufs, .count = b.count };
    run_op(fd, GOF_POLL_READ, dl, issue_recv, &x, 0, &o);
    r = finish(&o, 0);
    if (r >= 0) { msg->msg_namelen = 0; msg->msg_controllen = 0; msg->msg_flags = (int)o.flags; }
  } else {
    LPFN_WSARECVMSG fn = load_recvmsg(fd);
    if (!fn) { bufs_free(&b); fiber_set_errno(wsa_to_errno(WSAGetLastError())); return -1; }
    WSAMSG wm;
    memset(&wm, 0, sizeof(wm));
    wm.name = (LPSOCKADDR)msg->msg_name;
    wm.namelen = msg->msg_name ? msg->msg_namelen : 0;
    wm.lpBuffers = b.bufs;
    wm.dwBufferCount = b.count;
    wm.Control.buf = (CHAR *)msg->msg_control;
    wm.Control.len = msg->msg_control ? (ULONG)msg->msg_controllen : 0;
    io_ctx x = { .msg = &wm, .recvmsg = fn };
    run_op(fd, GOF_POLL_READ, dl, issue_recvmsg, &x, 0, &o);
    r = finish(&o, 1);
    if (r >= 0) {
      /* The provider updates the WSAMSG lengths and flags at completion. */
      msg->msg_namelen = wm.namelen;
      msg->msg_controllen = wm.Control.len;
      msg->msg_flags = (int)wm.dwFlags;
      if (o.wsa_error == WSAEMSGSIZE || o.wsa_error == ERROR_MORE_DATA) msg->msg_flags |= MSG_TRUNC;
    }
  }
  bufs_free(&b);
  return r;
}

ssize_t gof_sendmsg(gof_socket_t fd, const struct msghdr *msg, int flags, int timeout_ms) {
  int type = 0, error = socket_prepare(fd, &type);
  if (!error && !msg) error = EINVAL;
  if (!error && msg->msg_controllen > ULONG_MAX) error = EINVAL;
  /* Stream sockets carry no ancillary data through WSASend. */
  if (!error && type == SOCK_STREAM && msg->msg_control && msg->msg_controllen) error = EOPNOTSUPP;
  buf_list b = { .bufs = NULL };
  if (!error) error = bufs_from_iov(&b, msg->msg_iov, msg->msg_iovlen);
  if (error) { fiber_set_errno(error); return -1; }
  uint64_t dl = deadline_from_ms(timeout_ms);
  op_outcome o = { .flags = (DWORD)flags };
  ssize_t r;
  if (type == SOCK_STREAM) {
    io_ctx x = { .bufs = b.bufs, .count = b.count };
    run_op(fd, GOF_POLL_WRITE, dl, issue_send, &x, 0, &o);
  } else {
    WSAMSG wm;
    memset(&wm, 0, sizeof(wm));
    wm.name = (LPSOCKADDR)msg->msg_name;
    wm.namelen = msg->msg_name ? msg->msg_namelen : 0;
    wm.lpBuffers = b.bufs;
    wm.dwBufferCount = b.count;
    wm.Control.buf = (CHAR *)msg->msg_control;
    wm.Control.len = msg->msg_control ? (ULONG)msg->msg_controllen : 0;
    io_ctx x = { .msg = &wm };
    run_op(fd, GOF_POLL_WRITE, dl, issue_sendmsg, &x, 0, &o);
  }
  r = finish(&o, 0);
  bufs_free(&b);
  return r;
}

typedef struct connect_ctx {
  LPFN_CONNECTEX fn;
  const struct sockaddr *sa;
  int slen;
} connect_ctx;

static int issue_connect(void *c, SOCKET s, OVERLAPPED *ov, DWORD *bytes, DWORD *flags) {
  connect_ctx *x = c;
  (void)flags;
  return x->fn(s, x->sa, x->slen, NULL, 0, bytes, ov) ? 0 : SOCKET_ERROR;
}

int gof_connect(gof_socket_t fd, const struct sockaddr *sa, socklen_t slen, int timeout_ms) {
  int type = 0, error = socket_prepare(fd, &type);
  if (!error && !sa) error = EFAULT;
  if (error) { fiber_set_errno(error); return -1; }
  if (type != SOCK_STREAM) {
    /* Datagram connect only records the peer; it never waits. */
    if (connect(fd, sa, slen) == 0) return 0;
    fiber_set_errno(wsa_to_errno(WSAGetLastError()));
    return -1;
  }
  /* ConnectEx needs a bound socket. */
  struct sockaddr_storage local;
  int llen = (int)sizeof(local);
  if (getsockname(fd, (struct sockaddr *)&local, &llen) != 0) {
    memset(&local, 0, sizeof(local));
    local.ss_family = sa->sa_family;
    llen = sa->sa_family == AF_INET6 ? (int)sizeof(struct sockaddr_in6) : (int)sizeof(struct sockaddr_in);
    if (bind(fd, (struct sockaddr *)&local, llen) != 0) {
      fiber_set_errno(wsa_to_errno(WSAGetLastError()));
      return -1;
    }
  }
  connect_ctx x = { .fn = load_connectex(fd), .sa = sa, .slen = slen };
  if (!x.fn) { fiber_set_errno(wsa_to_errno(WSAGetLastError())); return -1; }
  op_outcome o = { .flags = 0 };
  run_op(fd, GOF_POLL_WRITE, deadline_from_ms(timeout_ms), issue_connect, &x, 0, &o);
  if (finish(&o, 0) < 0) return -1;
  /* Make getpeername/shutdown and the other socket calls see the connection. */
  if (setsockopt(fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) != 0) {
    fiber_set_errno(wsa_to_errno(WSAGetLastError()));
    return -1;
  }
  return 0;
}

#define GOF_ACCEPT_ADDR_LEN ((DWORD)sizeof(struct sockaddr_storage) + 16)

typedef struct accept_ctx {
  LPFN_ACCEPTEX fn;
  SOCKET accepted;
  char addrs[2 * (sizeof(struct sockaddr_storage) + 16)];
} accept_ctx;

static int issue_accept(void *c, SOCKET s, OVERLAPPED *ov, DWORD *bytes, DWORD *flags) {
  accept_ctx *x = c;
  (void)flags;
  return x->fn(s, x->accepted, x->addrs, 0, GOF_ACCEPT_ADDR_LEN, GOF_ACCEPT_ADDR_LEN, bytes, ov)
             ? 0 : SOCKET_ERROR;
}

gof_socket_t gof_accept(gof_socket_t fd, struct sockaddr *sa, socklen_t *slen, int timeout_ms) {
  int error = socket_prepare(fd, NULL);
  WSAPROTOCOL_INFOW info;
  int ilen = (int)sizeof(info);
  if (!error && getsockopt(fd, SOL_SOCKET, SO_PROTOCOL_INFOW, (char *)&info, &ilen) != 0)
    error = wsa_to_errno(WSAGetLastError());
  accept_ctx x;
  memset(&x, 0, sizeof(x));
  if (!error && !(x.fn = load_acceptex(fd))) error = wsa_to_errno(WSAGetLastError());
  if (error) { fiber_set_errno(error); return GOF_INVALID_SOCKET; }
  x.accepted = WSASocketW(info.iAddressFamily, info.iSocketType, info.iProtocol, NULL, 0,
                          WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (x.accepted == INVALID_SOCKET) {
    fiber_set_errno(wsa_to_errno(WSAGetLastError()));
    return GOF_INVALID_SOCKET;
  }
  op_outcome o = { .flags = 0 };
  run_op(fd, GOF_POLL_READ, deadline_from_ms(timeout_ms), issue_accept, &x, 0, &o);
  error = outcome_errno(&o, 0);
  if (!error && setsockopt(x.accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                           (char *)&fd, (int)sizeof(fd)) != 0)
    error = wsa_to_errno(WSAGetLastError());
  if (error) {
    closesocket(x.accepted);
    if (error == ECANCELED) gof_sched_wake_all();
    fiber_set_errno(error);
    return GOF_INVALID_SOCKET;
  }
  if (sa && slen) {
    LPFN_GETACCEPTEXSOCKADDRS addrs = load_acceptex_addrs(fd);
    struct sockaddr *local = NULL, *remote = NULL;
    INT llen = 0, rlen = 0;
    if (addrs) {
      addrs(x.addrs, 0, GOF_ACCEPT_ADDR_LEN, GOF_ACCEPT_ADDR_LEN, &local, &llen, &remote, &rlen);
    } else {
      rlen = (INT)sizeof(struct sockaddr_storage);
      remote = (struct sockaddr *)x.addrs; /* unreachable in practice */
      if (getpeername(x.accepted, remote, &rlen) != 0) rlen = 0;
    }
    if (remote && rlen > 0) memcpy(sa, remote, (size_t)(rlen < *slen ? rlen : *slen));
    *slen = rlen;
  }
  return x.accepted; /* prepared on its first operation, like any new socket */
}

int gof_io_reset_fd(gof_socket_t fd) {
  if (fd == INVALID_SOCKET) { fiber_set_errno(EINVAL); return -1; }
  pthread_mutex_lock(&io_mu);
  sock_entry *e = entry_get(fd, 1);
  int error = !e ? ENOMEM : e->pending ? EBUSY : 0;
  if (!error) {
    e->cancelled = 0;
    e->prepared = 0; /* relearn the socket on its next operation */
  }
  pthread_mutex_unlock(&io_mu);
  if (error) { fiber_set_errno(error); return -1; }
  return 0;
}

int gof_io_cancel_fd(gof_socket_t fd) {
  if (fd == INVALID_SOCKET) { fiber_set_errno(EINVAL); return -1; }
  pthread_mutex_lock(&io_mu);
  sock_entry *e = entry_get(fd, 1);
  if (e) {
    e->cancelled = 1;
    /* Each user still waits for its aborted (or completed) packet. */
    for (win_op *op = e->ops; op; op = op->next) (void)CancelIoEx((HANDLE)fd, &op->ov);
  }
  pthread_mutex_unlock(&io_mu);
  if (!e) { fiber_set_errno(ENOMEM); return -1; }
  return 0;
}

void gof_io_cancel_waiters(void) {
  pthread_mutex_lock(&io_mu);
  for (unsigned i = 0; i < SOCK_BUCKETS; ++i)
    for (sock_entry *e = sock_buckets[i]; e; e = e->next)
      for (win_op *op = e->ops; op; op = op->next) (void)CancelIoEx((HANDLE)e->s, &op->ov);
  pthread_mutex_unlock(&io_mu);
}

int gof_io_have_waiters(void) {
  /* Lock-free: the scheduler idle loop calls this under S.mu (see io.c). */
  return atomic_load_explicit(&g_io_waiter_count, memory_order_acquire) > 0;
}
#endif /* _WIN32 */
