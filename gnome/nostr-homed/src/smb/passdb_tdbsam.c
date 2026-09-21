/*
 * Samba tdbsam adapter for the SMB credential authority.
 *
 * IMPLEMENTATION NOTE: this adapter shells out to `smbpasswd` and
 * `pdbedit` because Samba does not expose a stable public C API to
 * passdb from third parties.  The invocations are:
 *
 *   set_password(u, pw) -> smbpasswd -a -s <u>
 *                          stdin: "<pw>\n<pw>\n"   (new + confirm)
 *   disable(u)          -> smbpasswd -d <u>
 *   remove(u)           -> pdbedit  -x -u <u>
 *
 * The `-a` in the set_password invocation requires that <u> already
 * exists as a POSIX account (see smbpasswd(8) — "-a: this option
 * specifies that the username following should be added to the local
 * smbpasswd file, with the new password typed. This account must
 * already exist in the system password file /etc/passwd").
 *
 * Security properties:
 *   - The plaintext password is NEVER placed on argv or in the
 *     environment.  It reaches the child ONLY through an anonymous
 *     pipe fed into stdin, which the child consumes and forgets.
 *   - The parent wipes its plaintext copy immediately after the child
 *     exits (success or failure) and closes every pipe fd.
 *   - No shell is invoked; each argv slot is passed verbatim to
 *     execv(2), so passwords/usernames cannot be re-interpreted.
 *   - The child's stdout+stderr are redirected to /dev/null so the
 *     password prompt echoed by smbpasswd does not leak into any log.
 *
 * Because this file requires a real Samba install to exercise
 * meaningfully, the CI build ships only the mock adapter
 * (tests/integration/test_smb_credential.c).  See
 * tests/acceptance/run_tdbsam.sh for the disposable-lab VM acceptance.
 *
 * beads nostrc-rb0e.7.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "passdb_tdbsam.h"
#include "smb_credential.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Optional configuration: path to the smbpasswd/pdbedit binaries.
 * On a real system these are usually /usr/bin/{smbpasswd,pdbedit}.
 * The acceptance script may override at compile time or via env. */
#ifndef NH_SMB_SMBPASSWD_PATH
#define NH_SMB_SMBPASSWD_PATH "/usr/bin/smbpasswd"
#endif
#ifndef NH_SMB_PDBEDIT_PATH
#define NH_SMB_PDBEDIT_PATH "/usr/bin/pdbedit"
#endif

/* Env overrides — respected only when the caller passed NULL paths to
 * the factory.  Present to make disposable-lab acceptance easier. */
#define NH_SMB_ENV_SMBPASSWD "NH_SMB_SMBPASSWD_PATH"
#define NH_SMB_ENV_PDBEDIT   "NH_SMB_PDBEDIT_PATH"

struct nh_smb_passdb_tdbsam {
  char smbpasswd_path[256];
  char pdbedit_path[256];
};

static void copy_path(char dst[static 256], const char *src) {
  snprintf(dst, 256, "%s", src);
}

/* Public factory: caller owns the context and must free it.  If a NULL
 * path is passed, we consult the corresponding environment variable
 * before falling back to the compile-time default. */
nh_smb_passdb_tdbsam *nh_smb_passdb_tdbsam_new(const char *smbpasswd_path,
                                               const char *pdbedit_path) {
  nh_smb_passdb_tdbsam *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  const char *sp = smbpasswd_path;
  if (!sp || !*sp) sp = getenv(NH_SMB_ENV_SMBPASSWD);
  if (!sp || !*sp) sp = NH_SMB_SMBPASSWD_PATH;
  const char *pe = pdbedit_path;
  if (!pe || !*pe) pe = getenv(NH_SMB_ENV_PDBEDIT);
  if (!pe || !*pe) pe = NH_SMB_PDBEDIT_PATH;
  copy_path(t->smbpasswd_path, sp);
  copy_path(t->pdbedit_path, pe);
  return t;
}

void nh_smb_passdb_tdbsam_free(nh_smb_passdb_tdbsam *t) { free(t); }

/*
 * Wipe a buffer without the compiler eliding the store.  Volatile
 * pointer access is portable enough for our purposes; the buffer is
 * heap and short-lived.
 */
static void wipe(void *p, size_t n) {
  volatile unsigned char *v = (volatile unsigned char *)p;
  while (n--) *v++ = 0;
}

/*
 * Fork+exec helper that writes `stdin_bytes` (may be NULL) to the
 * child's stdin and waits for it to exit.  Returns 0 iff the child
 * exited normally with status 0, -1 otherwise.
 *
 * No shell is involved: each argv slot is passed verbatim to execv(2).
 * SIGPIPE is ignored around the write so that a child that closes
 * stdin early (or never reads it) surfaces as a normal short write
 * plus a non-zero waitpid, not a signal to the parent.
 *
 * The child's stdout+stderr are redirected to /dev/null; the parent
 * closes all pipe fds it holds before waitpid so the child never
 * blocks on write to a pipe we forgot to drain.
 */
static int run_cmd_with_stdin(const char *path, char *const argv[],
                              const char *stdin_bytes, size_t stdin_len) {
  int pipefd[2] = {-1, -1};
  if (stdin_bytes) {
    if (pipe(pipefd) != 0) return -1;
  }

  /* Ignore SIGPIPE for the write loop; restore afterwards.  This is
   * intentionally scoped to the parent, not inherited by the child
   * (fork copies the disposition; execv then resets to the default). */
  struct sigaction ign, prev;
  memset(&ign, 0, sizeof ign);
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  int saved_sigpipe = sigaction(SIGPIPE, &ign, &prev);

  pid_t pid = fork();
  if (pid < 0) {
    if (pipefd[0] >= 0) close(pipefd[0]);
    if (pipefd[1] >= 0) close(pipefd[1]);
    if (saved_sigpipe == 0) sigaction(SIGPIPE, &prev, NULL);
    return -1;
  }
  if (pid == 0) {
    /* Child: wire stdin from the pipe (or /dev/null), silence stdout/stderr,
     * then execv.  We deliberately do NOT touch inherited fds >2; the
     * caller is expected to keep those under control (they are typically
     * O_CLOEXEC in this codebase). */
    if (stdin_bytes) {
      close(pipefd[1]);
      if (dup2(pipefd[0], STDIN_FILENO) < 0) _exit(126);
      close(pipefd[0]);
    } else {
      int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
      if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
    }
    int dnw = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (dnw >= 0) {
      dup2(dnw, STDOUT_FILENO);
      dup2(dnw, STDERR_FILENO);
      close(dnw);
    }
    execv(path, argv);
    _exit(127);
  }

  /* Parent: close the read end (child owns it), stream stdin, close the
   * write end so the child sees EOF, then reap. */
  if (pipefd[0] >= 0) close(pipefd[0]);
  int wrote_all = 1;
  if (stdin_bytes) {
    const char *p = stdin_bytes;
    size_t remaining = stdin_len;
    while (remaining > 0) {
      ssize_t w = write(pipefd[1], p, remaining);
      if (w < 0) {
        if (errno == EINTR) continue;
        wrote_all = 0;
        break;
      }
      if (w == 0) { wrote_all = 0; break; }
      p += w;
      remaining -= (size_t)w;
    }
    close(pipefd[1]);
  }

  int status = 0;
  pid_t r;
  while ((r = waitpid(pid, &status, 0)) < 0) {
    if (errno != EINTR) break;
  }

  if (saved_sigpipe == 0) sigaction(SIGPIPE, &prev, NULL);

  if (r < 0) return -1;
  if (!wrote_all) return -1;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
  return -1;
}

/* ---- ops ---- */

static int tdbsam_set_password(void *ctx, const char *username,
                               const char *password) {
  nh_smb_passdb_tdbsam *t = ctx;
  if (!t || !username || !password) return -1;
  /* Reject NUL/newline in the username to avoid confusing smbpasswd's
   * line-based prompts.  A NUL in the password is similarly rejected
   * because it would truncate the stdin blob unpredictably. */
  if (strchr(username, '\n') || strchr(username, '\r')) return -1;
  size_t plen = strlen(password);
  if (memchr(password, '\n', plen) || memchr(password, '\r', plen)) return -1;

  /*
   * smbpasswd -a -s USERNAME
   *   -a: add (username must already exist as a POSIX account)
   *   -s: silent — read the new password from stdin and read it
   *       AGAIN for confirmation.  Two newline-terminated copies.
   *
   * Build "<pw>\n<pw>\n" into a heap buffer we wipe on the way out.
   * The buffer never touches argv, env, or a file.
   */
  size_t blen = plen * 2 + 2;
  char *blob = malloc(blen);
  if (!blob) return -1;
  memcpy(blob, password, plen);
  blob[plen] = '\n';
  memcpy(blob + plen + 1, password, plen);
  blob[plen + 1 + plen] = '\n';

  char *argv[] = {
    t->smbpasswd_path,
    (char *)"-a",
    (char *)"-s",
    (char *)username,
    NULL,
  };
  int rc = run_cmd_with_stdin(t->smbpasswd_path, argv, blob, blen);
  wipe(blob, blen);
  free(blob);
  return rc;
}

static int tdbsam_disable(void *ctx, const char *username) {
  nh_smb_passdb_tdbsam *t = ctx;
  if (!t || !username) return -1;
  if (strchr(username, '\n') || strchr(username, '\r')) return -1;
  /* smbpasswd -d disables the account (sets the D flag).  No stdin. */
  char *argv[] = {
    t->smbpasswd_path,
    (char *)"-d",
    (char *)username,
    NULL,
  };
  return run_cmd_with_stdin(t->smbpasswd_path, argv, NULL, 0);
}

static int tdbsam_remove(void *ctx, const char *username) {
  nh_smb_passdb_tdbsam *t = ctx;
  if (!t || !username) return -1;
  if (strchr(username, '\n') || strchr(username, '\r')) return -1;
  /* pdbedit -x -u USERNAME removes the account from the passdb.  No stdin. */
  char *argv[] = {
    t->pdbedit_path,
    (char *)"-x",
    (char *)"-u",
    (char *)username,
    NULL,
  };
  return run_cmd_with_stdin(t->pdbedit_path, argv, NULL, 0);
}

const nh_smb_passdb_ops nh_smb_passdb_tdbsam_ops = {
  tdbsam_set_password,
  tdbsam_disable,
  tdbsam_remove,
};
