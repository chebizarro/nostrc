#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "../include/libgo/fiber.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define IO_TIMEOUT_MS 2000

static uint64_t monotonic_ms(void) {
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int udp_socket(struct sockaddr_in *addr) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  assert(fd >= 0);
  memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;
  addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(fd, (struct sockaddr *)addr, sizeof(*addr)) == 0);
  socklen_t len = sizeof(*addr);
  assert(getsockname(fd, (struct sockaddr *)addr, &len) == 0);
  assert(len == sizeof(*addr));
  return fd;
}

struct udp_peer {
  int fd;
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
  int client = udp_socket(&client_addr);
  int server = udp_socket(&server_addr);
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
  assert(errno == EMSGSIZE);
  assert(gof_recvfrom(server, buf, sizeof(buf), 0, NULL, NULL, 0) == -1);
  assert(errno == ETIMEDOUT);
  close(client);
  close(server);
}

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
  assert(gof_recvmsg(sp[1], &msg, 0, 0) == -1 && errno == ETIMEDOUT);
  close(sp[0]);
  close(sp[1]);
}

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
  int fd = udp_socket(&addr);
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
    int error = errno;
    uint64_t elapsed = monotonic_ms() - start;
    assert(n == -1 && error == ETIMEDOUT);
    assert(elapsed >= 50 && elapsed < IO_TIMEOUT_MS);
    /* In the one-worker CTest variant this cannot run if recv blocks it. */
    assert(atomic_load(&progress.ran));
  }
  close(fd);
  char buf[1];
  struct msghdr msg = {0};
  assert(gof_recvfrom(-1, buf, 1, 0, NULL, NULL, 0) == -1 && errno == EBADF);
  assert(gof_sendto(-1, buf, 1, 0, NULL, 0, 0) == -1 && errno == EBADF);
  assert(gof_recvmsg(-1, &msg, 0, 0) == -1 && errno == EBADF);
  assert(gof_sendmsg(-1, &msg, 0, 0) == -1 && errno == EBADF);
}

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
  assert(errno == ETIMEDOUT);
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
    assert(queued_bytes > 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    struct iovec iov = { .iov_base = (void *)"last", .iov_len = 4 };
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    ssize_t n = use_msg ? gof_sendmsg(sp[0], &msg, 0, 20)
                        : gof_sendto(sp[0], "last", 4, 0, NULL, 0, 20);
    assert(n == -1 && errno == ETIMEDOUT);
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

static void run_tests(void *arg) {
  (void)arg;
  test_udp();
  test_rights();
  test_timeouts();
  test_send_backpressure();
}

int main(void) {
  gof_init(0);
  assert(gof_spawn(run_tests, NULL, 0));
  gof_run();
  puts("gof_test_io_datagram: OK");
  return 0;
}
