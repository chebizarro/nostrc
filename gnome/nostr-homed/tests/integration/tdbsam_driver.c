/*
 * tdbsam_driver — thin CLI driver over the real Samba passdb adapter.
 *
 * This is NOT a ctest.  It is invoked by the disposable-lab acceptance
 * script (tests/acceptance/run_tdbsam.sh) on a Samba-provisioned VM to
 * exercise the passdb_tdbsam ops end-to-end against smbpasswd/pdbedit.
 *
 * Usage:
 *   tdbsam_driver set <username>       # reads password from stdin
 *   tdbsam_driver disable <username>
 *   tdbsam_driver remove <username>
 *
 * Reads at most 128 bytes of password from stdin; trailing '\n' or
 * '\r' is stripped.  Exits 0 on success, non-zero on failure.  Prints
 * a single-line PASS/FAIL diagnostic to stderr.
 *
 * The smbpasswd / pdbedit paths may be overridden via the environment
 * variables NH_SMB_SMBPASSWD_PATH and NH_SMB_PDBEDIT_PATH.
 *
 * beads nostrc-rb0e.7.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "passdb_tdbsam.h"
#include "smb_credential.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void wipe(void *p, size_t n) {
  volatile unsigned char *v = (volatile unsigned char *)p;
  while (n--) *v++ = 0;
}

static int read_password(char *buf, size_t cap, size_t *out_len) {
  /* Read up to cap-1 bytes from stdin, terminate on EOF or newline. */
  size_t n = 0;
  while (n + 1 < cap) {
    ssize_t r = read(STDIN_FILENO, buf + n, cap - 1 - n);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) break;
    n += (size_t)r;
  }
  /* Strip a single trailing CR/LF pair. */
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
  buf[n] = '\0';
  *out_len = n;
  return 0;
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s <set|disable|remove> <username>\n"
          "       'set' reads the password from stdin (single line)\n",
          argv0);
}

int main(int argc, char **argv) {
  if (argc != 3) { usage(argv[0]); return 2; }
  const char *op = argv[1];
  const char *user = argv[2];

  nh_smb_passdb_tdbsam *t = nh_smb_passdb_tdbsam_new(NULL, NULL);
  if (!t) {
    fprintf(stderr, "tdbsam_driver: FAIL alloc\n");
    return 3;
  }

  int rc = -1;
  if (strcmp(op, "set") == 0) {
    char pw[129];
    size_t plen = 0;
    if (read_password(pw, sizeof pw, &plen) != 0 || plen == 0) {
      fprintf(stderr, "tdbsam_driver: FAIL empty/unreadable password on stdin\n");
      wipe(pw, sizeof pw);
      nh_smb_passdb_tdbsam_free(t);
      return 4;
    }
    rc = nh_smb_passdb_tdbsam_ops.set_password(t, user, pw);
    wipe(pw, sizeof pw);
  } else if (strcmp(op, "disable") == 0) {
    rc = nh_smb_passdb_tdbsam_ops.disable(t, user);
  } else if (strcmp(op, "remove") == 0) {
    rc = nh_smb_passdb_tdbsam_ops.remove(t, user);
  } else {
    usage(argv[0]);
    nh_smb_passdb_tdbsam_free(t);
    return 2;
  }

  nh_smb_passdb_tdbsam_free(t);
  if (rc != 0) {
    fprintf(stderr, "tdbsam_driver: FAIL op=%s user=%s rc=%d\n", op, user, rc);
    return 5;
  }
  fprintf(stderr, "tdbsam_driver: PASS op=%s user=%s\n", op, user);
  return 0;
}
