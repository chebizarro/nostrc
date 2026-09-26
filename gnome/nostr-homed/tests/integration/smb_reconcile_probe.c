/*
 * smb_reconcile_probe — plan §4.2 B3 (2026-09-25).
 *
 * Ctest fixture that opens the SMB credential authority through
 * nh_smb_authority_open_ex() with a caller-supplied journal path
 * (arg 1) and smb.conf path (arg 2), then prints the resulting rc
 * name to stdout and exits with:
 *
 *   0  — NH_SMB_OK        (readiness accepted)
 *   42 — NH_SMB_RECONCILE_REQUIRED  (drift refused readiness — plan §4.2 B3)
 *   1  — any other rc     (unexpected failure)
 *
 * Exit code 42 is chosen out of the usual sysexits range so the
 * reconciliation test can distinguish it unambiguously from an
 * accidental compile-time abort (exit 1) or SIGABRT (exit 134).
 *
 * The passdb adapter used is the real tdbsam adapter, so `pdbedit -L
 * -s <conf>` is invoked exactly as it would be in production.  The
 * caller controls which `pdbedit` binary is used through the env
 * var NH_SMB_PDBEDIT_PATH and the smbpasswd path through
 * NH_SMB_SMBPASSWD_PATH.
 *
 * beads nostrc-69pw.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "passdb_tdbsam.h"
#include "smb_credential.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc != 3 && argc != 5) {
    fprintf(stderr, "usage: %s <journal-path> <smb.conf-path> [revoke <username>]\n", argv[0]);
    return 2;
  }
  const char *journal = argv[1];
  const char *smb_conf = argv[2];

  nh_smb_passdb_tdbsam *t = nh_smb_passdb_tdbsam_new(NULL, NULL, smb_conf);
  if (!t) {
    fprintf(stderr, "smb_reconcile_probe: tdbsam alloc failed\n");
    return 1;
  }
  nh_smb_authority *a = NULL;
  nh_smb_rc rc = nh_smb_authority_open_ex(journal, smb_conf,
                                          &nh_smb_passdb_tdbsam_ops,
                                          t, &a);
  if (rc == NH_SMB_OK && argc == 5) {
    if (strcmp(argv[3], "revoke") != 0) rc = NH_SMB_INVALID;
    else rc = nh_smb_credential_revoke(a, argv[4], NH_SMB_REVOKE_ADMIN);
  }
  const char *name = nh_smb_rc_name(rc);
  fprintf(stdout, "%s\n", name);
  if (a) {
    /* Print the detail line if there is one — the drift explanation
     * lives in a->error_detail. */
    const char *det = nh_smb_authority_error_detail(a);
    if (det && *det) fprintf(stderr, "detail: %s\n", det);
    nh_smb_authority_close(a);
  }
  nh_smb_passdb_tdbsam_free(t);
  if (rc == NH_SMB_OK) return 0;
  if (rc == NH_SMB_RECONCILE_REQUIRED) return 42;
  return 1;
}
