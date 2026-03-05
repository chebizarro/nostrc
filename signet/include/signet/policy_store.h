/* SPDX-License-Identifier: MIT
 *
 * policy_store.h - Policy storage backend interface for Signet.
 *
 * Policies are evaluated by (identity, client_pubkey, method, event_kind) and
 * return allow/deny with optional expiry (TTL).
 *
 * Phase 4:
 * - File-backed policy store is implemented (read-only; put/delete not supported yet)
 * - Stores return a stable reason_code describing which rule/default produced the decision
 */

#ifndef SIGNET_POLICY_STORE_H
#define SIGNET_POLICY_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct SignetPolicyStore SignetPolicyStore;

struct SignetVaultClient;

typedef enum {
  SIGNET_POLICY_RULE_DENY = 0,
  SIGNET_POLICY_RULE_ALLOW = 1
} SignetPolicyRuleDecision;

typedef struct {
  const char *identity;          /* borrowed */
  const char *client_pubkey_hex; /* borrowed (may be hex or npub; implementation may normalize) */
  const char *method;            /* borrowed */
  int event_kind;                /* -1 means wildcard/not-applicable */
} SignetPolicyKeyView;

typedef struct {
  SignetPolicyRuleDecision decision;
  int64_t expires_at;      /* unix seconds; 0 means never/unknown */

  /* Stable reason code describing how the decision was reached.
   * This pointer is owned by the policy store backend and must not be freed.
   * For the file backend this is a static string constant.
   *
   * Examples:
   * - "policy.deny.client"
   * - "policy.deny.method"
   * - "policy.deny.kind"
   * - "policy.allow.match"
   * - "policy.default_allow"
   * - "policy.default_deny"
   */
  const char *reason_code;
} SignetPolicyValue;

/* Get an existing policy value (including computed default for an identity policy).
 * Returns:
 *  - 0 if an identity policy was found and a decision was computed
 *  - 1 if no policy exists for the requested identity (treat as "not found")
 *  - -1 on error
 */
int signet_policy_store_get(SignetPolicyStore *ps,
                            const SignetPolicyKeyView *key,
                            int64_t now,
                            SignetPolicyValue *out_val);

/* Put/overwrite a policy value. Returns 0 on success, -1 on error.
 * NOTE: File backend is currently read-only and returns -1. */
int signet_policy_store_put(SignetPolicyStore *ps,
                            const SignetPolicyKeyView *key,
                            const SignetPolicyValue *val,
                            int64_t now);

/* Delete a policy entry. Returns 0 on success, 1 if missing, -1 on error.
 * NOTE: File backend is currently read-only and returns -1. */
int signet_policy_store_delete(SignetPolicyStore *ps,
                               const SignetPolicyKeyView *key,
                               int64_t now);

/* Free the policy store. Safe on NULL. */
void signet_policy_store_free(SignetPolicyStore *ps);

/* File-backed policy store. */
SignetPolicyStore *signet_policy_store_file_new(const char *path);

/* Vault-backed policy store (KV v2; later phases implement). */
SignetPolicyStore *signet_policy_store_vault_new(struct SignetVaultClient *vault,
                                                 const char *mount,
                                                 const char *prefix);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_POLICY_STORE_H */