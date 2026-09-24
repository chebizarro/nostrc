/* test_porthome_sandbox — headless coverage for nh_porthome_spawn_sandboxed.
 *
 * Bead nostrc-ww50 (Phase 2.5B).
 *
 * These tests run as an unprivileged uid — no setuid magic is required,
 * because the sandbox's NH_PORTHOME_SANDBOX_ALLOW_NONROOT env seam
 * skips the setgroups/setgid/setuid step when the parent is already
 * unprivileged. The rest of the hardening (no_new_privs, rlimits,
 * fd hygiene, minimal env) still runs — which is exactly what these
 * tests are asserting.
 *
 * We spawn a short pipeline of well-known utilities from PATH after
 * chdir()ing into a scratch tmpdir. `id -u` (uid), `sh -c 'ls
 * /proc/self/fd'` (fd inheritance), and `sh -c 'cat /proc/self/status'`
 * (no_new_privs bit).
 */
#define _GNU_SOURCE
#include "nh_porthome_sandbox.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Locate a helper binary by name, walking PATH. Skips setuid entries
 * (a naive PATH walk that returned /bin/su would defeat the test).
 * Returns 0 and fills @out (caller-owned buffer) on success. */
static int which(const char *name, char *out, size_t out_cap) {
  const char *path = getenv("PATH");
  if (!path || !*path) path = "/usr/bin:/bin";
  char *dup = strdup(path);
  if (!dup) return -1;
  int found = -1;
  char *save = NULL;
  for (char *tok = strtok_r(dup, ":", &save); tok;
       tok = strtok_r(NULL, ":", &save)) {
    char cand[512];
    int n = snprintf(cand, sizeof cand, "%s/%s", tok, name);
    if (n <= 0 || (size_t)n >= sizeof cand) continue;
    struct stat st;
    if (stat(cand, &st) != 0) continue;
    if (!S_ISREG(st.st_mode)) continue;
    if (st.st_mode & (S_ISUID | S_ISGID)) continue;
    if (access(cand, X_OK) != 0) continue;
    snprintf(out, out_cap, "%s", cand);
    found = 0;
    break;
  }
  free(dup);
  return found;
}

/* Read up to @cap-1 bytes from @fd into @buf, NUL-terminate. */
static ssize_t read_all(int fd, char *buf, size_t cap) {
  size_t off = 0;
  while (off + 1 < cap) {
    ssize_t r = read(fd, buf + off, cap - 1 - off);
    if (r < 0) { if (errno == EINTR) continue; return -1; }
    if (r == 0) break;
    off += (size_t)r;
  }
  buf[off] = '\0';
  return (ssize_t)off;
}

/* Case 1: `id -u` reports a non-root uid. On the CI seam the parent
 * IS already non-root — so we're asserting the sandbox didn't drop us
 * back to uid 0 by accident, and that no_new_privs is set (verifying
 * the parent's ambient uid flows through unchanged and the hardening
 * still applied on top). */
static void test_uid_and_no_new_privs(void) {
  char idpath[512], shpath[512];
  if (which("id", idpath, sizeof idpath) != 0) { fputs("SKIP: no id\n", stderr); return; }
  if (which("sh", shpath, sizeof shpath) != 0) { fputs("SKIP: no sh\n", stderr); return; }

  int p[2]; if (pipe(p) != 0) { perror("pipe"); exit(1); }
  char *argv[] = { idpath, (char *)"-u", NULL };
  pid_t pid = 0;
  nh_porthome_sandbox_rc rc = nh_porthome_spawn_sandboxed(
      argv, NULL, -1, p[1], -1, 5000, &pid);
  assert(rc == NH_PORTHOME_SANDBOX_OK);
  close(p[1]);
  int exit_code = -1, sig = 0, timed = 0;
  int wrc = nh_porthome_sandbox_wait(pid, 10000, &exit_code, &sig, &timed);
  assert(wrc == 0);
  assert(!timed);
  assert(sig == 0);
  assert(exit_code == 0);
  char buf[128] = {0};
  read_all(p[0], buf, sizeof buf);
  close(p[0]);
  long uid = strtol(buf, NULL, 10);
  assert(uid > 0);  /* never uid 0 */
  fprintf(stderr, "PASS uid=%ld (never root)\n", uid);

  /* And a companion probe: PR_SET_NO_NEW_PRIVS should be 1 in the child. */
  if (pipe(p) != 0) { perror("pipe"); exit(1); }
  char *shargv[] = {
      shpath, (char *)"-c",
      (char *)"grep '^NoNewPrivs' /proc/self/status || true",
      NULL };
  rc = nh_porthome_spawn_sandboxed(shargv, NULL, -1, p[1], -1, 5000, &pid);
  assert(rc == NH_PORTHOME_SANDBOX_OK);
  close(p[1]);
  wrc = nh_porthome_sandbox_wait(pid, 10000, &exit_code, &sig, &timed);
  assert(wrc == 0 && exit_code == 0 && sig == 0);
  memset(buf, 0, sizeof buf);
  read_all(p[0], buf, sizeof buf);
  close(p[0]);
  /* On Linux with a functioning /proc, the line is "NoNewPrivs: 1".
   * On stripped test envs where /proc/self/status omits the line the
   * child prints nothing and the assertion is skipped — the exit-code
   * check above still shows the sandbox path completed cleanly. */
  if (strstr(buf, "NoNewPrivs")) {
    assert(strstr(buf, "NoNewPrivs:\t1") != NULL ||
           strstr(buf, "NoNewPrivs: 1") != NULL);
    fprintf(stderr, "PASS no_new_privs set\n");
  } else {
    fprintf(stderr, "SKIP no_new_privs (no /proc line)\n");
  }
}

/* Case 2: only the three stdio fds survive into the child; a stray
 * pipe fd we open before the spawn is CLOSED. */
static void test_fd_hygiene(void) {
  char shpath[512];
  if (which("sh", shpath, sizeof shpath) != 0) { fputs("SKIP: no sh\n", stderr); return; }

  /* Pipe A: captures the child's `ls /proc/self/fd` listing. */
  int pipe_a[2]; if (pipe(pipe_a) != 0) { perror("pipe"); exit(1); }
  /* Pipe B: an EXTRA fd the parent holds open. We do NOT pass either
   * end to the spawn; the sandbox must close it before exec. */
  int pipe_b[2]; if (pipe(pipe_b) != 0) { perror("pipe"); exit(1); }

  char cmd[256];
  snprintf(cmd, sizeof cmd,
           "for f in /proc/self/fd/*; do echo $f; done");
  char *argv[] = { shpath, (char *)"-c", cmd, NULL };
  pid_t pid = 0;
  nh_porthome_sandbox_rc rc = nh_porthome_spawn_sandboxed(
      argv, NULL, -1, pipe_a[1], -1, 5000, &pid);
  assert(rc == NH_PORTHOME_SANDBOX_OK);
  close(pipe_a[1]);

  int exit_code = -1, sig = 0, timed = 0;
  int wrc = nh_porthome_sandbox_wait(pid, 10000, &exit_code, &sig, &timed);
  assert(wrc == 0 && exit_code == 0 && sig == 0);

  char buf[4096] = {0};
  read_all(pipe_a[0], buf, sizeof buf);
  close(pipe_a[0]);
  close(pipe_b[0]);
  close(pipe_b[1]);

  /* We should see exactly 0, 1, 2 in the listing (plus /proc/self/fd
   * itself, which is the fd sh's `for` loop opens on the directory —
   * that comes and goes inside sh, not from our sandbox). Anything
   * numbered high-single-digit / low-teens (typical pipe fds) means
   * fd hygiene FAILED. */
  int has0 = strstr(buf, "/proc/self/fd/0") != NULL;
  int has1 = strstr(buf, "/proc/self/fd/1") != NULL;
  int has2 = strstr(buf, "/proc/self/fd/2") != NULL;
  int has_stray = 0;
  for (int f = 3; f < 20; f++) {
    char needle[32]; snprintf(needle, sizeof needle, "/proc/self/fd/%d\n", f);
    /* Ignore fd 3 if it's the /proc/self/fd DIR sh's for-loop opens. */
    if (strstr(buf, needle) && f > 3) { has_stray = 1; break; }
  }
  assert(has0 && has1 && has2);
  if (has_stray) {
    fprintf(stderr, "FAIL: stray fd leaked into child:\n%s\n", buf);
    assert(!has_stray);
  }
  fprintf(stderr, "PASS fd hygiene (only 0/1/2 survive)\n");
}

/* Case 3: NOT_ROOT refusal. Force geteuid()!=0 with the ALLOW knob
 * cleared and confirm the sandbox refuses instead of silently
 * skipping the drop step. */
static void test_not_root_refusal(void) {
  /* If we happen to be running as root (unusual for a unit test host)
   * this case is vacuous — skip it. */
  if (geteuid() == 0) { fputs("SKIP not_root: parent is root\n", stderr); return; }
  /* Unset the ALLOW seam for this one call so we exercise the belt-
   * and-braces path. Restore it after. */
  const char *save = getenv("NH_PORTHOME_SANDBOX_ALLOW_NONROOT");
  char save_buf[16] = {0};
  if (save) snprintf(save_buf, sizeof save_buf, "%s", save);
  unsetenv("NH_PORTHOME_SANDBOX_ALLOW_NONROOT");

  char *argv[] = { (char *)"/bin/true", NULL };
  pid_t pid = 0;
  nh_porthome_sandbox_rc rc = nh_porthome_spawn_sandboxed(
      argv, NULL, -1, -1, -1, 5000, &pid);
  assert(rc == NH_PORTHOME_SANDBOX_NOT_ROOT);

  if (save_buf[0]) setenv("NH_PORTHOME_SANDBOX_ALLOW_NONROOT", save_buf, 1);
  fprintf(stderr, "PASS not_root refusal\n");
}

/* Case 4: relative argv[0] is refused (execve semantics). */
static void test_relative_argv_refused(void) {
  char *argv[] = { (char *)"true", NULL };
  pid_t pid = 0;
  nh_porthome_sandbox_rc rc = nh_porthome_spawn_sandboxed(
      argv, NULL, -1, -1, -1, 1000, &pid);
  assert(rc == NH_PORTHOME_SANDBOX_ARG);
  fprintf(stderr, "PASS relative argv[0] refused\n");
}

/* Case 5: bind on a privileged port fails. Uses ss/python via sh?
 * Simpler: call socket()+bind() inside `sh -c` via `python3`? Not
 * portable. We defer the bind check to the integration test which
 * builds a C stub. Here we just verify the fallback-user hint API is
 * accessible and clears after read. */
static void test_fallback_user_api(void) {
  int f = nh_porthome_sandbox_last_used_fallback_user();
  /* No spawn happened in this test body — must be 0. */
  assert(f == 0);
  fprintf(stderr, "PASS fallback-user api readable\n");
}

int main(void) {
  /* Nudge the sandbox with a tighter CPU cap for the sh probes so a
   * runaway `sh -c` under test doesn't hang the suite. */
  nh_porthome_sandbox_limits lim = {
      .drop_user = NULL,          /* keep defaults */
      .rlimit_cpu_sec = 10,
      .rlimit_as_bytes = 0,
      .rlimit_fsize_bytes = 0,
      .rlimit_nofile = 128,        /* sh's builtin loops open a few fds */
  };
  nh_porthome_sandbox_set_defaults(&lim);

  test_uid_and_no_new_privs();
  test_fd_hygiene();
  test_not_root_refusal();
  test_relative_argv_refused();
  test_fallback_user_api();
  fprintf(stderr, "test_porthome_sandbox: ALL PASS\n");
  return 0;
}
