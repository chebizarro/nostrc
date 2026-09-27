/*
 * test_session_relay_ws — a real NIP-01 client against nostr-session-relayd
 * over its Unix socket.
 *
 * Spawns the built daemon (fallback-bind under a private XDG_RUNTIME_DIR,
 * storage forced off with `storage_driver = "none"`) and speaks RFC 6455 to
 * it the way NIP-01 clients do: no Sec-WebSocket-Protocol header.
 *
 *   - The upgrade must complete promptly even when the client connects
 *     after the daemon has gone idle (nostrc-q9ba part 2: the listen fd was
 *     not in lws's poll set, so late clients were never accepted).
 *   - Frames must reach the NIP-01 handler (nostrc-q9ba part 2: a
 *     subprotocol-less upgrade was bound to the "http" protocol, so REQ and
 *     EVENT were silently dropped).
 *
 * Needs $NOSTR_SESSION_RELAYD; exits 77 (SKIP) when it is unset.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
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

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms) {
  struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static int write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (!f) return -1;
  fputs(text, f);
  return fclose(f);
}

static int write_all(int fd, const void *buf, size_t len) {
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

static int read_full(int fd, void *buf, size_t len) {
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

/* Connect, upgrade, and return the socket; *upgrade_ms gets the latency. */
static int ws_open(const char *sock_path, long long *upgrade_ms) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  struct timeval tv = { 3, 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock_path);
  long long t0 = now_ms();
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
    close(fd);
    return -1;
  }
  static const char req[] =
      "GET / HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "\r\n";
  if (write_all(fd, req, sizeof req - 1) != 0) {
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
  return fd;
}

/* Send one masked text frame. */
static int ws_send(int fd, const char *text) {
  size_t len = strlen(text);
  unsigned char head[8];
  size_t hl = 0;
  head[hl++] = 0x81; /* FIN | text */
  if (len < 126) {
    head[hl++] = (unsigned char)(0x80 | len);
  } else if (len <= 0xFFFF) {
    head[hl++] = 0x80 | 126;
    head[hl++] = (unsigned char)(len >> 8);
    head[hl++] = (unsigned char)len;
  } else {
    return -1;
  }
  static const unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
  unsigned char *frame = malloc(hl + 4 + len);
  if (!frame) return -1;
  memcpy(frame, head, hl);
  memcpy(frame + hl, mask, 4);
  for (size_t i = 0; i < len; i++)
    frame[hl + 4 + i] = (unsigned char)text[i] ^ mask[i % 4];
  int rc = write_all(fd, frame, hl + 4 + len);
  free(frame);
  return rc;
}

/* Receive the next text frame (skipping control frames). Caller frees. */
static char *ws_recv(int fd) {
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
static void expect_reply(int fd, const char *frame, const char *prefix,
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

static pid_t spawn_daemon(const char *bin, const char *xrd, const char *state) {
  pid_t pid = fork();
  if (pid != 0) return pid;
  setenv("XDG_RUNTIME_DIR", xrd, 1);
  setenv("XDG_DATA_HOME", state, 1);
  setenv("XDG_CONFIG_HOME", state, 1);
  setenv("HOME", state, 1);
  /* No session bus: SessionRelay1 is optional and must not be required. */
  setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent", 1);
  unsetenv("LISTEN_FDS");
  unsetenv("LISTEN_FDNAMES");
  unsetenv("LISTEN_PID");
  execl(bin, bin, (char *)NULL);
  _exit(127);
}

int main(void) {
  const char *bin = getenv("NOSTR_SESSION_RELAYD");
  if (!bin || !*bin) {
    fprintf(stderr, "NOSTR_SESSION_RELAYD unset; SKIP\n");
    return 77;
  }
  signal(SIGPIPE, SIG_IGN);

  char xrd[] = "/tmp/nsr-ws-rt-XXXXXX";
  char state[] = "/tmp/nsr-ws-state-XXXXXX";
  if (!mkdtemp(xrd) || !mkdtemp(state)) {
    perror("mkdtemp");
    return 1;
  }
  chmod(xrd, 0700);
  char path[600];
  snprintf(path, sizeof path, "%s/nostr", state);
  mkdir(path, 0700);
  snprintf(path, sizeof path, "%s/nostr/session-relay.conf", state);
  if (write_file(path, "storage_driver = \"none\"\n") != 0) {
    perror("write session-relay.conf");
    return 1;
  }

  pid_t pid = spawn_daemon(bin, xrd, state);
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  char sock_path[600];
  snprintf(sock_path, sizeof sock_path, "%s/nostr/relay.sock", xrd);
  struct stat st;
  int appeared = 0;
  for (int i = 0; i < 800 && !appeared; i++) {
    appeared = stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode);
    if (!appeared) sleep_ms(10);
  }
  CHECK(appeared, "socket never appeared at %s", sock_path);

  if (appeared) {
    /* Let the daemon go idle: the old loop was parked inside lws by now. */
    sleep_ms(300);
    for (int round = 0; round < 2; round++) {
      long long upgrade_ms = -1;
      int fd = ws_open(sock_path, &upgrade_ms);
      CHECK(fd >= 0, "round %d: WebSocket upgrade over relay.sock failed",
            round);
      if (fd < 0) break;
      fprintf(stderr, "round %d: upgrade in %lld ms\n", round, upgrade_ms);
      CHECK(upgrade_ms < 1000, "round %d: upgrade took %lld ms", round,
            upgrade_ms);
      expect_reply(fd, "[\"REQ\",\"s\",{\"search\":\"x\"}]",
                   "[\"CLOSED\",\"s\",\"unsupported: search\"",
                   "search REQ without storage");
      expect_reply(fd, "[\"EVENT\",{\"id\":\"00\"}]", "[\"OK\",",
                   "EVENT is answered");
      close(fd);
      sleep_ms(200);
    }
  }

  kill(pid, SIGTERM);
  int status = 0;
  int reaped = 0;
  for (int i = 0; i < 50 && !reaped; i++) {
    reaped = waitpid(pid, &status, WNOHANG) == pid;
    if (!reaped) sleep_ms(100);
  }
  CHECK(reaped, "daemon did not exit within 5 s of SIGTERM");
  if (!reaped) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  } else {
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "daemon exit status 0x%x", status);
  }
  CHECK(access(sock_path, F_OK) != 0,
        "fallback socket %s not unlinked at shutdown", sock_path);

  if (g_failures) {
    fprintf(stderr, "test_session_relay_ws: %d failure(s)\n", g_failures);
    return 1;
  }
  fprintf(stderr, "test_session_relay_ws: ok\n");
  return 0;
}
