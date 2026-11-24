/* SPDX-License-Identifier: MIT
 *
 * session_broker.h - Credential → session token broker for Signet v2.
 *
 * D-Bus GetSession(service_url) authorizes the agent, resolves its unique
 * credential bound to the URL, and exchanges that credential for a session.
 * The lease stores a hash of the returned token, not the token itself.
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
struct SignetDenyList;

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
 * @service_url: provisioned service URL to match exactly; not an override.
 *
 * Request to exchange a stored credential for a service session.
 *
 * Since: 1.0
 */
typedef struct {
  const char *credential_id;  /* which credential to use */
  const char *agent_id;       /* requesting agent */
  const char *service_url;    /* must match the provisioned endpoint */
} SignetSessionRequest;

/**
 * signet_session_broker_get:
 * @store: (not nullable): a #SignetStore
 * @policy: (nullable): policy registry used for authorization. NULL fails
 *   closed (no capability was ever granted).
 * @deny: (nullable): live deny list; deny-listed agents are refused before
 *   any capability grant is considered.
 * @audit: (nullable): audit logger for non-secret events.
 * @req: (not nullable): session request.
 * @result: (out) (not nullable): session result to populate.
 *
 * Retrieves the credential through the unified credential-access path
 * (explicit capability, owner-before-decrypt, deny/revoke precedence,
 * hash-chained audit on every outcome), exchanges it for a service session
 * token, and records the associated lease. Clear @result with
 * signet_session_result_clear() on success.
 *
 * Returns: 0 on success, or -1 on error
 *
 * Since: 1.0
 */
int signet_session_broker_get(struct SignetStore *store,
                                struct SignetPolicyRegistry *policy,
                                struct SignetDenyList *deny,
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
