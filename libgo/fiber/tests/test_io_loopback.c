/* Real loopback sockets through the fiber I/O wrappers, on every platform
 * (epoll, kqueue and the Windows completion port): a UDP message exchange
 * between fibers, datagram truncation, a receive deadline, a TCP
 * accept/connect/stream/EOF round trip, owner-directed cancellation of a
 * parked receive, and the cancel-versus-arrival race, which must neither lose
 * nor duplicate a datagram. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "test_net.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <arpa/inet.h>
#endif

#define IO_TIMEOUT_MS 5000
#define RACE_ROUNDS 300

static _Atomic int g_blocks;

/* Committed I/O parks (the scheduler reports them after the switch). */
void gof_trace_on_block(gof_socket_t fd, int events) {
  (void)fd; (void)events;
  atomic_fetch_add(&g_blocks, 1);
}

static uint64_t monotonic_ms(void) {
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void wait_blocked(int before) {
  uint64_t start = monotonic_ms();
  while (atomic_load(&g_blocks) <= before) {
    assert(monotonic_ms() - start < IO_TIMEOUT_MS);
    gof_sleep_ms(1);
  }
}

/* ---- UDP message exchange ---- */
struct udp_rx {
  gof_socket_t fd;
  struct sockaddr_in expect_from;
  _Atomic int done;
};

static void udp_receiver(void *arg) {
  struct udp_rx *rx = arg;
  char a[4], b[16];
  struct iovec iov[2] = {{ .iov_base = a, .iov_len = sizeof(a) },
                         { .iov_base = b, .iov_len = sizeof(b) }};
  struct sockaddr_in from;
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_name = &from;
  msg.msg_namelen = sizeof(from);
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;
  ssize_t n = gof_recvmsg(rx->fd, &msg, 0, -1); /* parks: nothing sent yet */
  assert(n == 10);
  assert(memcmp(a, "hell", 4) == 0 && memcmp(b, "oworld", 6) == 0);
  assert(!(msg.msg_flags & MSG_TRUNC));
  assert(msg.msg_namelen == sizeof(from) && from.sin_family == AF_INET);
  assert(from.sin_port == rx->expect_from.sin_port);
  assert(from.sin_addr.s_addr == rx->expect_from.sin_addr.s_addr);
  atomic_store(&rx->done, 1);
}

static void test_udp(void) {
  struct sockaddr_in ca, sa;
  gof_socket_t client = test_bound(SOCK_DGRAM, &ca);
  gof_socket_t server = test_bound(SOCK_DGRAM, &sa);
  assert(client != GOF_INVALID_SOCKET && server != GOF_INVALID_SOCKET);
  struct udp_rx rx = { .fd = server, .expect_from = ca };
  int before = atomic_load(&g_blocks);
  assert(gof_spawn(udp_receiver, &rx, 0));
  wait_blocked(before);
  struct iovec iov[2] = {{ .iov_base = (void *)"hello", .iov_len = 5 },
                         { .iov_base = (void *)"world", .iov_len = 5 }};
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_name = &sa;
  msg.msg_namelen = sizeof(sa);
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;
  assert(gof_sendmsg(client, &msg, 0, IO_TIMEOUT_MS) == 10);
  while (!atomic_load(&rx.done)) gof_yield();

  /* Truncation: the short length plus MSG_TRUNC, the rest discarded. */
  char big[100];
  memset(big, 'x', sizeof(big));
  assert(gof_sendto(client, big, sizeof(big), 0, (struct sockaddr *)&sa, sizeof(sa), 0) == 100);
  char small[10];
  struct iovec siov = { .iov_base = small, .iov_len = sizeof(small) };
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &siov;
  msg.msg_iovlen = 1;
  assert(gof_recvmsg(server, &msg, 0, IO_TIMEOUT_MS) == 10);
  assert(msg.msg_flags & MSG_TRUNC);
  assert(gof_sendto(client, big, sizeof(big), 0, (struct sockaddr *)&sa, sizeof(sa), 0) == 100);
  assert(gof_recvfrom(server, small, sizeof(small), 0, NULL, NULL, IO_TIMEOUT_MS) == 10);

  /* A deadline with nothing to receive. */
  uint64_t start = monotonic_ms();
  assert(gof_recvfrom(server, small, sizeof(small), 0, NULL, NULL, 30) == -1);
  assert(errno == ETIMEDOUT);
  assert(monotonic_ms() - start >= 30);
  /* The aborted receive consumed nothing: a later datagram still arrives. */
  assert(gof_sendto(client, "late", 4, 0, (struct sockaddr *)&sa, sizeof(sa), 0) == 4);
  assert(gof_recvfrom(server, small, sizeof(small), 0, NULL, NULL, IO_TIMEOUT_MS) == 4);
  assert(memcmp(small, "late", 4) == 0);
  test_close(client);
  test_close(server);
  puts("udp: message exchange, truncation, deadline: OK");
}

/* ---- TCP accept/connect/stream ---- */
#define TCP_BYTES (256 * 1024)
struct tcp_srv {
  gof_socket_t listener;
  _Atomic int done;
};

static void tcp_server(void *arg) {
  struct tcp_srv *srv = arg;
  struct sockaddr_storage peer;
  socklen_t plen = sizeof(peer);
  gof_socket_t c = gof_accept(srv->listener, (struct sockaddr *)&peer, &plen, IO_TIMEOUT_MS);
  assert(c != GOF_INVALID_SOCKET);
  assert(plen == sizeof(struct sockaddr_in) && peer.ss_family == AF_INET);
  assert(((struct sockaddr_in *)&peer)->sin_addr.s_addr == htonl(0x7f000001) /* loopback */);
  /* Two iovecs through the stream path of recvmsg. */
  char a[2], b[2];
  struct iovec iov[2] = {{ .iov_base = a, .iov_len = 2 }, { .iov_base = b, .iov_len = 2 }};
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;
  ssize_t n = gof_recvmsg(c, &msg, 0, IO_TIMEOUT_MS);
  assert(n > 0 && n <= 4);
  if (n == 4) {
    assert(memcmp(a, "ab", 2) == 0 && memcmp(b, "cd", 2) == 0);
  } else { /* a short read of the prefix: drain the rest of it */
    char rest[4];
    for (size_t got = (size_t)n; got < 4;) {
      ssize_t k = gof_read(c, rest, 4 - got);
      assert(k > 0);
      got += (size_t)k;
    }
  }
  static unsigned char buf[16384];
  size_t total = 0;
  for (;;) {
    ssize_t n = gof_read(c, buf, sizeof(buf));
    assert(n >= 0);
    if (n == 0) break; /* peer close */
    for (ssize_t i = 0; i < n; ++i) assert(buf[i] == (unsigned char)((total + (size_t)i) * 7));
    total += (size_t)n;
  }
  assert(total == TCP_BYTES);
  test_close(c);
  atomic_store(&srv->done, 1);
}

static void test_tcp(void) {
  struct sockaddr_in la;
  gof_socket_t l = test_bound(SOCK_STREAM, &la);
  assert(l != GOF_INVALID_SOCKET && listen(l, 4) == 0);
  struct tcp_srv srv = { .listener = l };
  assert(gof_spawn(tcp_server, &srv, 0));
  gof_socket_t c = socket(AF_INET, SOCK_STREAM, 0);
  assert(c != GOF_INVALID_SOCKET);
  assert(gof_connect(c, (struct sockaddr *)&la, sizeof(la), IO_TIMEOUT_MS) == 0);
  struct iovec iov[2] = {{ .iov_base = (void *)"ab", .iov_len = 2 },
                         { .iov_base = (void *)"cd", .iov_len = 2 }};
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;
  assert(gof_sendmsg(c, &msg, 0, -1) == 4);
  static unsigned char out[TCP_BYTES];
  for (size_t i = 0; i < sizeof(out); ++i) out[i] = (unsigned char)(i * 7);
  assert(gof_write(c, out, sizeof(out)) == (ssize_t)sizeof(out));
  test_close(c);
  while (!atomic_load(&srv.done)) gof_yield();
  test_close(l);
  puts("tcp: accept, connect, sendmsg/recvmsg, 256 KiB stream, EOF: OK");
}

/* ---- owner-directed cancellation ---- */
struct cancel_rx {
  gof_socket_t fd;
  int result;
  int error;
  char byte;
  _Atomic int done;
};

static void cancel_receiver(void *arg) {
  struct cancel_rx *rx = arg;
  ssize_t n = gof_recvfrom(rx->fd, &rx->byte, 1, 0, NULL, NULL, -1);
  rx->error = errno;
  rx->result = (int)n;
  atomic_store(&rx->done, 1);
}

static void test_cancel_mid_recv(void) {
  struct sockaddr_in ca, sa;
  gof_socket_t client = test_bound(SOCK_DGRAM, &ca);
  gof_socket_t server = test_bound(SOCK_DGRAM, &sa);
  assert(gof_io_reset_fd(server) == 0);
  struct cancel_rx rx = { .fd = server };
  int before = atomic_load(&g_blocks);
  assert(gof_spawn(cancel_receiver, &rx, 0));
  wait_blocked(before);
  /* A live user: the owner may not reset yet. */
  assert(gof_io_reset_fd(server) == -1 && errno == EBUSY);
  assert(gof_io_cancel_fd(server) == 0);
  while (!atomic_load(&rx.done)) gof_yield();
  assert(rx.result == -1 && rx.error == ECANCELED);
  /* The latch holds for later receives until the owner resets. */
  char byte;
  assert(gof_recvfrom(server, &byte, 1, 0, NULL, NULL, IO_TIMEOUT_MS) == -1 && errno == ECANCELED);
  assert(gof_io_reset_fd(server) == 0);
  assert(gof_sendto(client, "!", 1, 0, (struct sockaddr *)&sa, sizeof(sa), 0) == 1);
  assert(gof_recvfrom(server, &byte, 1, 0, NULL, NULL, IO_TIMEOUT_MS) == 1 && byte == '!');

  /* Cancel racing an arriving datagram: the receive either returns it or is
   * cancelled with the datagram still queued, never both or neither. */
  int delivered = 0, cancelled = 0;
  for (int i = 0; i < RACE_ROUNDS; ++i) {
    struct cancel_rx r = { .fd = server };
    assert(gof_spawn(cancel_receiver, &r, 0));
    for (int y = 0; y < (i % 3); ++y) gof_yield();
    char tag = (char)('A' + i % 26);
    if (i & 1) {
      assert(gof_sendto(client, &tag, 1, 0, (struct sockaddr *)&sa, sizeof(sa), 0) == 1);
      assert(gof_io_cancel_fd(server) == 0);
    } else {
      assert(gof_io_cancel_fd(server) == 0);
      assert(gof_sendto(client, &tag, 1, 0, (struct sockaddr *)&sa, sizeof(sa), 0) == 1);
    }
    while (!atomic_load(&r.done)) gof_yield();
    assert(gof_io_reset_fd(server) == 0);
    if (r.result == 1) {
      assert(r.byte == tag);
      ++delivered;
    } else {
      assert(r.result == -1 && r.error == ECANCELED);
      ++cancelled;
      assert(gof_recvfrom(server, &byte, 1, 0, NULL, NULL, IO_TIMEOUT_MS) == 1 && byte == tag);
    }
    /* Nothing extra was left behind. */
    assert(gof_recvfrom(server, &byte, 1, 0, NULL, NULL, 0) == -1 && errno == ETIMEDOUT);
  }
  test_close(client);
  test_close(server);
  printf("cancel: parked receive cancelled, latch/reset, %d race rounds "
         "(%d delivered, %d cancelled with the datagram kept): OK\n",
         RACE_ROUNDS, delivered, cancelled);
}

static void run_tests(void *arg) {
  (void)arg;
  test_udp();
  test_tcp();
  test_cancel_mid_recv();
}

int main(void) {
  test_net_init();
  gof_init(0);
  assert(gof_spawn(run_tests, NULL, 0));
  gof_run();
  puts("gof_test_io_loopback: OK");
  return 0;
}
