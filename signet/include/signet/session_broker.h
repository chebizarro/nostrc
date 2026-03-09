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

typedef struct {
  char *session_token;
  int64_t expires_at;
  char *lease_id;
} SignetSessionResult;

typedef struct {
  const char *credential_id;  /* which credential to use */
  const char *agent_id;       /* requesting agent */
  const char *service_url;    /* optional: override service URL */
} SignetSessionRequest;

/* Broker a session: look up credential, exchange for session token.
 * Returns 0 on success (result populated), -1 on error.
 * result must be freed with signet_session_result_clear(). */
int signet_session_broker_get(struct SignetStore *store,
                                struct SignetPolicyRegistry *policy,
                                struct SignetAuditLogger *audit,
                                const SignetSessionRequest *req,
                                SignetSessionResult *result);

/* Free a session result. Safe on NULL. */
void signet_session_result_clear(SignetSessionResult *result);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_SESSION_BROKER_H */
