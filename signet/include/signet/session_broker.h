/* SPDX-License-Identifier: MIT
 *
 * session_broker.h - Credential → session token broker for Signet v2.
 *
 * Agent calls GetSession(credential_id) via D-Bus or NIP-5L. Signet:
 * 1. Checks credential_policy (authorized? session_broker=true? capability?)
 * 2. Rate limit check
 * 3. Decrypts credential payload into mlock'd buffer
 * 4. POSTs to service endpoint to obtain session token + expiry
 * 5. Issues lease, logs (no secret values in log)
 * 6. Returns {session_token, expires_in}
 */

#ifndef SIGNET_SESSION_BROKER_H
#define SIGNET_SESSION_BROKER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

struct SignetStore;
struct SignetKeyStore;
struct SignetPolicyRegistry;
struct SignetAuditLogger;

/**
 * SignetSessionResult:
 * @session_token: session token value.
 * @expires_at: expiry time as Unix seconds, or 0 for no expiry.
 * @lease_id: lease identifier.
 *
 * Session token and lease returned by the session broker.
 *
 * Since: 1.0
 */
typedef struct {
  char *session_token;
  int64_t expires_at;
  char *lease_id;
} SignetSessionResult;

/**
 * SignetSessionRequest:
 * @credential_id: which credential to use.
 * @agent_id: requesting agent.
 * @service_url: optional: override service URL.
 *
 * Request to exchange a stored credential for a service session.
 *
 * Since: 1.0
 */
typedef struct {
  const char *credential_id;  /* which credential to use */
  const char *agent_id;       /* requesting agent */
  const char *service_url;    /* optional: override service URL */
} SignetSessionRequest;

/**
 * signet_session_broker_get:
 * @store: (not nullable): a #SignetStore
 * @policy: (not nullable): policy registry used for authorization.
 * @audit: (not nullable): audit logger for non-secret events.
 * @req: (not nullable): session request.
 * @result: (out) (not nullable): session result to populate.
 *
 * Looks up the requested credential, exchanges it for a service session token,
 * and records the associated lease. Clear @result with
 * signet_session_result_clear() on success.
 *
 * Returns: 0 on success, or -1 on error
 *
 * Since: 1.0
 */
int signet_session_broker_get(struct SignetStore *store,
                                struct SignetPolicyRegistry *policy,
                                struct SignetAuditLogger *audit,
                                const SignetSessionRequest *req,
                                SignetSessionResult *result);

/* Free a session result. Safe on NULL. */
/**
 * signet_session_result_clear:
 * @result: (out) (nullable): session result to populate
 *
 * Free a session result. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_session_result_clear(SignetSessionResult *result);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_SESSION_BROKER_H */
