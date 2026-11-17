/* Socket helpers shared by the fiber I/O tests. POSIX keeps its descriptors,
 * pipes and Unix socket pairs; Windows has neither pipe() nor socketpair()
 * for sockets, so it builds connected loopback pairs (TCP for SOCK_STREAM,
 * connected UDP for SOCK_DGRAM). */
#ifndef LIBGO_FIBER_TESTS_TEST_NET_H
#define LIBGO_FIBER_TESTS_TEST_NET_H
#include "libgo/fiber.h"
#include <string.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>

static inline void test_net_init(void) {
  WSADATA wsa;
  (void)WSAStartup(MAKEWORD(2, 2), &wsa); /* reference counted */
}
static inline int test_close(gof_socket_t s) { return closesocket(s); }
static inline ssize_t test_raw_write(gof_socket_t s, const void *buf, size_t n) {
  return send(s, (const char *)buf, (int)n, 0);
}

static inline gof_socket_t test_bound(int type, struct sockaddr_in *addr) {
  gof_socket_t s = socket(AF_INET, type, 0);
  if (s == INVALID_SOCKET) return s;
  memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;
  addr->sin_addr.s_addr = htonl(0x7f000001) /* loopback */;
  int len = (int)sizeof(*addr);
  if (bind(s, (struct sockaddr *)addr, len) != 0 ||
      getsockname(s, (struct sockaddr *)addr, &len) != 0) {
    closesocket(s);
    return INVALID_SOCKET;
  }
  return s;
}

/* sv[0] and sv[1] are connected to each other. */
static inline int test_socketpair(int type, gof_socket_t sv[2]) {
  test_net_init();
  struct sockaddr_in a, b;
  if (type == SOCK_DGRAM) {
    sv[0] = test_bound(SOCK_DGRAM, &a);
    sv[1] = test_bound(SOCK_DGRAM, &b);
    if (sv[0] == INVALID_SOCKET || sv[1] == INVALID_SOCKET ||
        connect(sv[0], (struct sockaddr *)&b, sizeof(b)) != 0 ||
        connect(sv[1], (struct sockaddr *)&a, sizeof(a)) != 0)
      return -1;
    return 0;
  }
  gof_socket_t l = test_bound(SOCK_STREAM, &a);
  if (l == INVALID_SOCKET || listen(l, 1) != 0) return -1;
  sv[0] = socket(AF_INET, SOCK_STREAM, 0);
  if (sv[0] == INVALID_SOCKET || connect(sv[0], (struct sockaddr *)&a, sizeof(a)) != 0) return -1;
  sv[1] = accept(l, NULL, NULL);
  closesocket(l);
  return sv[1] == INVALID_SOCKET ? -1 : 0;
}
/* A byte stream standing in for pipe(): p[0] reads, p[1] writes. */
static inline int test_pipe(gof_socket_t p[2]) { return test_socketpair(SOCK_STREAM, p); }
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static inline void test_net_init(void) {}
static inline int test_close(int s) { return close(s); }
static inline ssize_t test_raw_write(int s, const void *buf, size_t n) { return write(s, buf, n); }
static inline int test_bound(int type, struct sockaddr_in *addr) {
  int s = socket(AF_INET, type, 0);
  if (s < 0) return s;
  memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;
  addr->sin_addr.s_addr = htonl(0x7f000001) /* loopback */;
  socklen_t len = sizeof(*addr);
  if (bind(s, (struct sockaddr *)addr, len) != 0 ||
      getsockname(s, (struct sockaddr *)addr, &len) != 0) {
    close(s);
    return -1;
  }
  return s;
}
static inline int test_socketpair(int type, int sv[2]) { return socketpair(AF_UNIX, type, 0, sv); }
static inline int test_pipe(int p[2]) { return pipe(p); }
#endif
#endif /* LIBGO_FIBER_TESTS_TEST_NET_H */
