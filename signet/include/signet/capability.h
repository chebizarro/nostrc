/* SPDX-License-Identifier: MIT
 *
 * capability.h - Capability-based policy model for Signet v2.
 *
 * Capabilities are explicit grants; anything not listed is denied.
 * Rate limiting uses the token bucket from libnostr's rate_limiter.h.
 */

#ifndef SIGNET_CAPABILITY_H
#define SIGNET_CAPABILITY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Well-known capability strings. */
#define SIGNET_CAP_NOSTR_SIGN          "nostr.sign"
#define SIGNET_CAP_NOSTR_ENCRYPT       "nostr.encrypt"
#define SIGNET_CAP_CREDENTIAL_GET_TOKEN   "credential.get_token"
#define SIGNET_CAP_CREDENTIAL_GET_SESSION "credential.get_session"
#define SIGNET_CAP_SSH_SIGN            "ssh.sign"
#define SIGNET_CAP_SSH_LIST_KEYS       "ssh.list_keys"
#define SIGNET_CAP_SIGNET_MINT         "signet.mint"
#define SIGNET_CAP_SIGNET_REISSUE      "signet.reissue_token"
#define SIGNET_CAP_SIGNET_REVOKE       "signet.revoke"
#define SIGNET_CAP_RELAY_MANAGE        "relay.manage"

/* Agent policy: defines what an agent is authorized to do. */
typedef struct {
  char *name;                 /* policy name (e.g., "network-manager") */
  char **capabilities;        /* NULL-terminated array of capability strings */
  size_t n_capabilities;
  int *allowed_event_kinds;   /* array of allowed event kinds for nostr.sign */
  size_t n_allowed_kinds;
  char **disallowed_credential_types; /* types the agent cannot access */
  size_t n_disallowed_types;
  uint32_t rate_limit_per_hour;       /* 0 = unlimited */
} SignetAgentPolicy;

/* Policy registry — maps agent_id → policy. */
typedef struct SignetPolicyRegistry SignetPolicyRegistry;

/* Create a policy registry. Returns NULL on OOM. */
SignetPolicyRegistry *signet_policy_registry_new(void);

/* Free a policy registry. Safe on NULL. */
void signet_policy_registry_free(SignetPolicyRegistry *pr);

/* Register a named policy. Returns 0 on success. */
int signet_policy_registry_add(SignetPolicyRegistry *pr,
                                const SignetAgentPolicy *policy);

/* Assign a named policy to an agent. Returns 0 on success. */
int signet_policy_registry_assign(SignetPolicyRegistry *pr,
                                   const char *agent_id,
                                   const char *policy_name);

/* Check if an agent has a specific capability.
 * Returns true if the agent's policy grants the capability. */
bool signet_policy_has_capability(SignetPolicyRegistry *pr,
                                   const char *agent_id,
                                   const char *capability);

/* Check if an agent is allowed to sign a specific event kind.
 * Returns true if allowed (or if no kind restrictions). */
bool signet_policy_allowed_kind(SignetPolicyRegistry *pr,
                                 const char *agent_id,
                                 int event_kind);

/* Check rate limit for (agent_id, capability).
 * Returns true if the request is allowed, false if rate-limited.
 * Uses libnostr's token bucket internally. */
bool signet_policy_rate_limit_check(SignetPolicyRegistry *pr,
                                     const char *agent_id,
                                     const char *capability);

/* Evaluate a method call against the full policy.
 * Combines capability check, kind check, and rate limiting.
 * method: NIP-46 method name (maps to capability internally).
 * Returns true if allowed. */
bool signet_policy_evaluate(SignetPolicyRegistry *pr,
                             const char *agent_id,
                             const char *method,
                             int event_kind);

/* Map a NIP-46/D-Bus method name to a capability string.
 * Returns static string or NULL if unknown. */
const char *signet_method_to_capability(const char *method);

/* Free a policy struct's contents. Does NOT free the struct itself. */
void signet_agent_policy_clear(SignetAgentPolicy *policy);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_CAPABILITY_H */
