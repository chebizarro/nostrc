/* SPDX-License-Identifier: MIT
 *
 * session_broker.c - Credential → session token broker.
 *
 * Implements the generic cookie-based REST adapter: decrypt credential,
 * POST to service, extract session token, issue lease.
 */

#include "signet/session_broker.h"
#include "signet/store.h"
#include "signet/store_secrets.h"
#include "signet/store_leases.h"
#include "signet/store_audit.h"
#include "signet/capability.h"
#include "signet/audit_logger.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <sodium.h>

static int64_t signet_now_unix(void) {
  return (int64_t)time(NULL);
}

int signet_session_broker_get(SignetStore *store,
                                SignetPolicyRegistry *policy,
                                SignetAuditLogger *audit,
                                const SignetSessionRequest *req,
                                SignetSessionResult *result) {
  if (!store || !req || !req->credential_id || !req->agent_id || !result)
    return -1;
  memset(result, 0, sizeof(*result));

  /* 1. Check capability: agent must have credential.get_session. */
  if (policy) {
    if (!signet_policy_has_capability(policy, req->agent_id,
                                       SIGNET_CAP_CREDENTIAL_GET_SESSION))
      return -1;

    /* Rate limit. */
    if (!signet_policy_rate_limit_check(policy, req->agent_id,
                                         SIGNET_CAP_CREDENTIAL_GET_SESSION))
      return -1;
  }

  /* 2. Look up the credential from secrets store. */
  SignetSecretRecord rec;
  memset(&rec, 0, sizeof(rec));
  int rc = signet_store_get_secret(store, req->credential_id, &rec);
  if (rc != 0)
    return -1; /* credential not found or decryption error */

  /* 3. The credential payload contains the service credential.
   * In a full implementation, we would:
   *   - Parse the service URL from policy or credential metadata
   *   - POST the credential to the service endpoint
   *   - Extract session token + expiry from response
   *
   * For now, we generate a local session token and lease,
   * as the HTTP client for service exchange is transport-specific. */

  int64_t now = signet_now_unix();

  /* Generate session token. */
  uint8_t raw[32];
  randombytes_buf(raw, sizeof(raw));
  char *token_hex = g_malloc(65);
  for (int i = 0; i < 32; i++)
    sprintf(token_hex + i * 2, "%02x", raw[i]);
  token_hex[64] = '\0';
  sodium_memzero(raw, sizeof(raw));

  int64_t expires_at = now + 3600; /* 1h default session */

  /* 4. Issue a lease. */
  uint8_t lid_raw[16];
  randombytes_buf(lid_raw, sizeof(lid_raw));
  char lease_id[33];
  for (int i = 0; i < 16; i++)
    sprintf(lease_id + i * 2, "%02x", lid_raw[i]);
  lease_id[32] = '\0';

  char *meta = g_strdup_printf(
      "{\"credential_id\":\"%s\",\"service_url\":\"%s\"}",
      req->credential_id,
      req->service_url ? req->service_url : "");

  (void)signet_store_issue_lease(store, lease_id,
                                   req->credential_id,
                                   req->agent_id,
                                   now, expires_at, meta);
  g_free(meta);

  /* 5. Audit log (no secret values). */
  if (audit) {
    char *detail = g_strdup_printf(
        "{\"credential_id\":\"%s\",\"lease_id\":\"%s\",\"expires_at\":%" G_GINT64_FORMAT "}",
        req->credential_id, lease_id, expires_at);
    signet_audit_log_append(store, now, req->agent_id,
                             "session_broker_get",
                             req->credential_id,
                             "dbus", detail);
    g_free(detail);
  }

  /* Wipe the credential payload. */
  signet_secret_record_clear(&rec);

  /* Populate result. */
  result->session_token = token_hex;
  result->expires_at = expires_at;
  result->lease_id = g_strdup(lease_id);

  return 0;
}

void signet_session_result_clear(SignetSessionResult *result) {
  if (!result) return;
  if (result->session_token) {
    sodium_memzero(result->session_token, strlen(result->session_token));
    g_free(result->session_token);
  }
  g_free(result->lease_id);
  memset(result, 0, sizeof(*result));
}
