/* nh_porthome_sandbox.c — see nh_porthome_sandbox.h.
 *
 * SPDX-License-Identifier: MIT
 * Bead nostrc-ww50, nostrc-h10m (Phase 2.5B).
 *
 * The security posture is by construction, not by policy: this file
 * refuses to run if the parent isn't root (so a mistaken direct call
 * from a test binary doesn't silently drop into a no-op path where
 * setuid was skipped), and every hardening step below has a matching
 * error path — the child _exit()s immediately if it can't reach the
 * fully hardened state. The sibling profile-image sandbox uses the
 * same approach (src/profile/profile_image.c).
 */
#define _GNU_SOURCE
#include "nh_porthome_sandbox.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ─── Defaults (overridable at runtime) ─────────────────────────────── */
#define NH_SANDBOX_DEFAULT_USER      "nostr-home-fetch"
#define NH_SANDBOX_FALLBACK_USER     "nobody"
#define NH_SANDBOX_DEFAULT_CPU_SEC   30u
#define NH_SANDBOX_DEFAULT_AS_BYTES  (512ull * 1024ull * 1024ull)
#define NH_SANDBOX_DEFAULT_FS_BYTES  (128ull * 1024ull * 1024ull)
#define NH_SANDBOX_DEFAULT_NOFILE    64u

static nh_porthome_sandbox_limits g_defaults = {0};
static char g_forced_drop_user[64];
static int  g_last_used_fallback;

static const char *effective_drop_user(const char *arg) {
  if (g_forced_drop_user[0]) return g_forced_drop_user;
  if (arg && arg[0]) return arg;
  if (g_defaults.drop_user && g_defaults.drop_user[0])
    return g_defaults.drop_user;
  return NH_SANDBOX_DEFAULT_USER;
}

static unsigned defaults_cpu(void) {
  return g_defaults.rlimit_cpu_sec ? g_defaults.rlimit_cpu_sec
                                   : NH_SANDBOX_DEFAULT_CPU_SEC;
}
static size_t defaults_as(void) {
  return g_defaults.rlimit_as_bytes ? g_defaults.rlimit_as_bytes
                                    : NH_SANDBOX_DEFAULT_AS_BYTES;
}
static size_t defaults_fsize(void) {
  return g_defaults.rlimit_fsize_bytes ? g_defaults.rlimit_fsize_bytes
                                       : NH_SANDBOX_DEFAULT_FS_BYTES;
}
static unsigned defaults_nofile(void) {
  return g_defaults.rlimit_nofile ? g_defaults.rlimit_nofile
                                  : NH_SANDBOX_DEFAULT_NOFILE;
}

void nh_porthome_sandbox_set_defaults(const nh_porthome_sandbox_limits *lim) {
  if (!lim) { memset(&g_defaults, 0, sizeof g_defaults); return; }
  g_defaults = *lim;
}

void nh_porthome_sandbox_set_drop_user(const char *name) {
  if (!name || !*name) { g_forced_drop_user[0] = '\0'; return; }
  snprintf(g_forced_drop_user, sizeof g_forced_drop_user, "%s", name);
}

int nh_porthome_sandbox_last_used_fallback_user(void) {
  int v = g_last_used_fallback;
  g_last_used_fallback = 0;
  return v;
}

/* Resolve the requested drop_user; fall back to "nobody" if the
 * dedicated user is missing (sysusers.d not yet applied — typical on
 * CI hosts). Sets *out_used_fallback = 1 on fallback. Refuses uid 0
 * either way. Returns 0 on success and fills *out_pw with a pointer
 * to a static/stateful passwd entry (caller must not free). */
static int resolve_drop_user(const char *requested,
                             struct passwd **out_pw,
                             int *out_used_fallback) {
  *out_used_fallback = 0;
  const char *name = effective_drop_user(requested);
  struct passwd *pw = getpwnam(name);
  if (!pw) {
    /* Try the fallback exactly once. */
    if (strcmp(name, NH_SANDBOX_FALLBACK_USER) != 0) {
      pw = getpwnam(NH_SANDBOX_FALLBACK_USER);
      if (pw) *out_used_fallback = 1;
    }
  }
  if (!pw) return -1;
  if (pw->pw_uid == 0) return -1; /* never drop to root */
  *out_pw = pw;
  return 0;
}

/* Close every fd on /proc/self/fd that isn't in @keep[]. Fallback path
 * (no /proc) closes fds 3..NOFILE-1 by brute force. Best-effort:
 * we do not treat close() failures as fatal. */
static void close_fds_except(const int *keep, size_t n_keep) {
  DIR *d = opendir("/proc/self/fd");
  if (d) {
    int dfd = dirfd(d);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
      char *end = NULL;
      long v = strtol(e->d_name, &end, 10);
      if (!end || *end) continue;
      int fd = (int)v;
      if (fd < 3) continue;
      if (fd == dfd) continue;
      int skip = 0;
      for (size_t i = 0; i < n_keep; i++) if (keep[i] == fd) { skip = 1; break; }
      if (skip) continue;
      (void)close(fd);
    }
    closedir(d);
    return;
  }
  /* /proc missing (e.g. inside a very stripped chroot); brute-force. */
  long maxfd = sysconf(_SC_OPEN_MAX);
  if (maxfd <= 0 || maxfd > 65535) maxfd = 65535;
  for (int fd = 3; fd < (int)maxfd; fd++) {
    int skip = 0;
    for (size_t i = 0; i < n_keep; i++) if (keep[i] == fd) { skip = 1; break; }
    if (skip) continue;
    (void)close(fd);
  }
}

static char *const default_envp[] = {
  (char *)"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
  (char *)"HOME=/nonexistent",
  (char *)"TZ=UTC",
  (char *)"LANG=C.UTF-8",
  NULL,
};

static int setrlimit_soft_hard(int res, rlim_t v) {
  struct rlimit rl = { .rlim_cur = v, .rlim_max = v };
  return setrlimit(res, &rl);
}

nh_porthome_sandbox_rc nh_porthome_spawn_sandboxed_ex(
    char *const argv[], char *const envp[],
    int stdin_fd, int stdout_fd, int stderr_fd,
    const int *keep_fds, size_t n_keep_fds,
    uint32_t deadline_ms,
    pid_t *out_pid) {
  (void)deadline_ms; /* wait-side hint — see nh_porthome_sandbox_wait */

  if (!argv || !argv[0] || !argv[0][0] || argv[0][0] != '/')
    return NH_PORTHOME_SANDBOX_ARG;
  if (!out_pid) return NH_PORTHOME_SANDBOX_ARG;

  /* Belt and braces: refuse to attempt privilege-drop from a non-root
   * parent. We never expect this path in production (nostr-authd runs
   * as root); a non-root caller would setuid() to itself which is a
   * no-op and silently ship a helper still running with the caller's
   * ambient authority. Fail loudly instead.
   *
   * Test seam: NH_PORTHOME_SANDBOX_ALLOW_NONROOT=1 in the environment
   * lets headless CI (which cannot become root) still exercise the
   * fd-hygiene / rlimit / no_new_privs paths — the child then runs
   * as the same uid as the parent, which is fine because the parent
   * is already unprivileged in that case. Production installs never
   * set this env var (the broker runs as root). */
  int allow_nonroot = 0;
  const char *nr = getenv("NH_PORTHOME_SANDBOX_ALLOW_NONROOT");
  if (nr && nr[0] == '1') allow_nonroot = 1;
  if (geteuid() != 0 && !allow_nonroot) return NH_PORTHOME_SANDBOX_NOT_ROOT;

  struct passwd *pw = NULL;
  int used_fallback = 0;
  if (geteuid() == 0) {
    if (resolve_drop_user(NULL, &pw, &used_fallback) != 0)
      return NH_PORTHOME_SANDBOX_NO_USER;
  }

  pid_t pid = fork();
  if (pid < 0) return NH_PORTHOME_SANDBOX_FORK;
  if (pid == 0) {
    /* ── Child ─────────────────────────────────────────────────────── */

    /* 1. Wire stdio first, before we close arbitrary fds. */
    int null_fd = -1;
    if (stdin_fd < 0 || stdout_fd < 0) {
      null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
      if (null_fd < 0) _exit(70);
    }
    int in_fd  = (stdin_fd  >= 0) ? stdin_fd  : null_fd;
    int out_fd = (stdout_fd >= 0) ? stdout_fd : null_fd;
    int err_fd = (stderr_fd >= 0) ? stderr_fd : 2; /* inherit journal */
    if (dup2(in_fd,  0) < 0) _exit(70);
    if (dup2(out_fd, 1) < 0) _exit(70);
    if (dup2(err_fd, 2) < 0) _exit(70);

    /* 2. Drop privileges (only when we can — see NOT_ROOT check above). */
    if (pw) {
      if (setgroups(1, &pw->pw_gid) != 0) _exit(70);
      if (setgid(pw->pw_gid) != 0)        _exit(70);
      if (setuid(pw->pw_uid) != 0)        _exit(70);
      if (getuid() == 0 || geteuid() == 0) _exit(70);
    }

    /* 3. no_new_privs BEFORE the rlimit/env clean-up so a suid binary
     *    on PATH (unlikely with our env, but belt and braces) cannot
     *    re-elevate. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(70);
    if (prctl(PR_SET_DUMPABLE,     0, 0, 0, 0) != 0) _exit(70);

    /* 4. rlimits. Best-effort — hosts with tight ambient rlimits may
     *    already be below our defaults; we forward errors as exit 70. */
    if (setrlimit_soft_hard(RLIMIT_CPU,    (rlim_t)defaults_cpu())    != 0) _exit(70);
    if (setrlimit_soft_hard(RLIMIT_AS,     (rlim_t)defaults_as())     != 0) _exit(70);
    if (setrlimit_soft_hard(RLIMIT_FSIZE,  (rlim_t)defaults_fsize())  != 0) _exit(70);
    if (setrlimit_soft_hard(RLIMIT_NOFILE, (rlim_t)defaults_nofile()) != 0) _exit(70);
    /* Deny new core dumps — belt-and-braces alongside PR_SET_DUMPABLE=0. */
    (void)setrlimit_soft_hard(RLIMIT_CORE, 0);

    /* 5. Close every fd that isn't stdio (or explicitly kept). Only
     *    0/1/2 + @keep_fds[] survive. Bead nostrc-ww50: the auth
     *    fetch spawner passes the child_staging_fd through here so the
     *    helper can inherit it via `--staging-fd N`. */
    {
      /* Cap the kept-fds list to a sane size so a rogue caller can't
       * blow the stack; NH_KEEP_MAX = NOFILE default (64). */
      #define NH_SANDBOX_KEEP_MAX 64
      int keep[NH_SANDBOX_KEEP_MAX];
      size_t nk = 0;
      keep[nk++] = 0;
      keep[nk++] = 1;
      keep[nk++] = 2;
      for (size_t i = 0; i < n_keep_fds && nk < NH_SANDBOX_KEEP_MAX; i++) {
        int fd = keep_fds ? keep_fds[i] : -1;
        if (fd < 0) continue;
        /* dedupe against stdio, but preserve if caller passes 0/1/2
         * explicitly — the loop below happily walks either way. */
        int dupe = 0;
        for (size_t j = 0; j < nk; j++) if (keep[j] == fd) { dupe = 1; break; }
        if (!dupe) keep[nk++] = fd;
      }
      close_fds_except(keep, nk);
      #undef NH_SANDBOX_KEEP_MAX
    }

    /* 6. Exec. Use the caller-supplied env if any, else our allow-list.
     *    argv[0] is absolute (checked in the parent). */
    char *const *use_env = envp ? envp : default_envp;
    execve(argv[0], argv, use_env);
    _exit(70);
  }

  /* Parent. */
  g_last_used_fallback = used_fallback;
  *out_pid = pid;
  return NH_PORTHOME_SANDBOX_OK;
}

/* Waitpid with a wall-clock deadline. We use a coarse 20 ms tick loop
 * — the sandbox is spawned once per PROVISION_HOME, so latency is
 * dominated by the network I/O the helper itself does, not this
 * polling interval. */
int nh_porthome_sandbox_wait(pid_t pid, uint32_t deadline_ms,
                             int *out_exit_code, int *out_signal,
                             int *out_timed_out) {
  if (out_exit_code) *out_exit_code = -1;
  if (out_signal)    *out_signal    = 0;
  if (out_timed_out) *out_timed_out = 0;
  if (pid <= 0) return -1;

  int status = 0;
  if (deadline_ms == 0) {
    for (;;) {
      pid_t r = waitpid(pid, &status, 0);
      if (r == pid) break;
      if (r < 0 && errno == EINTR) continue;
      if (r < 0) return -1;
    }
  } else {
    const uint32_t tick_ms = 20u;
    uint32_t waited = 0;
    int killed = 0;
    for (;;) {
      pid_t r = waitpid(pid, &status, WNOHANG);
      if (r == pid) break;
      if (r < 0 && errno == EINTR) continue;
      if (r < 0) return -1;
      if (r == 0) {
        if (waited >= deadline_ms && !killed) {
          /* Two-step: SIGTERM then SIGKILL after a grace tick. */
          (void)kill(pid, SIGTERM);
          killed = 1;
          if (out_timed_out) *out_timed_out = 1;
        } else if (killed && waited >= deadline_ms + 200u) {
          (void)kill(pid, SIGKILL);
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)tick_ms * 1000000L };
        nanosleep(&ts, NULL);
        waited += tick_ms;
        continue;
      }
    }
  }

  if (WIFEXITED(status)) {
    if (out_exit_code) *out_exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    if (out_signal) *out_signal = WTERMSIG(status);
  }
  return 0;
}

/* Thin wrapper preserving the pre-ww50 signature — no extra fds
 * survive close_fds_except beyond 0/1/2. */
nh_porthome_sandbox_rc nh_porthome_spawn_sandboxed(
    char *const argv[], char *const envp[],
    int stdin_fd, int stdout_fd, int stderr_fd,
    uint32_t deadline_ms,
    pid_t *out_pid) {
  return nh_porthome_spawn_sandboxed_ex(
      argv, envp, stdin_fd, stdout_fd, stderr_fd,
      NULL, 0, deadline_ms, out_pid);
}
