/*
 * Standalone SMB credential authority (bucket D5).
 *
 * The Nostr-homed login runtime authenticates users via Nostr keys.
 * Samba's passdb, however, still requires a password.  This authority
 * closes the gap: once the broker has produced an SMB_CREDENTIAL proof
 * receipt for an account, the authority mints a short-lived, random
 * SMB password, installs it in Samba's passdb through a pluggable
 * adapter, and returns the password EXACTLY ONCE in a volatile,
 * zeroizing envelope.  The password is never persisted; only issuance
 * metadata (credential_id, username, uid, pubkey, issued/expiry
 * timestamps, revocation state) is written to the SQLite journal.
 *
 * The authority is deliberately isolated from the broker: it is a
 * separate object (later a separate service) that only observes proof
 * receipts through its caller.  The wire-level protocol
 * (NH_AUTH_OP_BEGIN_SMB_PROOF) is not implemented here; the broker
 * will call nh_smb_credential_issue() after validating the SMB proof
 * receipt.
 *
 * Design references: beads nostrc-rb0e.6.
 */
#ifndef NH_SMB_CREDENTIAL_H
#define NH_SMB_CREDENTIAL_H

#include "nostr_identity.h"
#include "secure_buf.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NH_SMB_CREDENTIAL_ID_LEN 36u
#define NH_SMB_CREDENTIAL_ID_CAP (NH_SMB_CREDENTIAL_ID_LEN + 1u)

/* Password policy floor: minimum length of the CSPRNG password we mint.
 * Passwords come from base64url encoding at least 16 random bytes, which
 * yields 22 characters, comfortably above this floor.  Callers cannot
 * downgrade the policy. */
#define NH_SMB_PASSWORD_MIN_LEN 20u
#define NH_SMB_PASSWORD_MAX_LEN 64u

/* Default random-byte input width for password minting.  base64url of 18
 * bytes is 24 characters (no padding). */
#define NH_SMB_PASSWORD_ENTROPY_BYTES 18u

/* Result codes.  Stable across implementations; never persisted. */
typedef enum nh_smb_rc {
  NH_SMB_OK = 0,
  NH_SMB_INVALID = 1,
  NH_SMB_NO_MEMORY = 2,
  NH_SMB_STORAGE_ERROR = 3,
  NH_SMB_PASSDB_ERROR = 4,
  NH_SMB_NOT_FOUND = 5,
  NH_SMB_INTERNAL = 6,
  /* Startup reconciliation refused readiness: the dedicated Samba
   * passdb enumerated by `pdbedit -L -s <smb.conf.standalone>`
   * disagrees with the SQLite issuance journal (`smb.db`).  Returned
   * by `nh_smb_authority_open_ex()` when it is asked to reconcile and
   * finds a passdb account absent from the active-issuance set (or
   * vice versa).  The authority does NOT auto-repair — an operator
   * runs `nostr-authctl smb-journal --dump` and reconciles by hand.
   * Plan §4.2 B3 (2026-09-25). */
  NH_SMB_RECONCILE_REQUIRED = 7
} nh_smb_rc;

/* Revocation reason recorded in the journal.  Strings are stable tokens. */
typedef enum nh_smb_revoke_reason {
  NH_SMB_REVOKE_ADMIN = 1,
  NH_SMB_REVOKE_EXPIRED = 2,
  NH_SMB_REVOKE_ACCOUNT_DISABLED = 3,
  NH_SMB_REVOKE_ROTATED = 4,
  NH_SMB_REVOKE_SERVICE_RESTART = 5
} nh_smb_revoke_reason;

/*
 * Adapter contract to Samba's password backend.  All three callbacks are
 * required; each takes the adapter context registered at open time and
 * returns 0 on success, non-zero on failure.  Adapters must not persist
 * the password beyond what Samba itself does, and must never log it.
 * The real tdbsam adapter shells out to pdbedit/smbpasswd; a mock is
 * provided for portable tests.
 */
typedef struct nh_smb_passdb_ops {
  int (*set_password)(void *ctx, const char *username, const char *password);
  int (*disable)(void *ctx, const char *username);
  int (*remove)(void *ctx, const char *username);
  /*
   * Optional: enumerate every account currently present in the passdb.
   * On success returns 0, sets `*users_out` to a caller-owned array of
   * caller-owned NUL-terminated username strings, and `*count_out` to
   * the array length.  Ownership rule: caller frees every entry then
   * the array itself.  On failure returns non-zero and leaves *out
   * unchanged.  When this pointer is NULL the authority skips its
   * startup reconciliation pass (used by portable test mocks that
   * never diverge from the journal).  Plan §4.2 B3 (2026-09-25).
   */
  int (*enumerate)(void *ctx, char ***users_out, size_t *count_out);
} nh_smb_passdb_ops;

/* Opaque authority handle.  Not thread safe: one owner, one event loop. */
typedef struct nh_smb_authority nh_smb_authority;

/*
 * Volatile one-time delivery envelope.  Holds the freshly-minted
 * password in a mlock'd secure buffer.  Callers MUST call
 * nh_smb_envelope_clear() before dropping the struct; the buffer is
 * zeroized regardless of whether it was consumed.  Reading `password`
 * as a NUL-terminated string is valid until clear (or reuse) is
 * called.  The envelope is single-use: once the caller has handed the
 * password to the desktop client it should be cleared immediately.
 */
typedef struct nh_smb_envelope {
  char credential_id[NH_SMB_CREDENTIAL_ID_CAP];
  char username[NH_IDENTITY_USERNAME_CAP];
  uint64_t issued_at_ms;
  uint64_t expires_at_ms;
  size_t password_len; /* excludes trailing NUL */
  nostr_secure_buf password;
  bool consumed;
} nh_smb_envelope;

/*
 * Optional binding string recorded (opaque) alongside the issuance so
 * an observer can correlate the receipt with the credential without
 * seeing the password.  E.g. a hex-encoded receipt token digest.  May
 * be NULL/empty.  Length is bounded; longer strings are rejected.
 */
#define NH_SMB_BINDING_MAX 128u

typedef struct nh_smb_issue_request {
  nh_identity_account account;
  const char *binding; /* optional; NUL-terminated; NH_SMB_BINDING_MAX max */
  uint64_t now_monotonic_ms;
  uint64_t lifetime_ms; /* > 0 */
} nh_smb_issue_request;

/*
 * Lifecycle.  `journal_path` is the SQLite issuance journal file
 * (created if missing; caller owns its permissions/directory).  The
 * adapter and ctx are borrowed; they must outlive the authority.  On
 * open, any credentials whose expiry has already passed are revoked
 * (a single idempotent nh_smb_authority_sweep_expired() pass runs at
 * the end of the open path; failure to sweep is logged but does not
 * fail open — the CLI/timer sweep in nostr-authctl smb-sweep is the
 * scheduled backstop, plan §4.1 A3).
 * On close, all currently-outstanding credentials are revoked (service
 * restart is a revocation event).
 */
nh_smb_rc nh_smb_authority_open(const char *journal_path,
                                const nh_smb_passdb_ops *passdb_ops,
                                void *passdb_ctx,
                                nh_smb_authority **out);

/*
 * Extended open() taking the dedicated standalone-Samba config path.
 * When `smb_conf_path` is non-NULL and non-empty AND the passdb ops
 * table exposes `enumerate`, the first open of an empty journal records
 * the passdb usernames as one-time adopted accounts. Subsequent opens
 * diff the dedicated passdb against active issuance rows plus those
 * adopted usernames. Drift fails with NH_SMB_RECONCILE_REQUIRED; the
 * authority does NOT auto-repair after bootstrap.
 *
 * Passing NULL / empty `smb_conf_path` is equivalent to calling the
 * plain nh_smb_authority_open(): no reconciliation pass runs and no
 * `-s`/`-c` flag is forwarded to the passdb adapter.  Portable tests
 * that construct a mock passdb ops table without `enumerate` also
 * skip reconciliation — the authority does not treat that as an
 * error, it treats it as "reconciliation is not this adapter's job".
 *
 * Plan §4.2 B3 (2026-09-25).  Beads nostrc-69pw.
 */
nh_smb_rc nh_smb_authority_open_ex(const char *journal_path,
                                   const char *smb_conf_path,
                                   const nh_smb_passdb_ops *passdb_ops,
                                   void *passdb_ctx,
                                   nh_smb_authority **out);
void nh_smb_authority_close(nh_smb_authority *authority);
const char *nh_smb_authority_error_detail(const nh_smb_authority *authority);
const char *nh_smb_rc_name(nh_smb_rc rc);

/*
 * Issue a fresh SMB credential.  On success:
 *   - `out` is populated with the credential id, timestamps and a
 *     zeroizing envelope carrying the plaintext password.  The caller
 *     owns the envelope and MUST call nh_smb_envelope_clear().
 *   - The passdb adapter has been asked to install the password.
 *   - The journal contains the issuance record (username, uid, pubkey,
 *     issued_at, expires_at, credential_id); no password is stored.
 *   - Any previously-outstanding credential for the same username is
 *     revoked ("rotated"); only one credential is active at a time.
 * On failure the envelope is not populated and no journal row is added.
 * If passdb installation succeeded but the subsequent journal write
 * failed, the authority tears down the passdb entry to keep them in
 * sync.
 */
nh_smb_rc nh_smb_credential_issue(nh_smb_authority *authority,
                                  const nh_smb_issue_request *request,
                                  nh_smb_envelope *out);

/*
 * Revoke a named credential.  Marks the currently-active journal row
 * (if any) revoked with the given reason and removes/disables the
 * account in the passdb backend.  Returns NH_SMB_NOT_FOUND if there is
 * no active journal row and the passdb adapter's remove callback
 * reports no such user.
 */
nh_smb_rc nh_smb_credential_revoke(nh_smb_authority *authority,
                                   const char *username,
                                   nh_smb_revoke_reason reason);

/*
 * Sweep all credentials whose expiry is at or before `now_ms` and
 * revoke each.  Returns the number of credentials revoked in
 * `revoked_out` (optional).  Safe to call from a timer.
 */
nh_smb_rc nh_smb_authority_sweep_expired(nh_smb_authority *authority,
                                         uint64_t now_ms,
                                         size_t *revoked_out);

/* Envelope helpers.  clear() wipes and frees the secure buffer; safe on
 * a zero-initialised envelope. */
void nh_smb_envelope_clear(nh_smb_envelope *envelope);

/*
 * Read-only journal probe used by tests and administrative tooling.
 * `out_row` is opaque state; NULL fields are permitted for callers
 * that only want the count.
 */
typedef struct nh_smb_journal_row {
  char credential_id[NH_SMB_CREDENTIAL_ID_CAP];
  char username[NH_IDENTITY_USERNAME_CAP];
  uint32_t uid;
  char pubkey_hex[NH_IDENTITY_PUBKEY_HEX_CAP];
  uint64_t issued_at_ms;
  uint64_t expires_at_ms;
  uint64_t revoked_at_ms; /* 0 when still active */
  int revoke_reason; /* 0 when still active */
} nh_smb_journal_row;

nh_smb_rc nh_smb_authority_lookup_active(nh_smb_authority *authority,
                                         const char *username,
                                         nh_smb_journal_row *out_row);
nh_smb_rc nh_smb_authority_count(nh_smb_authority *authority,
                                 size_t *rows_out);

#ifdef __cplusplus
}
#endif

#endif /* NH_SMB_CREDENTIAL_H */
