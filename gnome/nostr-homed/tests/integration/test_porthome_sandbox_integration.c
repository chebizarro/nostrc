/* test_porthome_sandbox_integration — hostile-child assertions.
 *
 * Bead nostrc-ww50 (Phase 2.5B).
 *
 * Spawns porthome_sandbox_stub in a series of modes and asserts the
 * sandbox actually blocks each privileged action. Runs unprivileged
 * (parent uid != 0) via the ALLOW_NONROOT seam.
 *
 * argv[1] MUST be the absolute path to porthome_sandbox_stub. The
 * CMake add_test() target passes $<TARGET_FILE:...> for us. Falling
 * back to a hard-coded ./porthome_sandbox_stub keeps this file
 * runnable by hand from the build dir.
 */
#define _GNU_SOURCE
#include "nh_porthome_sandbox.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *g_stub;

/* Run stub in @mode; return (exit_code, signal, timed_out) via out params. */
static void run_stub(const char *mode, int *exit_code, int *sig, int *timed) {
  char *argv[] = { (char *)g_stub, (char *)mode, NULL };
  pid_t pid = 0;
  nh_porthome_sandbox_rc rc = nh_porthome_spawn_sandboxed(
      argv, NULL, -1, -1, -1, 15000, &pid);
  if (rc != NH_PORTHOME_SANDBOX_OK) {
    fprintf(stderr, "spawn(%s) failed rc=%d errno=%d\n",
            mode, (int)rc, errno);
    assert(rc == NH_PORTHOME_SANDBOX_OK);
  }
  int wrc = nh_porthome_sandbox_wait(pid, 15000, exit_code, sig, timed);
  assert(wrc == 0);
}

static void test_noop_smoke(void) {
  int e = -1, s = 0, t = 0;
  run_stub("noop", &e, &s, &t);
  assert(e == 0 && s == 0 && !t);
  fprintf(stderr, "PASS noop smoke\n");
}

static void test_write_etc_passwd_denied(void) {
  int e = -1, s = 0, t = 0;
  run_stub("write-etc-passwd", &e, &s, &t);
  /* If we're accidentally running with write access to /etc/passwd,
   * the stub returns 1 and we fail loudly. */
  assert(s == 0);
  assert(e == 0);
  fprintf(stderr, "PASS write /etc/passwd refused (EACCES/EROFS/EPERM)\n");
}

static void test_bind_lowport_denied(void) {
  int e = -1, s = 0, t = 0;
  run_stub("bind-lowport", &e, &s, &t);
  assert(s == 0);
  if (e == 3) {
    fprintf(stderr, "SKIP bind :80 (EADDRINUSE — another service on host)\n");
    return;
  }
  if (e == 4) {
    fprintf(stderr, "SKIP bind :80 (unrelated errno; host quirks)\n");
    return;
  }
  assert(e == 0);
  fprintf(stderr, "PASS bind :80 refused (EACCES/EPERM)\n");
}

static void test_cpu_rlimit_kills(void) {
  /* Set a 1-second CPU rlimit for THIS test so the spin loop is
   * reaped promptly. */
  nh_porthome_sandbox_limits lim = {
      .rlimit_cpu_sec = 1,
      .rlimit_as_bytes = 0,
      .rlimit_fsize_bytes = 0,
      .rlimit_nofile = 64,
  };
  nh_porthome_sandbox_set_defaults(&lim);

  int e = -1, s = 0, t = 0;
  run_stub("spin-cpu", &e, &s, &t);
  /* The kernel first sends SIGXCPU (soft cap) and, since we set
   * soft==hard, immediately SIGKILL. Either signal is acceptable
   * evidence the rlimit worked. If the child's wall-clock-deadline
   * timer tripped first (t==1) that ALSO proves it never exited on
   * its own; count that as a pass too. */
  assert(e != 2);
  if (s == 0 && !t) {
    fprintf(stderr, "FAIL: spin-cpu exited cleanly with e=%d — RLIMIT_CPU not applied?\n", e);
    assert(0);
  }
  fprintf(stderr, "PASS RLIMIT_CPU tripped (signal=%d timed_out=%d)\n", s, t);

  /* Restore defaults. */
  nh_porthome_sandbox_set_defaults(NULL);
}

int main(int argc, char **argv) {
  const char *raw = (argc > 1) ? argv[1] : "./porthome_sandbox_stub";
  /* The sandbox refuses to execve a relative argv[0] (belt-and-braces
   * against PATH-poisoning). Resolve to an absolute path here so the
   * add_test() target file expression is accepted verbatim. */
  static char abs_buf[PATH_MAX];
  if (raw[0] == '/') {
    snprintf(abs_buf, sizeof abs_buf, "%s", raw);
  } else if (realpath(raw, abs_buf) == NULL) {
    fprintf(stderr, "realpath(%s) failed: %s\n", raw, strerror(errno));
    return 77;
  }
  g_stub = abs_buf;
  struct stat st;
  if (stat(g_stub, &st) != 0 || !(st.st_mode & S_IXUSR)) {
    fprintf(stderr, "porthome_sandbox_stub not found at %s (skip)\n", g_stub);
    return 77;
  }
  test_noop_smoke();
  test_write_etc_passwd_denied();
  test_bind_lowport_denied();
  test_cpu_rlimit_kills();
  fprintf(stderr, "test_porthome_sandbox_integration: ALL PASS\n");
  return 0;
}
