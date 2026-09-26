/*
 * nostr-authctl — small administrative CLI for the Nostr auth runtime.
 *
 * Plan §4.1 A3: production caller for the SMB authority's expired-
 * credential sweep.  The sweep_expired path was previously exercised
 * only by integration tests; this CLI wires it to a scheduled systemd
 * timer (nostr-smb-sweep.timer/.service) so a running production
 * install actually revokes expired credentials without depending on
 * broker restart or a hand-driven admin console.
 *
 * The CLI deliberately runs OUT of the broker main loop: keeping it as
 * an exec-once subcommand preserves the dep-purity gate posture (no new
 * library edges into `nostr-authd`, plan §5.1) and gives an operator a
 * safe re-entry point during recovery.
 *
 * Subcommands:
 *
 *     smb-sweep [--journal PATH]
 *         Open the SMB credential authority journal, run one sweep of
 *         expired credentials, and close.  Prints one JSON line to
 *         stdout on success and returns 0.  Non-zero exit means the
 *         sweep did not complete; stderr carries a human-readable
 *         reason.  With no --journal, uses the packaged default
 *         (/var/lib/nostr-auth/smb.db) — matches the -DNH_AUTHD_ARGS
 *         path installed by the .service unit.
 *
 * The passdb adapter used by this CLI is intentionally the same
 * process-local tdbsam adapter the broker links against, so a sweep
 * observed here has the same visible effect as one triggered by the
 * broker itself.
 */
#define _POSIX_C_SOURCE 200809L
#include "smb_credential.h"
#include "passdb_tdbsam.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef NH_AUTHCTL_DEFAULT_JOURNAL
#define NH_AUTHCTL_DEFAULT_JOURNAL "/var/lib/nostr-auth/smb.db"
#endif

static void usage(FILE *out) {
  fprintf(out,
      "usage: nostr-authctl smb-sweep [--journal PATH]\n"
      "\n"
      "Subcommands:\n"
      "  smb-sweep     Revoke SMB credentials whose expiry has passed.\n"
      "\n"
      "Options:\n"
      "  --journal PATH   Path to the SMB issuance journal SQLite file\n"
      "                   (default: %s)\n"
      "  -h, --help       Show this help and exit\n",
      NH_AUTHCTL_DEFAULT_JOURNAL);
}

static uint64_t wall_ms_now(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

static int cmd_smb_sweep(int argc, char **argv) {
  const char *journal = NH_AUTHCTL_DEFAULT_JOURNAL;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--journal")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "nostr-authctl: --journal requires a path\n");
        return 2;
      }
      journal = argv[++i];
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage(stdout);
      return 0;
    } else {
      fprintf(stderr, "nostr-authctl smb-sweep: unknown arg: %s\n", argv[i]);
      return 2;
    }
  }

  /* Sweep operates read-mostly through the same passdb adapter as the
   * broker.  Load the standalone-Samba config from the environment
   * (NH_SMB_CONF) or fall back to the shipped default so `smbpasswd -c`
   * / `pdbedit -s` in the sweep-invoked revoke path targets the same
   * dedicated tdbsam nostr-authd writes to.  NULL here means the env
   * override + compile-time default take over inside the constructor. */
  nh_smb_passdb_tdbsam *tdbsam = nh_smb_passdb_tdbsam_new(NULL, NULL, NULL);
  if (!tdbsam) {
    fprintf(stderr,
        "nostr-authctl smb-sweep: cannot construct tdbsam adapter\n");
    return 3;
  }

  nh_smb_authority *auth = NULL;
  nh_smb_rc rc = nh_smb_authority_open(journal, &nh_smb_passdb_tdbsam_ops,
                                       tdbsam, &auth);
  if (rc != NH_SMB_OK) {
    fprintf(stderr,
        "nostr-authctl smb-sweep: open failed: %s (%s)\n",
        nh_smb_rc_name(rc),
        auth ? nh_smb_authority_error_detail(auth) : "no detail");
    if (auth) nh_smb_authority_close(auth);
    nh_smb_passdb_tdbsam_free(tdbsam);
    return 3;
  }

  size_t swept = 0;
  rc = nh_smb_authority_sweep_expired(auth, wall_ms_now(), &swept);
  if (rc != NH_SMB_OK) {
    fprintf(stderr,
        "nostr-authctl smb-sweep: sweep failed: %s (%s)\n",
        nh_smb_rc_name(rc),
        nh_smb_authority_error_detail(auth));
    nh_smb_authority_close(auth);
    nh_smb_passdb_tdbsam_free(tdbsam);
    return 4;
  }

  fprintf(stdout,
      "{\"op\":\"smb-sweep\",\"journal\":\"%s\",\"revoked\":%zu}\n",
      journal, swept);
  nh_smb_authority_close(auth);
  nh_smb_passdb_tdbsam_free(tdbsam);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(stderr);
    return 2;
  }
  if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
    usage(stdout);
    return 0;
  }
  if (!strcmp(argv[1], "smb-sweep")) {
    return cmd_smb_sweep(argc - 2, argv + 2);
  }
  fprintf(stderr, "nostr-authctl: unknown subcommand: %s\n", argv[1]);
  usage(stderr);
  return 2;
}
