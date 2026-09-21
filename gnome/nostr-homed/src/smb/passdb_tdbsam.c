/*
 * Samba tdbsam adapter for the SMB credential authority.
 *
 * IMPLEMENTATION NOTE: this adapter shells out to `smbpasswd -a -s -s`
 * (and, on remove, `pdbedit -x`) because Samba does not expose a
 * stable public C API to passdb from third parties.  It therefore
 * requires that Samba is installed on the host with a passdb backend
 * of `tdbsam` and that this process runs as root (or as a user with
 * write access to the passdb files and the `smbpasswd`/`pdbedit`
 * binaries).  Because there is no way to exercise this from a
 * portable unit-test host, the CI build ships only the mock adapter
 * (see tests/integration/test_smb_credential.c) and this file is only
 * compiled when NOSTR_HOMED_ENABLE_SMB is on.  The maintainer
 * VM-tests the real adapter against a Samba install.
 *
 * The adapter deliberately routes the plaintext password to
 * smbpasswd's stdin (not argv, not the environment) and closes the
 * pipe so smbpasswd does not linger; the parent then explicitly wipes
 * the local copy.  Nothing else in this file touches the password.
 *
 * beads nostrc-rb0e.6.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

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
 * On a real system these are usually /usr/bin/{smbpasswd,pdbedit}. */
#ifndef NH_SMB_SMBPASSWD_PATH
#define NH_SMB_SMBPASSWD_PATH "/usr/bin/smbpasswd"
#endif
#ifndef NH_SMB_PDBEDIT_PATH
#define NH_SMB_PDBEDIT_PATH "/usr/bin/pdbedit"
#endif

typedef struct nh_smb_passdb_tdbsam {
  char smbpasswd_path[256];
  char pdbedit_path[256];
} nh_smb_passdb_tdbsam;

/* Public factory: caller owns the context and must free it. */
nh_smb_passdb_tdbsam *nh_smb_passdb_tdbsam_new(const char *smbpasswd_path,
                                               const char *pdbedit_path) {
  nh_smb_passdb_tdbsam *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  const char *sp = smbpasswd_path ? smbpasswd_path : NH_SMB_SMBPASSWD_PATH;
  const char *pe = pdbedit_path ? pdbedit_path : NH_SMB_PDBEDIT_PATH;
  snprintf(t->smbpasswd_path, sizeof t->smbpasswd_path, "%s", sp);
  snprintf(t->pdbedit_path, sizeof t->pdbedit_path, "%s", pe);
  return t;
}

void nh_smb_passdb_tdbsam_free(nh_smb_passdb_tdbsam *t) { free(t); }

/* Fork+exec helper that writes `stdin_bytes` to the child's stdin (if
 * non-NULL) and waits for it to exit. Returns 0 on exit code 0, -1
 * otherwise.  No shell involvement; each argv slot is passed verbatim. */
static int spawn(const char *path, char *const argv[], const char *stdin_bytes,
                 size_t stdin_len) {
  int pipefd[2] = {-1, -1};
  if (stdin_bytes && pipe(pipefd) != 0) return -1;
  pid_t pid = fork();
  if (pid < 0) {
    if (pipefd[0] >= 0) close(pipefd[0]);
    if (pipefd[1] >= 0) close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    if (stdin_bytes) {
      close(pipefd[1]);
      dup2(pipefd[0], STDIN_FILENO);
      close(pipefd[0]);
    } else {
      int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
      if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
    }
    /* Suppress stdout/stderr chatter — smbpasswd echoes the username. */
    int dnw = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (dnw >= 0) {
      dup2(dnw, STDOUT_FILENO);
      dup2(dnw, STDERR_FILENO);
      close(dnw);
    }
    execv(path, argv);
    _exit(127);
  }
  if (pipefd[0] >= 0) close(pipefd[0]);
  if (stdin_bytes) {
    const char *p = stdin_bytes;
    size_t remaining = stdin_len;
    while (remaining > 0) {
      ssize_t w = write(pipefd[1], p, remaining);
      if (w < 0) {
        if (errno == EINTR) continue;
        break;
      }
      p += w; remaining -= (size_t)w;
    }
    close(pipefd[1]);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
  return 0;
}

static int tdbsam_set_password(void *ctx, const char *username,
                               const char *password) {
  nh_smb_passdb_tdbsam *t = ctx;
  if (!t || !username || !password) return -1;
  /* smbpasswd -a -s USERNAME
   *   -a: add if not present
   *   -s: silent, read password from stdin (twice, terminated by newline)
   *
   * smbpasswd expects the password on two lines (new + confirm).  We
   * write a single "pw\npw\n" blob and close the pipe. */
  size_t plen = strlen(password);
  size_t blen = plen * 2 + 2;
  char *blob = malloc(blen);
  if (!blob) return -1;
  memcpy(blob, password, plen);
  blob[plen] = '\n';
  memcpy(blob + plen + 1, password, plen);
  blob[plen + 1 + plen] = '\n';
  char *argv[] = { t->smbpasswd_path, "-a", "-s", (char *)username, NULL };
  int rc = spawn(t->smbpasswd_path, argv, blob, blen);
  /* Wipe the local password blob. */
  volatile char *v = (volatile char *)blob;
  for (size_t i = 0; i < blen; i++) v[i] = 0;
  free(blob);
  return rc;
}

static int tdbsam_disable(void *ctx, const char *username) {
  nh_smb_passdb_tdbsam *t = ctx;
  if (!t || !username) return -1;
  /* smbpasswd -d disables the account (sets the D flag). */
  char *argv[] = { t->smbpasswd_path, "-d", (char *)username, NULL };
  return spawn(t->smbpasswd_path, argv, NULL, 0);
}

static int tdbsam_remove(void *ctx, const char *username) {
  nh_smb_passdb_tdbsam *t = ctx;
  if (!t || !username) return -1;
  /* pdbedit -x -u USERNAME removes the account from the passdb. */
  char *argv[] = { t->pdbedit_path, "-x", "-u", (char *)username, NULL };
  return spawn(t->pdbedit_path, argv, NULL, 0);
}

const nh_smb_passdb_ops nh_smb_passdb_tdbsam_ops = {
  tdbsam_set_password,
  tdbsam_disable,
  tdbsam_remove,
};
