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

/**
 * SignetPolicyStore:
 * Opaque policy storage backend.
 *
 * Since: 1.0
 */
typedef struct SignetPolicyStore SignetPolicyStore;

/**
 * SignetPolicyRuleDecision:
 * @SIGNET_POLICY_RULE_DENY: signet policy rule deny
 * @SIGNET_POLICY_RULE_ALLOW: signet policy rule allow
 *
 * Stored policy rule decision values.
 *
 * Since: 1.0
 */
typedef enum {
  SIGNET_POLICY_RULE_DENY = 0,
  SIGNET_POLICY_RULE_ALLOW = 1
} SignetPolicyRuleDecision;

/**
 * SignetPolicyKeyView:
 * @identity: borrowed.
 * @client_pubkey_hex: borrowed (may be hex or npub; implementation may normalize).
 * @method: borrowed.
 * @event_kind: -1 means wildcard/not-applicable.
 *
 * Borrowed lookup key for policy evaluation.
 *
 * Since: 1.0
 */
typedef struct {
  const char *identity;          /* borrowed */
  const char *client_pubkey_hex; /* borrowed (may be hex or npub; implementation may normalize) */
  const char *method;            /* borrowed */
  int event_kind;                /* -1 means wildcard/not-applicable */
} SignetPolicyKeyView;

/**
 * SignetPolicyValue:
 * @decision: allow/deny decision.
 * @expires_at: unix seconds; 0 means never/unknown.
 * @reason_code: stable non-secret reason code.
 *
 * Policy value returned by a policy backend.
 *
 * Since: 1.0
 */
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
/**
 * signet_policy_store_get:
 * @ps: (not nullable): a #SignetPolicyStore
 * @key: (not nullable): key
 * @now: current Unix time in seconds
 * @out_val: (out) (not nullable): return location for val
 *
 * Get an existing policy value (including computed default for an identity policy). Returns:  - 0 if an identity policy was found and a decision was computed  - 1 if no policy exists for the requested identity (treat as "not found")  - -1 on error.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_policy_store_get(SignetPolicyStore *ps,
                            const SignetPolicyKeyView *key,
                            int64_t now,
                            SignetPolicyValue *out_val);

/* Put/overwrite a policy value. Returns 0 on success, -1 on error.
 * NOTE: File backend is currently read-only and returns -1. */
/**
 * signet_policy_store_put:
 * @ps: (not nullable): a #SignetPolicyStore
 * @key: (not nullable): key
 * @val: (not nullable): val
 * @now: current Unix time in seconds
 *
 * Put/overwrite a policy value. Returns 0 on success, -1 on error. NOTE: File backend is currently read-only and returns -1.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_policy_store_put(SignetPolicyStore *ps,
                            const SignetPolicyKeyView *key,
                            const SignetPolicyValue *val,
                            int64_t now);

/* Delete a policy entry. Returns 0 on success, 1 if missing, -1 on error.
 * NOTE: File backend is currently read-only and returns -1. */
/**
 * signet_policy_store_delete:
 * @ps: (not nullable): a #SignetPolicyStore
 * @key: (not nullable): key
 * @now: current Unix time in seconds
 *
 * Delete a policy entry. Returns 0 on success, 1 if missing, -1 on error. NOTE: File backend is currently read-only and returns -1.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_policy_store_delete(SignetPolicyStore *ps,
                               const SignetPolicyKeyView *key,
                               int64_t now);

/* Free the policy store. Safe on NULL. */
/**
 * signet_policy_store_free:
 * @ps: (nullable): a #SignetPolicyStore
 *
 * Free the policy store. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_policy_store_free(SignetPolicyStore *ps);

/* Set the full identity policy from a JSON object string.
 * Parses the JSON, updates the in-memory table, and persists to the backing
 * file (if file-backed).
 *
 * Expected JSON shape:
 *   {
 *     "default": "allow"|"deny",
 *     "allow_clients": ["*"|"<hex>"|"npub1..."],
 *     "deny_clients":  [...],
 *     "allow_methods": ["sign_event","nip04_encrypt",...],
 *     "deny_methods":  [...],
 *     "allow_kinds":   [1, 4, ...] or ["*"],
 *     "deny_kinds":    [...],
 *     "ttl_seconds":   3600
 *   }
 *
 * Returns 0 on success, -1 on error.  out_error receives a heap string on
 * error (caller frees with g_free; may be NULL). */
/**
 * signet_policy_store_set_identity_json:
 * @ps: (not nullable): a #SignetPolicyStore
 * @identity: (not nullable): policy identity name
 * @policy_json: (nullable): policy json
 * @now: current Unix time in seconds
 * @out_error: (out) (transfer full) (nullable): return location for a newly allocated error string
 *
 * Set the full identity policy from a JSON object string. Parses the JSON, updates the in-memory table, and persists to the backing file (if file-backed).
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_policy_store_set_identity_json(SignetPolicyStore *ps,
                                          const char *identity,
                                          const char *policy_json,
                                          int64_t now,
                                          char **out_error);

/* File-backed policy store. */
/**
 * signet_policy_store_file_new:
 * @path: (nullable): policy file path
 *
 * File-backed policy store.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetPolicyStore *signet_policy_store_file_new(const char *path);

/* SQLCipher-backed policy store (future implementation). */

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_POLICY_STORE_H */
