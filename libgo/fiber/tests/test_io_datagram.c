#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "test_net.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

#define IO_TIMEOUT_MS 2000

static uint64_t monotonic_ms(void) {
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static gof_socket_t udp_socket(struct sockaddr_in *addr) {
  gof_socket_t fd = test_bound(SOCK_DGRAM, addr);
  assert(fd != GOF_INVALID_SOCKET);
  return fd;
}

struct udp_peer {
  gof_socket_t fd;
  struct sockaddr_in remote;
  atomic_int done;
};

static void udp_echo(void *arg) {
  struct udp_peer *peer = arg;
  char buf[16];
  struct sockaddr_in source;
  socklen_t len = sizeof(source);
  ssize_t n = gof_recvfrom(peer->fd, buf, sizeof(buf), 0,
                           (struct sockaddr *)&source, &len, IO_TIMEOUT_MS);
  assert(n == 4 && memcmp(buf, "ping", 4) == 0);
  assert(len == sizeof(source) && source.sin_family == AF_INET);
  assert(source.sin_addr.s_addr == peer->remote.sin_addr.s_addr);
  assert(source.sin_port == peer->remote.sin_port);
  assert(gof_sendto(peer->fd, "pong", 4, 0, (struct sockaddr *)&source,
                    len, IO_TIMEOUT_MS) == 4);
  atomic_store(&peer->done, 1);
}

static void test_udp(void) {
  struct sockaddr_in client_addr, server_addr, source;
  gof_socket_t client = udp_socket(&client_addr);
  gof_socket_t server = udp_socket(&server_addr);
  struct udp_peer peer = { .fd = server, .remote = client_addr };
  atomic_init(&peer.done, 0);
  assert(gof_spawn(udp_echo, &peer, 0));
  assert(gof_sendto(client, "ping", 4, 0, (struct sockaddr *)&server_addr,
                    sizeof(server_addr), IO_TIMEOUT_MS) == 4);
  char buf[16];
  socklen_t len = sizeof(source);
  assert(gof_recvfrom(client, buf, sizeof(buf), 0, (struct sockaddr *)&source,
                      &len, IO_TIMEOUT_MS) == 4);
  assert(memcmp(buf, "pong", 4) == 0);
  assert(len == sizeof(source) && source.sin_family == AF_INET);
  assert(source.sin_addr.s_addr == server_addr.sin_addr.s_addr);
  assert(source.sin_port == server_addr.sin_port);
  while (!atomic_load(&peer.done)) gof_yield();

  /* Zero-length sends are real messages, not stream-style no-ops. */
  assert(gof_sendto(client, "", 0, 0, (struct sockaddr *)&server_addr,
                    sizeof(server_addr), 0) == 0);
  assert(gof_recvfrom(server, buf, sizeof(buf), 0, NULL, NULL, IO_TIMEOUT_MS) == 0);
  /* Oversized UDP sends must fail, never be split into smaller packets. */
  static const char oversized[65536] = {0};
  assert(gof_sendto(client, oversized, sizeof(oversized), 0,
                    (struct sockaddr *)&server_addr, sizeof(server_addr), 0) == -1);
  assert(test_errno() == EMSGSIZE);
  assert(gof_recvfrom(server, buf, sizeof(buf), 0, NULL, NULL, 0) == -1);
  assert(test_errno() == ETIMEDOUT);
  test_close(client);
  test_close(server);
}

#if !defined(_WIN32)
/* SCM_RIGHTS over a Unix datagram pair: POSIX only. */
union control_buf {
  struct cmsghdr align;
  unsigned char bytes[CMSG_SPACE(sizeof(int))];
};

struct rights_peer {
  int socket_fd;
  int passed_fd;
  atomic_int done;
};

static void send_rights(void *arg) {
  struct rights_peer *peer = arg;
  struct iovec iov[2] = {{ .iov_base = (void *)"hello", .iov_len = 5 },
                         { .iov_base = (void *)"world", .iov_len = 5 }};
  union control_buf control = {0};
  struct msghdr msg = {0};
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;
  msg.msg_control = control.bytes;
  msg.msg_controllen = sizeof(control.bytes);
  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  assert(cmsg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &peer->passed_fd, sizeof(int));
  assert(gof_sendmsg(peer->socket_fd, &msg, 0, IO_TIMEOUT_MS) == 10);
  atomic_store(&peer->done, 1);
}

static void test_rights(void) {
  int sp[2], pipefd[2];
  assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) == 0);
  assert(pipe(pipefd) == 0);
  struct rights_peer peer = { .socket_fd = sp[0], .passed_fd = pipefd[0] };
  atomic_init(&peer.done, 0);
  assert(gof_spawn(send_rights, &peer, 0));
  char first[3], second[7];
  struct iovec iov[2] = {{ .iov_base = first, .iov_len = sizeof(first) },
                         { .iov_base = second, .iov_len = sizeof(second) }};
  union control_buf control = {0};
  struct msghdr msg = {0};
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;
  msg.msg_control = control.bytes;
  msg.msg_controllen = sizeof(control.bytes);
  assert(gof_recvmsg(sp[1], &msg, 0, IO_TIMEOUT_MS) == 10);
  assert(memcmp(first, "hel", 3) == 0 && memcmp(second, "loworld", 7) == 0);
  assert(!(msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)));
  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  assert(cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS);
  assert(cmsg->cmsg_len == CMSG_LEN(sizeof(int)));
  assert(CMSG_NXTHDR(&msg, cmsg) == NULL);
  int received = -1;
  memcpy(&received, CMSG_DATA(cmsg), sizeof(received));
  assert(received >= 0 && received != pipefd[0]);
  while (!atomic_load(&peer.done)) gof_yield();
  close(pipefd[0]);
  assert(write(pipefd[1], "!", 1) == 1);
  char byte;
  assert(read(received, &byte, 1) == 1 && byte == '!');
  close(received);
  close(pipefd[1]);

  /* Flags and datagram boundaries survive the wrapper. */
  struct iovec empty_iov = { .iov_base = first, .iov_len = 0 };
  struct msghdr empty = {0};
  empty.msg_iov = &empty_iov;
  empty.msg_iovlen = 1;
  assert(gof_sendmsg(sp[0], &empty, 0, 0) == 0);
  assert(gof_recvmsg(sp[1], &msg, 0, 0) == 0);
  assert(gof_sendto(sp[0], "abcdefghij", 10, 0, NULL, 0, 0) == 10);
  iov[0].iov_len = 2;
  msg.msg_iovlen = 1;
  msg.msg_controllen = 0;
  assert(gof_recvmsg(sp[1], &msg, MSG_PEEK, 0) == 2);
  assert(msg.msg_flags & MSG_TRUNC);
  assert(gof_recvmsg(sp[1], &msg, 0, 0) == 2);
  assert(msg.msg_flags & MSG_TRUNC);
  assert(gof_recvmsg(sp[1], &msg, 0, 0) == -1 && test_errno() == ETIMEDOUT);
  close(sp[0]);
  close(sp[1]);
}
#endif

struct progress {
  atomic_int entered;
  atomic_int ran;
};

static void progress_fiber(void *arg) {
  struct progress *progress = arg;
  while (!atomic_load(&progress->entered)) gof_yield();
  atomic_store(&progress->ran, 1);
}

static void test_timeouts(void) {
  struct sockaddr_in addr;
  gof_socket_t fd = udp_socket(&addr);
  for (int use_msg = 0; use_msg < 2; ++use_msg) {
    struct progress progress;
    atomic_init(&progress.entered, 0);
    atomic_init(&progress.ran, 0);
    assert(gof_spawn(progress_fiber, &progress, 0));
    char buf[8];
    struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    uint64_t start = monotonic_ms();
    atomic_store(&progress.entered, 1);
    ssize_t n = use_msg ? gof_recvmsg(fd, &msg, 0, 50)
                        : gof_recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL, 50);
    int error = test_errno();
    uint64_t elapsed = monotonic_ms() - start;
    assert(n == -1 && error == ETIMEDOUT);
    assert(elapsed >= 50 && elapsed < IO_TIMEOUT_MS);
    /* In the one-worker CTest variant this cannot run if recv blocks it. */
    assert(atomic_load(&progress.ran));
  }
  test_close(fd);
  char buf[1];
  struct msghdr msg = {0};
  assert(gof_recvfrom(GOF_INVALID_SOCKET, buf, 1, 0, NULL, NULL, 0) == -1 && test_errno() == EBADF);
  assert(gof_sendto(GOF_INVALID_SOCKET, buf, 1, 0, NULL, 0, 0) == -1 && test_errno() == EBADF);
  assert(gof_recvmsg(GOF_INVALID_SOCKET, &msg, 0, 0) == -1 && test_errno() == EBADF);
  assert(gof_sendmsg(GOF_INVALID_SOCKET, &msg, 0, 0) == -1 && test_errno() == EBADF);
}

#if !defined(_WIN32)
/* A full send queue retried through readiness: POSIX only (an overlapped
 * send on Windows is queued by the stack rather than refused). */
struct drain_peer {
  int fd;
  size_t queued_bytes;
  atomic_int done;
};

static void drain_queue(void *arg) {
  struct drain_peer *peer = arg;
  char buf[1024];
  size_t left = peer->queued_bytes;
  while (left > 0) {
    size_t want = left < sizeof(buf) ? left : sizeof(buf);
    ssize_t n = gof_recvfrom(peer->fd, buf, want, 0, NULL, NULL, IO_TIMEOUT_MS);
    assert(n > 0);
    for (ssize_t i = 0; i < n; ++i) assert(buf[i] == 'f');
    left -= (size_t)n;
  }
  size_t received = 0;
  while (received < 4) {
    ssize_t n = gof_recvfrom(peer->fd, buf + received, 4 - received, 0,
                             NULL, NULL, IO_TIMEOUT_MS);
    assert(n > 0);
    received += (size_t)n;
  }
  assert(memcmp(buf, "last", 4) == 0);
  assert(gof_recvfrom(peer->fd, buf, sizeof(buf), 0, NULL, NULL, 0) == -1);
  assert(test_errno() == ETIMEDOUT);
  atomic_store(&peer->done, 1);
}

static void test_send_backpressure(void) {
  for (int use_msg = 0; use_msg < 2; ++use_msg) {
    int sp[2];
    /* Darwin returns ENOBUFS for a full Unix datagram queue. A stream pair
     * gives EAGAIN on both platforms and exercises the same send retry path. */
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
    int size = 4096;
    assert(setsockopt(sp[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
    assert(fcntl(sp[0], F_SETFL, O_NONBLOCK) == 0);
    char filler[1024];
    memset(filler, 'f', sizeof(filler));
    size_t queued_bytes = 0;
    for (;;) {
      ssize_t n = send(sp[0], filler, sizeof(filler), 0);
      if (n < 0) break;
      assert(n > 0);
      queued_bytes += (size_t)n;
    }
    assert(queued_bytes > 0 && (test_errno() == EAGAIN || test_errno() == EWOULDBLOCK));
    struct iovec iov = { .iov_base = (void *)"last", .iov_len = 4 };
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    ssize_t n = use_msg ? gof_sendmsg(sp[0], &msg, 0, 20)
                        : gof_sendto(sp[0], "last", 4, 0, NULL, 0, 20);
    assert(n == -1 && test_errno() == ETIMEDOUT);
    struct drain_peer peer = { .fd = sp[1], .queued_bytes = queued_bytes };
    atomic_init(&peer.done, 0);
    assert(gof_spawn(drain_queue, &peer, 0));
    /* Negative timeout exercises the no-deadline path while the peer drains. */
    n = use_msg ? gof_sendmsg(sp[0], &msg, 0, -1)
                : gof_sendto(sp[0], "last", 4, 0, NULL, 0, -1);
    assert(n == 4);
    while (!atomic_load(&peer.done)) gof_yield();
    close(sp[0]);
    close(sp[1]);
  }
}


/* nostrc-e3ps5: gof_wait_writable and gof_io_wake_writers on a full datagram
 * pair. A Darwin datagram writer refused with ENOBUFS waits on a NOTE_LOWAT
 * registration sized to its datagram (the peer's receive space, see
 * fiber.h); elsewhere lowat is ENOTSUP and the plain wait is used. */
struct some_reader {
  int fd;
  int count;
  atomic_int done;
};

static void read_some(void *arg) {
  struct some_reader *r = arg;
  char buf[2048];
  for (int i = 0; i < r->count; ++i)
    assert(gof_recvfrom(r->fd, buf, sizeof(buf), 0, NULL, NULL, IO_TIMEOUT_MS) > 0);
  atomic_store(&r->done, 1);
}

static void kick_writers(void *arg) {
  assert(gof_io_wake_writers(*(int *)arg) == 0);
}

static void test_writable_waits(void) {
  int sp[2];
  assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) == 0);
  int size = 8192;
  assert(setsockopt(sp[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
  assert(setsockopt(sp[1], SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0);
  assert(fcntl(sp[0], F_SETFL, fcntl(sp[0], F_GETFL) | O_NONBLOCK) == 0);
  static char buf[1000];
  int queued = 0;
  while (send(sp[0], buf, sizeof(buf), 0) == (ssize_t)sizeof(buf)) ++queued;
  int full = test_errno();
  assert(full == ENOBUFS || full == EAGAIN);
  assert(gof_wait_writable(sp[0], 0, 0) == -1 && test_errno() == EINVAL);
#if defined(__APPLE__)
  /* What one datagram from an unbound half costs the receiver (sun_noname). */
  const size_t need = sizeof(buf) + sizeof(struct sockaddr);
#else
  assert(gof_wait_writable(sp[0], 1, 20) == -1 && test_errno() == ENOTSUP);
  const size_t need = 0;
#endif
  /* Full: nothing wakes the wait. */
  assert(gof_wait_writable(sp[0], need, 20) == -1 && test_errno() == ETIMEDOUT);
  /* A wake with no writer parked is latched, consumed by exactly one wait,
   * and cleared by gof_io_reset_fd. */
  assert(gof_io_wake_writers(sp[0]) == 0);
  assert(gof_wait_writable(sp[0], need, 20) == 0);
  assert(gof_wait_writable(sp[0], need, 20) == -1 && test_errno() == ETIMEDOUT);
  assert(gof_io_wake_writers(sp[0]) == 0);
  assert(gof_io_reset_fd(sp[0]) == 0);
  assert(gof_wait_writable(sp[0], need, 20) == -1 && test_errno() == ETIMEDOUT);
  /* A parked writer is woken by another fiber (or, if that fiber ran first,
   * by the latch it left). */
  assert(gof_spawn(kick_writers, &sp[0], 0));
  assert(gof_wait_writable(sp[0], need, IO_TIMEOUT_MS) == 0);
  /* On macOS the peer reading one datagram makes room for one: the low-water
   * wait returns and the datagram fits. Linux raises a datagram socket's
   * writability only once its queued memory falls below half its send
   * buffer, so there the peer drains the queue. */
#if defined(__APPLE__)
  (void)queued;
  struct some_reader r = {.fd = sp[1], .count = 1};
#else
  struct some_reader r = {.fd = sp[1], .count = queued};
#endif
  atomic_init(&r.done, 0);
  assert(gof_spawn(read_some, &r, 0));
  assert(gof_wait_writable(sp[0], need, IO_TIMEOUT_MS) == 0);
  while (!atomic_load(&r.done)) gof_yield();
  assert(send(sp[0], buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
  /* Owner cancellation ends the wait. */
  assert(gof_io_cancel_fd(sp[0]) == 0);
  assert(gof_wait_writable(sp[0], need, IO_TIMEOUT_MS) == -1 && test_errno() == ECANCELED);
  assert(gof_io_reset_fd(sp[0]) == 0);
  close(sp[0]);
  close(sp[1]);
}

#endif

#if !defined(_WIN32) && defined(MSG_DONTWAIT)
/* nostrc-bmsti: message calls check O_NONBLOCK once per gof_io_reset_fd
 * registration instead of an F_GETFL per call. A flag cleared behind libgo's
 * back makes the elided fcntl observable. */
static int fd_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  assert(flags >= 0);
  return (flags & O_NONBLOCK) != 0;
}

static void make_blocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  assert(flags >= 0 && fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0);
}

static ssize_t send_byte(int fd) {
  char byte = 'n';
  struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  return gof_sendmsg(fd, &msg, 0, 0);
}

static void recv_byte(int fd) {
  char byte = 0;
  struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  assert(gof_recvmsg(fd, &msg, 0, IO_TIMEOUT_MS) == 1 && byte == 'n');
}

/* An empty receive with a zero timeout returns at once even on a descriptor
 * that is blocking again: a cached slot passes MSG_DONTWAIT (or re-checks). */
static void recv_empty(int fd) {
  char buf[1];
  struct iovec iov = { .iov_base = buf, .iov_len = 1 };
  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  assert(gof_recvmsg(fd, &msg, 0, 0) == -1 && test_errno() == ETIMEDOUT);
  assert(gof_recvfrom(fd, buf, 1, 0, NULL, NULL, 0) == -1 && test_errno() == ETIMEDOUT);
}

/* A fresh socketpair whose first end is fd number `want` (-1: any). */
static void pair_at(int want, int sp[2]) {
  assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) == 0);
  if (want < 0 || sp[0] == want) return;
  if (sp[1] == want) { int t = sp[0]; sp[0] = sp[1]; sp[1] = t; return; }
  assert(dup2(sp[0], want) == want);
  close(sp[0]);
  sp[0] = want;
}

struct nonblock_sender { int fd; atomic_int sent; };

static void *nonblock_sender_thread(void *arg) {
  struct nonblock_sender *s = arg;
  for (int i = 0; i < 4; ++i) {
    /* A plain thread cannot park: a zero timeout turns EAGAIN into ETIMEDOUT
     * (XNU refuses a nonblocking send while another sender holds the socket
     * buffer lock), so retry those. */
    ssize_t n;
    while ((n = send_byte(s->fd)) != 1) {
      assert(n == -1 && test_errno() == ETIMEDOUT);
      sched_yield();
    }
    atomic_fetch_add(&s->sent, 1);
  }
  return NULL;
}

static void test_nonblock_cache(void) {
  /* Never registered (this runs first, on fresh numbers): every call checks. */
  int sp[2];
  pair_at(-1, sp);
  for (int i = 0; i < 2; ++i) {
    make_blocking(sp[0]);
    assert(send_byte(sp[0]) == 1);
    assert(fd_nonblocking(sp[0]));
    recv_byte(sp[1]);
  }
  /* Registered: the first call after reset_fd checks, later ones skip it. */
  assert(gof_io_reset_fd(sp[0]) == 0);
  make_blocking(sp[0]);
  assert(send_byte(sp[0]) == 1);
  assert(fd_nonblocking(sp[0]));
  make_blocking(sp[0]); /* an alias clears it: the cached path still never blocks */
  assert(send_byte(sp[0]) == 1);
  assert(!fd_nonblocking(sp[0]));
  recv_empty(sp[0]);
  assert(!fd_nonblocking(sp[0]));
  recv_byte(sp[1]);
  recv_byte(sp[1]);
  /* Reset before close, then reuse the number: the new descriptor re-checks. */
  int num = sp[0];
  assert(gof_io_reset_fd(num) == 0);
  test_close(sp[0]);
  test_close(sp[1]);
  pair_at(num, sp);
  assert(!fd_nonblocking(num));
  assert(send_byte(num) == 1);
  assert(fd_nonblocking(num));
  recv_byte(sp[1]);
  /* Closed WITHOUT the required reset and reused: a stale verdict elides the
   * fcntl, yet MSG_DONTWAIT keeps every message call nonblocking. */
  test_close(sp[0]);
  test_close(sp[1]);
  pair_at(num, sp);
  assert(!fd_nonblocking(num));
  recv_empty(num);
  assert(send_byte(num) == 1);
  recv_byte(sp[1]);
  /* Plain threads (fipsd's crypto workers) race the first check after reset. */
  int space = 1 << 16; /* room for all 16 datagrams */
  assert(setsockopt(sp[1], SOL_SOCKET, SO_RCVBUF, &space, sizeof space) == 0);
  assert(gof_io_reset_fd(num) == 0);
  make_blocking(num);
  struct nonblock_sender sender = { .fd = num };
  atomic_init(&sender.sent, 0);
  pthread_t threads[4];
  for (int i = 0; i < 4; ++i) assert(pthread_create(&threads[i], NULL, nonblock_sender_thread, &sender) == 0);
  for (int i = 0; i < 4; ++i) assert(pthread_join(threads[i], NULL) == 0);
  assert(atomic_load(&sender.sent) == 16 && fd_nonblocking(num));
  for (int i = 0; i < 16; ++i) recv_byte(sp[1]);
  assert(gof_io_reset_fd(num) == 0);
  test_close(sp[0]);
  test_close(sp[1]);
}
#endif

static void run_tests(void *arg) {
  (void)arg;
#if !defined(_WIN32) && defined(MSG_DONTWAIT)
  test_nonblock_cache();
#endif
  test_udp();
#if !defined(_WIN32)
  test_rights();
#endif
  test_timeouts();
#if !defined(_WIN32)
  test_send_backpressure();
  test_writable_waits();
#endif
}

int main(void) {
  test_net_init();
  gof_init(0);
  assert(gof_spawn(run_tests, NULL, 0));
  gof_run();
  puts("gof_test_io_datagram: OK");
  return 0;
}
