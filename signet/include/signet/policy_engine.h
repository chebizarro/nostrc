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

typedef struct SignetPolicyEngine SignetPolicyEngine;

typedef enum {
  SIGNET_POLICY_DECISION_DENY = 0,
  SIGNET_POLICY_DECISION_ALLOW = 1
} SignetPolicyDecision;

typedef struct {
  SignetPolicyDecision decision;
  int64_t expires_at;        /* 0 = never */
  const char *reason_code;   /* stable string; never contains secrets */
} SignetPolicyResult;

typedef struct {
  SignetPolicyDecision default_decision; /* used when no policy entry matches */
} SignetPolicyEngineConfig;

/* Create policy engine. Returns NULL on OOM. */
SignetPolicyEngine *signet_policy_engine_new(struct SignetPolicyStore *store,
                                             struct SignetAuditLogger *audit,
                                             const SignetPolicyEngineConfig *cfg);

/* Free policy engine. Safe on NULL. */
void signet_policy_engine_free(SignetPolicyEngine *pe);

/* Evaluate a policy decision.
 *
 * identity/client/method are required.
 * event_kind may be -1 when not applicable.
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