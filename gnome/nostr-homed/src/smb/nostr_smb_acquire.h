/*
 * Desktop SMB credential acquisition (bucket D7, beads nostrc-rb0e.8).
 *
 * The acquire tool is a user-facing helper a logged-in user runs to obtain
 * their own short-lived SMB password from the broker via a Nostr proof,
 * and deliver it to a callable local sink (a Samba credentials= file, or a
 * one-shot stdout pipe).
 *
 * The transport, ACL and proof flow live in the D6 auth_client helper and
 * the broker; this module is a thin, testable core over that helper.  It:
 *
 *   1. Drives BEGIN_SMB_PROOF -> SELECT_PROVIDER -> SUBMIT_UNLOCK on an
 *      already-connected user.sock fd via nh_auth_client_smb_proof_with().
 *   2. On NH_AUTH_RESULT_OK, hands the envelope's password to the sink:
 *        - if sink->creds_path is non-NULL, writes a Samba credentials=
 *          file at that path, mode 0600, formatted as:
 *            username=<name>\n
 *            password=<pw>\n
 *        - if sink->stdout_stream is non-NULL, writes "<pw>\n" to it.
 *      Either or both may be set; at least one must be set for OK.
 *   3. Immediately wipes the secure envelope and any local stack copies.
 *   4. Returns a granular status the CLI maps to exit codes / stderr.
 *
 * The core does NOT own the fd, the passphrase memory, or the sink FILE*.
 * Callers close the fd on return; the getpass()/env-var passphrase source
 * lives in the CLI main().  The header exposes only what the headless
 * integration test needs to drive the core without exec'ing a binary.
 */
#ifndef NH_SMB_ACQUIRE_H
#define NH_SMB_ACQUIRE_H

#include "nostr_auth_protocol.h"

#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default endpoint for the user.sock broker connection. */
#define NH_SMB_ACQUIRE_DEFAULT_SOCKET "/run/nostr-auth/user.sock"

/* Default service token the broker recognises for SMB proofs. */
#define NH_SMB_ACQUIRE_DEFAULT_SERVICE "smb-credential"

/* Environment override for the socket path. */
#define NH_SMB_ACQUIRE_ENV_SOCKET "NOSTR_AUTH_USER_SOCK"

/* Environment override for a non-interactive passphrase (test/scripted). */
#define NH_SMB_ACQUIRE_ENV_PASSPHRASE "NOSTR_HOMED_PASSPHRASE"

/* Delivery sink.  Neither pointer is retained after nh_smb_acquire_run
 * returns.  If both are NULL the run returns NH_SMB_ACQUIRE_ERR_INTERNAL. */
typedef struct nh_smb_acquire_sink {
  /* Absolute path of the credentials= file to write (mode 0600). */
  const char *creds_path;
  /* Optional pipe for --stdout mode; only the password + '\n' is written. */
  FILE *stdout_stream;
} nh_smb_acquire_sink;

/* Granular acquire status.  The broker's protocol result is returned
 * separately via *result_out so callers can distinguish e.g. denied
 * from expired without mapping through this enum. */
typedef enum nh_smb_acquire_status {
  NH_SMB_ACQUIRE_OK = 0,
  /* auth_client transport failed (short read, EOF, invalid JSON, ...). */
  NH_SMB_ACQUIRE_ERR_TRANSPORT = 1,
  /* Broker returned INVALID_PROOF / UNKNOWN_ACCOUNT / NOT_READY. */
  NH_SMB_ACQUIRE_ERR_PROOF = 2,
  /* Broker returned DENIED (ACL / peer uid mismatch). */
  NH_SMB_ACQUIRE_ERR_DENIED = 3,
  /* Broker returned EXPIRED (challenge / proof lifetime). */
  NH_SMB_ACQUIRE_ERR_EXPIRED = 4,
  /* Broker returned RATE_LIMITED. */
  NH_SMB_ACQUIRE_ERR_RATE_LIMITED = 5,
  /* Delivery to sink failed (file open/write, mode enforcement). */
  NH_SMB_ACQUIRE_ERR_DELIVERY = 6,
  /* Configuration / unexpected internal error. */
  NH_SMB_ACQUIRE_ERR_INTERNAL = 7,
  /* Other unmapped broker result (e.g. PROVIDER_UNAVAILABLE). */
  NH_SMB_ACQUIRE_ERR_OTHER = 8,
} nh_smb_acquire_status;

/* Optional diagnostic hooks.  All fields are set on return; NULL disables. */
typedef struct nh_smb_acquire_diag {
  /* True iff the secure envelope was wiped after handoff (or on error). */
  bool envelope_cleared;
  /* Whether a credentials file was written to disk. */
  bool creds_file_written;
  /* Whether the stdout sink was written to. */
  bool stdout_written;
  /* Copy of the broker's reported account username (for logging).  Empty
   * string when the broker did not report one. */
  char account_username[64];
} nh_smb_acquire_diag;

/* Drive the SMB proof on an already-connected user.sock fd and deliver
 * the minted password to the sink.  See file header for the sink format
 * and wipe guarantees.
 *
 * `service` may be NULL to use NH_SMB_ACQUIRE_DEFAULT_SERVICE.
 * `provider` must be a canonical name (see NH_AUTH_PROVIDER_NAME_*).
 * `passphrase` must be non-NULL for the "local" provider; may be NULL for
 * "nip46" (the external signer gates approval).
 * `err_stream` is used only for human diagnostic lines and may be NULL.
 * `result_out` receives the broker's protocol result (always set).
 * `diag_out` is optional; the wipe/delivery assertions live here.
 */
nh_smb_acquire_status nh_smb_acquire_run(int fd,
                                         const char *service,
                                         const char *provider,
                                         const char *passphrase,
                                         const nh_smb_acquire_sink *sink,
                                         nh_auth_result *result_out,
                                         nh_smb_acquire_diag *diag_out,
                                         FILE *err_stream);

#ifdef __cplusplus
}
#endif

#endif /* NH_SMB_ACQUIRE_H */
