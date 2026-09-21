/*
 * Portable unit test for nh_auth_worker_run.
 *
 * Verifies that the worker runtime:
 *   1. Runs a normal job in a child and returns its output.
 *   2. Enforces a hard timeout by SIGKILLing a stuck child and reaping
 *      it without leaving a zombie (assert the call returns TIMEOUT
 *      and completes well before the child's would-be sleep, and that
 *      the in-flight counter drops back to zero).
 *   3. Reports FAILED (with exit_code / term_signal) when the child
 *      crashes (raises SIGABRT / exits non-zero).
 *   4. Wipes the caller's secret input buffer after the call.
 *
 * Portable: uses only anonymous pipes + fork + waitpid + poll -- no
 * SEQPACKET, no filesystem sockets, no OS-specific facilities beyond
 * what auth_worker.c already assumes.
 */
#define _POSIX_C_SOURCE 200809L
#include "auth_worker.h"
#include "../nh_test.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* --- worker fns --- */

/* Echoes input back to out_fd and exits 0. */
static int fn_echo(const uint8_t *input, size_t input_len, int out_fd, void *ud) {
  (void)ud;
  size_t off = 0;
  while (off < input_len) {
    ssize_t n = write(out_fd, input + off, input_len - off);
    if (n < 0) return 1;
    off += (size_t)n;
  }
  return 0;
}

/* Sleeps well past any reasonable test timeout so the parent must
 * SIGKILL us; never writes anything. */
static int fn_sleep_forever(const uint8_t *input, size_t input_len,
                            int out_fd, void *ud) {
  (void)input; (void)input_len; (void)out_fd; (void)ud;
  /* Sleep in a loop so signals don't shortcut us. */
  for (;;) sleep(60);
  return 0;
}

/* Aborts via SIGABRT so the parent sees a signaled child. */
static int fn_crash(const uint8_t *input, size_t input_len,
                    int out_fd, void *ud) {
  (void)input; (void)input_len; (void)out_fd; (void)ud;
  /* Reset the default disposition so abort() actually kills us. */
  signal(SIGABRT, SIG_DFL);
  abort();
  return 1;
}

/* Exits with a specific non-zero exit code without writing output. */
static int fn_exit_code(const uint8_t *input, size_t input_len,
                        int out_fd, void *ud) {
  (void)input; (void)input_len; (void)out_fd; (void)ud;
  return 7;
}

int main(void) {
  /* ---- 1. normal job returns its result ---- */
  {
    uint8_t in[] = "hello worker";
    size_t in_len = sizeof in - 1;
    /* Copy into a heap buffer because worker wipes it. */
    uint8_t *buf = malloc(in_len);
    NH_CHECK(buf);
    memcpy(buf, in, in_len);
    uint8_t *out = NULL;
    size_t out_len = 0;
    nh_auth_worker_result r = {0};
    nh_auth_worker_status st = nh_auth_worker_run(
        fn_echo, NULL, buf, in_len, /*timeout*/ 5000, /*max*/ 4096,
        &out, &out_len, &r);
    NH_CHECK(st == NH_AUTH_WORKER_OK);
    NH_CHECK(r.exit_code == 0);
    NH_CHECK(out && out_len == in_len && memcmp(out, in, in_len) == 0);
    /* Secret input was wiped in the parent after send. */
    int all_zero = 1;
    for (size_t i = 0; i < in_len; i++) if (buf[i] != 0) { all_zero = 0; break; }
    NH_CHECK(all_zero);
    free(out);
    free(buf);
    NH_CHECK(nh_auth_worker_inflight() == 0);
  }

  /* ---- 2. timeout -> SIGKILL + reap, no zombie, bounded wall time ---- */
  {
    uint64_t start = mono_ms();
    uint8_t *buf = NULL; /* no input needed */
    uint8_t *out = NULL;
    size_t out_len = 0;
    nh_auth_worker_result r = {0};
    nh_auth_worker_status st = nh_auth_worker_run(
        fn_sleep_forever, NULL, buf, 0, /*timeout*/ 200, /*max*/ 4096,
        &out, &out_len, &r);
    uint64_t elapsed = mono_ms() - start;
    NH_CHECK(st == NH_AUTH_WORKER_TIMEOUT);
    NH_CHECK(r.term_signal == SIGKILL);
    /* Must not have hung waiting on the "60s" sleep; give a generous 5s
     * upper bound to tolerate a slow CI host. If reaping were missing
     * this would either hang forever or leave a zombie. */
    NH_CHECK(elapsed < 5000);
    NH_CHECK(out == NULL && out_len == 0);
    NH_CHECK(nh_auth_worker_inflight() == 0);
    /* No zombies remain: waitpid(-1, WNOHANG) should return 0 (no more
     * children). If we had a zombie it would return the reaped pid. */
    int status = 0;
    pid_t leftover = waitpid(-1, &status, WNOHANG);
    NH_CHECK(leftover == 0 || (leftover == -1 && errno == ECHILD));
  }

  /* ---- 3a. child crash via SIGABRT -> FAILED with term_signal set ---- */
  {
    uint8_t *out = NULL;
    size_t out_len = 0;
    nh_auth_worker_result r = {0};
    nh_auth_worker_status st = nh_auth_worker_run(
        fn_crash, NULL, NULL, 0, 5000, 4096, &out, &out_len, &r);
    NH_CHECK(st == NH_AUTH_WORKER_FAILED);
    /* Either abort() delivered SIGABRT (term_signal) or, if the runtime
     * saw a normal exit for some reason, exit_code is non-zero. */
    NH_CHECK(r.term_signal == SIGABRT || r.exit_code != 0);
    NH_CHECK(out == NULL);
    NH_CHECK(nh_auth_worker_inflight() == 0);
  }

  /* ---- 3b. non-zero exit code is surfaced ---- */
  {
    uint8_t *out = NULL;
    size_t out_len = 0;
    nh_auth_worker_result r = {0};
    nh_auth_worker_status st = nh_auth_worker_run(
        fn_exit_code, NULL, NULL, 0, 5000, 4096, &out, &out_len, &r);
    NH_CHECK(st == NH_AUTH_WORKER_FAILED);
    NH_CHECK(r.exit_code == 7);
    NH_CHECK(r.term_signal == 0);
    free(out);
    NH_CHECK(nh_auth_worker_inflight() == 0);
  }

  /* ---- 4. bad args ---- */
  {
    nh_auth_worker_result r = {0};
    nh_auth_worker_status st = nh_auth_worker_run(
        NULL, NULL, NULL, 0, 100, 128, NULL, NULL, &r);
    NH_CHECK(st == NH_AUTH_WORKER_INTERNAL);
    NH_CHECK(nh_auth_worker_inflight() == 0);
  }

  fprintf(stderr, "test_auth_worker: OK\n");
  return 0;
}
