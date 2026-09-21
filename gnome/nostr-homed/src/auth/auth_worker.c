/*
 * auth_worker: fork/pipe/timeout/reap runtime for the scrypt vault unlock.
 * See auth_worker.h.
 */
#define _POSIX_C_SOURCE 200809L
#include "auth_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "secure_buf.h"

/* -------- single-slot guard --------------------------------------------
 * Placeholder for the future max-2 concurrent scrypt worker pool. Today
 * only one submit_unlock can be in flight (the broker is accept-one/
 * handle-one), so a single atomic counter is enough. Once the concurrent
 * broker event loop lands, this is where the pool cap will move; the
 * follow-up beads issue tracks that (see nostrc-zcll.2).
 */
static atomic_int g_worker_inflight = 0;
#define NH_AUTH_WORKER_MAX_INFLIGHT 1

int nh_auth_worker_inflight(void) {
  return atomic_load(&g_worker_inflight);
}

/* -------- helpers ------------------------------------------------------ */

static int set_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* Close every fd the child does not need. We cannot rely on close_range()
 * (Linux 5.9+/glibc 2.34+) being portable, and macOS has closefrom() but
 * only on very recent versions. A bounded loop from fd=3 up to the soft
 * RLIMIT_NOFILE (capped) is portable and fast enough.
 */
static void close_unrelated_fds(int keep_a, int keep_b) {
  struct rlimit rl;
  int max_fd = 1024;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < (rlim_t)16384)
    max_fd = (int)rl.rlim_cur;
  for (int fd = 3; fd < max_fd; fd++) {
    if (fd == keep_a || fd == keep_b) continue;
    /* close() with an invalid fd is a cheap EBADF; that's fine */
    (void)close(fd);
  }
}

/* Write all of buf to fd, tolerating short writes and EINTR. Returns 0 on
 * success, -1 on error. */
static int write_all(int fd, const uint8_t *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

/* Read exactly len bytes from fd. Returns 0 on success, -1 on error/EOF. */
static int read_all(int fd, uint8_t *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = read(fd, buf + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

/* Monotonic clock in milliseconds. */
static uint64_t now_ms(void) {
  struct timespec ts;
#if defined(CLOCK_MONOTONIC)
  clock_gettime(CLOCK_MONOTONIC, &ts);
#else
  clock_gettime(CLOCK_REALTIME, &ts);
#endif
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Reap the child, blocking; ignore already-reaped. */
static void reap(pid_t pid, int *status_out) {
  int status = 0;
  for (;;) {
    pid_t r = waitpid(pid, &status, 0);
    if (r == pid) break;
    if (r < 0) {
      if (errno == EINTR) continue;
      break; /* ECHILD: already reaped */
    }
  }
  if (status_out) *status_out = status;
}

/* -------- child side --------------------------------------------------- */

static void child_run(nh_auth_worker_fn fn, void *user_data,
                      int in_fd, int out_fd, size_t input_len) __attribute__((noreturn));

static void child_run(nh_auth_worker_fn fn, void *user_data,
                      int in_fd, int out_fd, size_t input_len) {
  /* Disable core dumps: the child holds the passphrase and derived key. */
  struct rlimit no_core = {0, 0};
  (void)setrlimit(RLIMIT_CORE, &no_core);

  /* Close every inherited fd except our two pipe endpoints. In particular
   * this drops the listening SEQPACKET socket the broker was accept()ing
   * on. */
  close_unrelated_fds(in_fd, out_fd);

  /* Read the input payload. Secret material (passphrase, sealed blob) is
   * held in a mlock()ed secure buffer so it stays out of swap and can be
   * explicitly wiped before exit. */
  nostr_secure_buf in = {0};
  if (input_len > 0) {
    in = secure_alloc(input_len);
    if (!in.ptr) _exit(64);
    if (read_all(in_fd, in.ptr, input_len) != 0) {
      secure_free(&in);
      _exit(65);
    }
  }
  close(in_fd);

  int rc = fn((const uint8_t *)in.ptr, input_len, out_fd, user_data);

  /* Wipe our copy of the secret input. secure_free wipes+munlocks+frees. */
  secure_free(&in);

  /* Close output so the parent's read sees EOF. */
  close(out_fd);
  _exit(rc & 0xff);
}

/* -------- parent side -------------------------------------------------- */

/* Read the child's output up to max bytes; on EOF returns 0 with *out
 * malloc()ed to received data. Returns -1 on error, -2 on overflow. */
static int drain_output(int fd, size_t max_output, uint8_t **out, size_t *out_len) {
  size_t cap = 256;
  if (max_output && cap > max_output) cap = max_output;
  size_t len = 0;
  uint8_t *buf = malloc(cap);
  if (!buf) return -1;
  for (;;) {
    if (len == cap) {
      if (max_output && cap >= max_output) {
        /* Read one more byte to detect overflow */
        uint8_t sink;
        ssize_t n = read(fd, &sink, 1);
        if (n == 0) break;
        if (n < 0 && errno == EINTR) continue;
        /* Any real byte here means the child exceeded the bound. */
        free(buf);
        return -2;
      }
      size_t ncap = cap * 2;
      if (max_output && ncap > max_output) ncap = max_output;
      uint8_t *nb = realloc(buf, ncap);
      if (!nb) { free(buf); return -1; }
      buf = nb; cap = ncap;
    }
    ssize_t n = read(fd, buf + len, cap - len);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      free(buf);
      return -1;
    }
    len += (size_t)n;
  }
  *out = buf;
  *out_len = len;
  return 0;
}

nh_auth_worker_status nh_auth_worker_run(nh_auth_worker_fn fn, void *user_data,
                                         uint8_t *input, size_t input_len,
                                         uint32_t timeout_ms, size_t max_output,
                                         uint8_t **out, size_t *out_len,
                                         nh_auth_worker_result *result_out) {
  nh_auth_worker_result r = {NH_AUTH_WORKER_INTERNAL, 0, 0};
  if (out) *out = NULL;
  if (out_len) *out_len = 0;
  if (!fn) {
    if (result_out) *result_out = r;
    return NH_AUTH_WORKER_INTERNAL;
  }

  /* Single-slot guard. */
  int prev = atomic_fetch_add(&g_worker_inflight, 1);
  if (prev >= NH_AUTH_WORKER_MAX_INFLIGHT) {
    atomic_fetch_sub(&g_worker_inflight, 1);
    r.status = NH_AUTH_WORKER_BUSY;
    if (result_out) *result_out = r;
    return NH_AUTH_WORKER_BUSY;
  }

  int in_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  if (pipe(in_pipe) < 0) goto internal;
  if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); goto internal; }
  /* Parent's ends must not survive across an accidental exec elsewhere. */
  (void)set_cloexec(in_pipe[1]);
  (void)set_cloexec(out_pipe[0]);

  pid_t pid = fork();
  if (pid < 0) {
    close(in_pipe[0]); close(in_pipe[1]);
    close(out_pipe[0]); close(out_pipe[1]);
    goto internal;
  }
  if (pid == 0) {
    /* Child */
    close(in_pipe[1]);
    close(out_pipe[0]);
    child_run(fn, user_data, in_pipe[0], out_pipe[1], input_len);
    /* not reached */
  }
  /* Parent */
  close(in_pipe[0]);
  close(out_pipe[1]);

  /* Write the input payload (secret) to the child. Ignore SIGPIPE briefly
   * so a crashed child yields EPIPE instead of killing us. */
  void (*old_pipe)(int) = signal(SIGPIPE, SIG_IGN);
  int wrote_ok = 1;
  if (input && input_len) {
    if (write_all(in_pipe[1], input, input_len) != 0) wrote_ok = 0;
  }
  /* Wipe the caller's input buffer: it held a secret. */
  if (input && input_len) secure_wipe(input, input_len);
  close(in_pipe[1]);
  /* If the write failed the child likely never got the payload; the
   * subsequent read/poll will EOF or timeout and we surface FAILED. */
  (void)wrote_ok;

  /* Poll the output pipe for readability with a hard deadline. */
  uint64_t start = now_ms();
  int timed_out = 0;
  for (;;) {
    struct pollfd pfd = { .fd = out_pipe[0], .events = POLLIN };
    int wait_ms;
    if (timeout_ms == 0) {
      wait_ms = -1;
    } else {
      uint64_t elapsed = now_ms() - start;
      if (elapsed >= timeout_ms) { timed_out = 1; break; }
      wait_ms = (int)(timeout_ms - elapsed);
    }
    int pr = poll(&pfd, 1, wait_ms);
    if (pr < 0) {
      if (errno == EINTR) continue;
      /* poll failure -> treat as timeout/kill */
      timed_out = 1;
      break;
    }
    if (pr == 0) { timed_out = 1; break; }
    /* Readable or HUP: drain and then waitpid. */
    break;
  }

  if (timed_out) {
    /* Hard kill and reap. */
    kill(pid, SIGKILL);
    int status = 0;
    reap(pid, &status);
    close(out_pipe[0]);
    signal(SIGPIPE, old_pipe);
    atomic_fetch_sub(&g_worker_inflight, 1);
    r.status = NH_AUTH_WORKER_TIMEOUT;
    r.term_signal = SIGKILL;
    if (result_out) *result_out = r;
    return NH_AUTH_WORKER_TIMEOUT;
  }

  /* Drain and reap. */
  uint8_t *buf = NULL;
  size_t buf_len = 0;
  int drc = drain_output(out_pipe[0], max_output, &buf, &buf_len);
  close(out_pipe[0]);

  int status = 0;
  reap(pid, &status);
  signal(SIGPIPE, old_pipe);
  atomic_fetch_sub(&g_worker_inflight, 1);

  if (drc == -1) {
    free(buf);
    r.status = NH_AUTH_WORKER_FAILED;
    if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    if (WIFSIGNALED(status)) r.term_signal = WTERMSIG(status);
    if (result_out) *result_out = r;
    return NH_AUTH_WORKER_FAILED;
  }
  if (drc == -2) {
    /* Output overflowed max_output */
    r.status = NH_AUTH_WORKER_FAILED;
    if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    if (WIFSIGNALED(status)) r.term_signal = WTERMSIG(status);
    if (result_out) *result_out = r;
    return NH_AUTH_WORKER_FAILED;
  }

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    if (out) *out = buf; else free(buf);
    if (out_len) *out_len = buf_len;
    r.status = NH_AUTH_WORKER_OK;
    r.exit_code = 0;
    if (result_out) *result_out = r;
    return NH_AUTH_WORKER_OK;
  }
  /* Non-zero exit or signaled: report FAILED with detail so callers can
   * distinguish (e.g.) a domain-level "denied" exit code from a crash. */
  free(buf);
  r.status = NH_AUTH_WORKER_FAILED;
  if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
  if (WIFSIGNALED(status)) r.term_signal = WTERMSIG(status);
  if (result_out) *result_out = r;
  return NH_AUTH_WORKER_FAILED;

internal:
  atomic_fetch_sub(&g_worker_inflight, 1);
  r.status = NH_AUTH_WORKER_INTERNAL;
  if (result_out) *result_out = r;
  return NH_AUTH_WORKER_INTERNAL;
}
