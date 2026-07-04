/* SPDX-License-Identifier: MIT
 *
 * policy_engine.h - Policy evaluation engine for Signet.
 *
 * This module combines policy storage lookups with decision semantics and
 * stable, auditable reason codes. It is called from both NIP-46 request
 * handling and management operations.
 *
 * Evaluates per-identity policy rules against incoming requests.
 */

#ifndef SIGNET_POLICY_ENGINE_H
#define SIGNET_POLICY_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

struct SignetAuditLogger;
struct SignetPolicyStore;

/**
 * SignetPolicyEngine:
 * Opaque policy evaluation engine.
 *
 * Since: 1.0
 */
typedef struct SignetPolicyEngine SignetPolicyEngine;

/**
 * SignetPolicyDecision:
 * @SIGNET_POLICY_DECISION_DENY: signet policy decision deny
 * @SIGNET_POLICY_DECISION_ALLOW: signet policy decision allow
 *
 * Final allow/deny decision values.
 *
 * Since: 1.0
 */
typedef enum {
  SIGNET_POLICY_DECISION_DENY = 0,
  SIGNET_POLICY_DECISION_ALLOW = 1
} SignetPolicyDecision;

/**
 * SignetPolicyResult:
 * @decision: allow/deny decision.
 * @expires_at: 0 = never.
 * @reason_code: stable string; never contains secrets.
 *
 * Result of evaluating a policy decision.
 *
 * Since: 1.0
 */
typedef struct {
  SignetPolicyDecision decision;
  int64_t expires_at;        /* 0 = never */
  const char *reason_code;   /* stable string; never contains secrets */
} SignetPolicyResult;

/**
 * SignetPolicyEngineConfig:
 * @default_decision: used when no policy entry matches.
 *
 * Configuration for the policy engine.
 *
 * Since: 1.0
 */
typedef struct {
  SignetPolicyDecision default_decision; /* used when no policy entry matches */
} SignetPolicyEngineConfig;

/* Create policy engine. Returns NULL on OOM. */
/**
 * signet_policy_engine_new:
 * @store: (not nullable): a #SignetStore
 * @audit: (not nullable): audit
 * @cfg: (nullable): configuration to use
 *
 * Create policy engine. Returns NULL on OOM.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetPolicyEngine *signet_policy_engine_new(struct SignetPolicyStore *store,
                                             struct SignetAuditLogger *audit,
                                             const SignetPolicyEngineConfig *cfg);

/* Free policy engine. Safe on NULL. */
/**
 * signet_policy_engine_free:
 * @pe: (nullable): a #SignetPolicyEngine
 *
 * Free policy engine. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_policy_engine_free(SignetPolicyEngine *pe);

/* Evaluate a policy decision.
 *
 * identity/client/method are required.
 * event_kind may be -1 when not applicable.
 */
/**
 * signet_policy_engine_eval:
 * @pe: (not nullable): a #SignetPolicyEngine
 * @identity: (not nullable): policy identity name
 * @client_pubkey_hex: (not nullable): client public key in hexadecimal form
 * @method: (not nullable): method name
 * @event_kind: Nostr event kind, or -1 when not applicable
 * @now: current Unix time in seconds
 * @out_result: (out) (not nullable): return location for result
 *
 * Evaluate a policy decision.
 *
 * Returns: %true if the condition is met, otherwise %false
 *
 * Since: 1.0
 */
bool signet_policy_engine_eval(SignetPolicyEngine *pe,
                               const char *identity,
                               const char *client_pubkey_hex,
                               const char *method,
                               int event_kind,
                               int64_t now,
                               SignetPolicyResult *out_result);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_POLICY_ENGINE_H */
