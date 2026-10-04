/*
 * ws_test_client.h -- a deliberately tiny RFC 6455 client for relayd tests.
 *
 * Speaks to a relay over AF_UNIX the way NIP-01 clients do: no
 * Sec-WebSocket-Protocol header, masked text frames out, unmasked text
 * frames in. Header-only (static functions) so each test is one TU.
 */
#ifndef RELAYD_WS_TEST_CLIENT_H
#define RELAYD_WS_TEST_CLIENT_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                        \
  do {                                                          \
    if (!(cond)) {                                              \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
      fprintf(stderr, __VA_ARGS__);                             \
      fputc('\n', stderr);                                      \
      g_failures++;                                             \
    }                                                           \
  } while (0)

static inline long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline void sleep_ms(long ms) {
  struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static inline int write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (!f) return -1;
  fputs(text, f);
  return fclose(f);
}

static inline int write_all(int fd, const void *buf, size_t len) {
  const unsigned char *p = buf;
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static inline int read_full(int fd, void *buf, size_t len) {
  unsigned char *p = buf;
  while (len > 0) {
    ssize_t n = read(fd, p, len);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1; /* EOF, error, or SO_RCVTIMEO expiry */
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

/* AF_UNIX stream socket with close-on-exec (the tests fork the daemon):
 * SOCK_CLOEXEC where it exists, fcntl() on macOS. */
static inline int unix_socket_cloexec(void) {
#ifdef SOCK_CLOEXEC
  return socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
    close(fd);
    return -1;
  }
  return fd;
#endif
}

/* Waits (bounded) until the daemon at @sock_path accepts connections: 0, or
 * -1 after @timeout_ms. The socket file appears at bind(), before listen(),
 * so its existence alone races the daemon's startup: a connect in between is
 * refused (nostrc-8kb5; as in test_session_relay_peercred.c). The probe is
 * closed at once. */
static inline int ws_wait_accepting(const char *sock_path, long timeout_ms) {
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  if (strlen(sock_path) >= sizeof sa.sun_path) return -1;
  strncpy(sa.sun_path, sock_path, sizeof sa.sun_path - 1);
  for (long waited = 0; waited < timeout_ms; waited += 10) {
    int fd = unix_socket_cloexec();
    if (fd < 0) return -1;
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    close(fd);
    if (rc == 0) return 0;
    sleep_ms(10);
  }
  return -1;
}

/* Connect, upgrade, and return the socket; *upgrade_ms gets the latency. */
static inline int ws_open_with_protocol(const char *sock_path,
                                        const char *protocol, long long *upgrade_ms) {
  int fd = unix_socket_cloexec();
  if (fd < 0) return -1;
  struct timeval tv = { 3, 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  size_t plen = strlen(sock_path);
  if (plen >= sizeof sa.sun_path) {
    close(fd);
    return -1;
  }
  memcpy(sa.sun_path, sock_path, plen + 1);
  long long t0 = now_ms();
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
    close(fd);
    return -1;
  }
  char req[512];
  int req_len = snprintf(req, sizeof req,
      "GET / HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "%s\r\n", protocol ? protocol : "");
  if (req_len < 0 || (size_t)req_len >= sizeof req ||
      write_all(fd, req, (size_t)req_len) != 0) {
    close(fd);
    return -1;
  }
  char hdr[2048];
  size_t got = 0;
  while (got < sizeof hdr - 1) {
    ssize_t n = read(fd, hdr + got, 1);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    got += (size_t)n;
    if (got >= 4 && memcmp(hdr + got - 4, "\r\n\r\n", 4) == 0) break;
  }
  hdr[got] = '\0';
  *upgrade_ms = now_ms() - t0;
  if (strncmp(hdr, "HTTP/1.1 101", 12) != 0) {
    fprintf(stderr, "upgrade response (%zu bytes): %.80s\n", got, hdr);
    close(fd);
    return -1;
  }
  CHECK(strstr(hdr, "Sec-WebSocket-Protocol:") == NULL || protocol != NULL,
        "unsolicited subprotocol in upgrade response");
  return fd;
}

static inline int ws_open(const char *sock_path, long long *upgrade_ms) {
  return ws_open_with_protocol(sock_path, NULL, upgrade_ms);
}

/* Encode one masked text frame into a malloc'd buffer; returns its size or
 * 0 on failure. */
static inline size_t ws_encode(const char *text, unsigned char **out) {
  size_t len = strlen(text);
  unsigned char head[4];
  size_t hl = 0;
  head[hl++] = 0x81; /* FIN | text */
  if (len < 126) {
    head[hl++] = (unsigned char)(0x80 | len);
  } else if (len <= 0xFFFF) {
    head[hl++] = 0x80 | 126;
    head[hl++] = (unsigned char)(len >> 8);
    head[hl++] = (unsigned char)len;
  } else {
    return 0;
  }
  static const unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
  unsigned char *frame = malloc(hl + 4 + len);
  if (!frame) return 0;
  memcpy(frame, head, hl);
  memcpy(frame + hl, mask, 4);
  for (size_t i = 0; i < len; i++)
    frame[hl + 4 + i] = (unsigned char)text[i] ^ mask[i % 4];
  *out = frame;
  return hl + 4 + len;
}

/* Send one masked text frame. */
static inline int ws_send(int fd, const char *text) {
  unsigned char *frame = NULL;
  size_t n = ws_encode(text, &frame);
  int rc = n ? write_all(fd, frame, n) : -1;
  free(frame);
  return rc;
}

/* Send two frames in a single write(), so the server reads both before it
 * gets to service writability for the first. */
static inline int ws_send_pair(int fd, const char *a, const char *b) {
  unsigned char *fa = NULL, *fb = NULL;
  size_t na = ws_encode(a, &fa), nb = ws_encode(b, &fb);
  unsigned char *both = (na && nb) ? malloc(na + nb) : NULL;
  int rc = -1;
  if (both) {
    memcpy(both, fa, na);
    memcpy(both + na, fb, nb);
    rc = write_all(fd, both, na + nb);
  }
  free(both);
  free(fa);
  free(fb);
  return rc;
}

/* Receive the next text frame (skipping control frames). Caller frees. */
static inline char *ws_recv(int fd) {
  for (;;) {
    unsigned char h[2];
    if (read_full(fd, h, 2) != 0) return NULL;
    uint64_t len = h[1] & 0x7F;
    if (len == 126) {
      unsigned char e[2];
      if (read_full(fd, e, 2) != 0) return NULL;
      len = ((uint64_t)e[0] << 8) | e[1];
    } else if (len == 127) {
      unsigned char e[8];
      if (read_full(fd, e, 8) != 0) return NULL;
      len = 0;
      for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
    }
    if (len > (1u << 20)) return NULL;
    char *buf = malloc((size_t)len + 1);
    if (!buf) return NULL;
    if (len && read_full(fd, buf, (size_t)len) != 0) {
      free(buf);
      return NULL;
    }
    buf[len] = '\0';
    if ((h[0] & 0x0F) == 0x1) return buf;
    free(buf); /* ping/pong/continuation: not expected here */
  }
}

/* Send `frame`, expect a reply that starts with `prefix`. */
static inline void expect_reply(int fd, const char *frame, const char *prefix,
                         const char *what) {
  long long t0 = now_ms();
  CHECK(ws_send(fd, frame) == 0, "%s: send failed", what);
  char *reply = ws_recv(fd);
  long long dt = now_ms() - t0;
  CHECK(reply != NULL, "%s: no reply within 3 s", what);
  if (reply) {
    CHECK(strncmp(reply, prefix, strlen(prefix)) == 0,
          "%s: got %s, expected prefix %s", what, reply, prefix);
    fprintf(stderr, "  %s: %s (%lld ms)\n", what, reply, dt);
    free(reply);
  }
}

#endif /* RELAYD_WS_TEST_CLIENT_H */
