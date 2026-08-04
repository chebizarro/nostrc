/* SPDX-License-Identifier: MIT
 *
 * credential_access.h - Unified credential access control for Signet v2.
 *
 * Every runtime retrieval of a stored credential payload (D-Bus GetToken,
 * session brokering, git credential helper, NIP-5L) MUST go through
 * signet_credential_access_acquire(). The single path enforces, in order:
 *
 *   1. Deny-list precedence  - a deny-listed agent pubkey loses regardless of
 *                              any capability grant (deny > allow).
 *   2. Explicit capability   - the caller must name the capability it is
 *                              exercising; a missing policy registry or an
 *                              unassigned agent fails closed.
 *   3. Rate limiting         - token-bucket per (agent, capability).
 *   4. Owner before decrypt  - ownership and credential status (revoked >
 *                              expired) are checked on METADATA ONLY; the
 *                              payload is never decrypted for a caller that
 *                              is not the owner.
 *   5. Type deny rules       - the policy's disallowed_credential_types are
 *                              enforced against the credential's type.
 *   6. One-use lease burn    - when a lease_id is presented it is consumed
 *                              atomically; a burned/expired/foreign lease
 *                              fails closed.
 *   7. Hash-chained audit    - every outcome (allow, deny, error) appends a
 *                              redacted entry to the store's tamper-evident
 *                              audit chain. Payload bytes never reach the
 *                              audit trail. A success whose audit append
 *                              fails is rolled back and denied.
 */

#ifndef SIGNET_CREDENTIAL_ACCESS_H
#define SIGNET_CREDENTIAL_ACCESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "signet/store_secrets.h"

struct SignetStore;
struct SignetPolicyRegistry;
struct SignetDenyList;
struct SignetAuditLogger;

/**
 * SignetCredAccessStatus:
 * Outcome of a unified credential access request. Anything other than
 * SIGNET_CRED_ACCESS_OK means the payload was NOT released.
 *
 * Since: 1.3
 */
typedef enum {
  SIGNET_CRED_ACCESS_OK = 0,
  SIGNET_CRED_ACCESS_NOT_FOUND,
  SIGNET_CRED_ACCESS_EXPIRED,
  SIGNET_CRED_ACCESS_REVOKED,
  SIGNET_CRED_ACCESS_NO_CAPABILITY,
  SIGNET_CRED_ACCESS_RATE_LIMITED,
  SIGNET_CRED_ACCESS_DENY_LISTED,
  SIGNET_CRED_ACCESS_NOT_OWNER,
  SIGNET_CRED_ACCESS_TYPE_DENIED,
  SIGNET_CRED_ACCESS_LEASE_INVALID,
  SIGNET_CRED_ACCESS_ERROR,
} SignetCredAccessStatus;

/**
 * SignetCredentialAccessContext:
 * @store: (not nullable): backing store (secrets, leases, audit chain).
 * @policy: (nullable): capability registry. NULL fails closed: every
 *   capability-gated request is denied.
 * @deny: (nullable): live deny list; NULL skips the deny check (no store-backed
 *   deny list configured).
 * @logger: (nullable): optional structured JSONL audit logger, in addition to
 *   the mandatory store hash chain.
 *
 * Shared dependencies for credential access decisions.
 *
 * Since: 1.3
 */
typedef struct {
  struct SignetStore *store;
  struct SignetPolicyRegistry *policy;
  struct SignetDenyList *deny;
  struct SignetAuditLogger *logger;
} SignetCredentialAccessContext;

/**
 * SignetCredentialAccessRequest:
 * @agent_id: (not nullable): AUTHENTICATED caller identity (from transport auth).
 * @credential_id: (not nullable): credential to access.
 * @capability: (not nullable): explicit capability being exercised
 *   (e.g. SIGNET_CAP_CREDENTIAL_GET_TOKEN). Empty/NULL fails closed.
 * @transport: (nullable): transport tag for audit ("dbus_unix", "contextvm", ...).
 * @lease_id: (nullable): pre-issued lease to burn on use (one-use semantics).
 * @issue_lease: issue a tracking lease recording this grant.
 * @lease_ttl_seconds: lease lifetime; <=0 defaults to 3600. Clamped to the
 *   credential's own expiry when one is set.
 *
 * A single credential access request.
 *
 * Since: 1.3
 */
typedef struct {
  const char *agent_id;
  const char *credential_id;
  const char *capability;
  const char *transport;
  const char *lease_id;
  bool issue_lease;
  int64_t lease_ttl_seconds;
} SignetCredentialAccessRequest;

/**
 * SignetCredentialAccessGrant:
 * @record: decrypted credential record (payload mlock'd). Valid only on OK.
 * @lease_id: issued tracking lease id, or NULL when issue_lease was false.
 * @lease_expires_at: expiry of the issued lease (0 if none).
 *
 * Result of a successful access. Clear with
 * signet_credential_access_grant_clear() (wipes the payload).
 *
 * Since: 1.3
 */
typedef struct {
  SignetSecretRecord record;
  char *lease_id;
  int64_t lease_expires_at;
} SignetCredentialAccessGrant;

/**
 * signet_credential_access_acquire:
 * @ctx: (not nullable): access dependencies.
 * @req: (not nullable): the request.
 * @now: current Unix time in seconds.
 * @out_grant: (out) (not nullable): populated on SIGNET_CRED_ACCESS_OK.
 *
 * Evaluate a credential access request end to end and, when authorized,
 * decrypt the payload. EVERY outcome appends a redacted entry to the
 * store's hash-chained audit log; a success that cannot be audited is
 * rolled back and reported as SIGNET_CRED_ACCESS_ERROR.
 *
 * Thread safety: safe to call concurrently; store access is serialized
 * internally.
 *
 * Returns: the access outcome; payload released only on SIGNET_CRED_ACCESS_OK
 *
 * Since: 1.3
 */
SignetCredAccessStatus signet_credential_access_acquire(
    const SignetCredentialAccessContext *ctx,
    const SignetCredentialAccessRequest *req,
    int64_t now,
    SignetCredentialAccessGrant *out_grant);

/**
 * signet_cred_access_reason:
 * @status: an access outcome.
 *
 * Stable, non-secret reason code string for audit/diagnostics.
 *
 * Returns: (transfer none): a static string
 *
 * Since: 1.3
 */
const char *signet_cred_access_reason(SignetCredAccessStatus status);

/**
 * signet_credential_access_grant_clear:
 * @grant: (nullable): grant to clear.
 *
 * Wipe and free a grant (payload is zeroized). Safe on NULL.
 *
 * Since: 1.3
 */
void signet_credential_access_grant_clear(SignetCredentialAccessGrant *grant);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_CREDENTIAL_ACCESS_H */
