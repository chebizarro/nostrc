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
/**
 * SIGNET_CAP_NOSTR_SIGN:
 *
 * Capability string allowing Nostr event signing.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_NOSTR_SIGN          "nostr.sign"
/**
 * SIGNET_CAP_NOSTR_ENCRYPT:
 *
 * Capability string allowing Nostr encryption operations.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_NOSTR_ENCRYPT       "nostr.encrypt"
/**
 * SIGNET_CAP_CREDENTIAL_GET_TOKEN:
 *
 * Capability string allowing credential token retrieval.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_CREDENTIAL_GET_TOKEN   "credential.get_token"
/**
 * SIGNET_CAP_CREDENTIAL_GET_SESSION:
 *
 * Capability string allowing credential-to-session brokering.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_CREDENTIAL_GET_SESSION "credential.get_session"
/**
 * SIGNET_CAP_SSH_SIGN:
 *
 * Capability string allowing SSH signature generation.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_SSH_SIGN            "ssh.sign"
/**
 * SIGNET_CAP_SSH_LIST_KEYS:
 *
 * Capability string allowing SSH public-key enumeration.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_SSH_LIST_KEYS       "ssh.list_keys"
/**
 * SIGNET_CAP_SIGNET_MINT:
 *
 * Capability string allowing Signet token or identity minting.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_SIGNET_MINT         "signet.mint"
/**
 * SIGNET_CAP_SIGNET_REISSUE:
 *
 * Capability string allowing bootstrap token reissue.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_SIGNET_REISSUE      "signet.reissue_token"
/**
 * SIGNET_CAP_SIGNET_REVOKE:
 *
 * Capability string allowing agent revocation.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_SIGNET_REVOKE       "signet.revoke"
/**
 * SIGNET_CAP_RELAY_MANAGE:
 *
 * Capability string allowing relay management.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_RELAY_MANAGE        "relay.manage"
/**
 * SIGNET_CAP_PASSKEY_GET_INFO:
 *
 * Capability string allowing FIDO authenticator information queries.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_PASSKEY_GET_INFO    "passkey.get_info"
/**
 * SIGNET_CAP_PASSKEY_MAKE_CREDENTIAL:
 *
 * Capability string allowing passkey creation.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_PASSKEY_MAKE_CREDENTIAL "passkey.make_credential"
/**
 * SIGNET_CAP_PASSKEY_GET_ASSERTION:
 *
 * Capability string allowing passkey assertions.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_PASSKEY_GET_ASSERTION   "passkey.get_assertion"
/**
 * SIGNET_CAP_PASSKEY_EXPORT:
 *
 * Capability string allowing passkey export.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_PASSKEY_EXPORT          "passkey.export"
/**
 * SIGNET_CAP_PASSKEY_IMPORT:
 *
 * Capability string allowing passkey import.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_PASSKEY_IMPORT          "passkey.import"
/**
 * SIGNET_CAP_PASSKEY_MANAGE:
 *
 * Capability string allowing passkey management operations.
 *
 * Since: 1.0
 */
#define SIGNET_CAP_PASSKEY_MANAGE      "passkey.manage"

/* Agent policy: defines what an agent is authorized to do. */
/**
 * SignetAgentPolicy:
 * @name: policy name (e.g., "network-manager").
 * @capabilities: NULL-terminated array of capability strings.
 * @n_capabilities: n capabilities value.
 * @allowed_event_kinds: array of allowed event kinds for nostr.sign.
 * @n_allowed_kinds: n allowed kinds value.
 * @disallowed_credential_types: types the agent cannot access.
 * @n_disallowed_types: n disallowed types value.
 * @rate_limit_per_hour: 0 = unlimited.
 *
 * Capability policy assigned to one or more agents.
 *
 * Since: 1.0
 */
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
/**
 * SignetPolicyRegistry:
 * Opaque registry mapping agents to named policies.
 *
 * Since: 1.0
 */
typedef struct SignetPolicyRegistry SignetPolicyRegistry;

/* Create a policy registry. Returns NULL on OOM. */
/**
 * signet_policy_registry_new:
 *
 * Create a policy registry. Returns NULL on OOM.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetPolicyRegistry *signet_policy_registry_new(void);

/* Free a policy registry. Safe on NULL. */
/**
 * signet_policy_registry_free:
 * @pr: (nullable): a #SignetPolicyRegistry
 *
 * Free a policy registry. Safe on NULL.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Since: 1.0
 */
void signet_policy_registry_free(SignetPolicyRegistry *pr);

/* Register a named policy. Returns 0 on success. */
/**
 * signet_policy_registry_add:
 * @pr: (not nullable): a #SignetPolicyRegistry
 * @policy: (nullable): policy
 *
 * Register a named policy. Returns 0 on success.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_policy_registry_add(SignetPolicyRegistry *pr,
                                const SignetAgentPolicy *policy);

/* Assign a named policy to an agent. Returns 0 on success. */
/**
 * signet_policy_registry_assign:
 * @pr: (not nullable): a #SignetPolicyRegistry
 * @agent_id: (not nullable): agent identifier
 * @policy_name: (not nullable): policy name
 *
 * Assign a named policy to an agent. Returns 0 on success.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_policy_registry_assign(SignetPolicyRegistry *pr,
                                   const char *agent_id,
                                   const char *policy_name);

/* Check if an agent has a specific capability.
 * Returns true if the agent's policy grants the capability. */
/**
 * signet_policy_has_capability:
 * @pr: (not nullable): a #SignetPolicyRegistry
 * @agent_id: (not nullable): agent identifier
 * @capability: (not nullable): capability
 *
 * Check if an agent has a specific capability. Returns true if the agent's policy grants the capability.
 *
 * Returns: %true if the condition is met, otherwise %false
 *
 * Since: 1.0
 */
bool signet_policy_has_capability(SignetPolicyRegistry *pr,
                                   const char *agent_id,
                                   const char *capability);

/* Check if an agent is allowed to sign a specific event kind.
 * Returns true if allowed (or if no kind restrictions). */
/**
 * signet_policy_allowed_kind:
 * @pr: (not nullable): a #SignetPolicyRegistry
 * @agent_id: (not nullable): agent identifier
 * @event_kind: Nostr event kind, or -1 when not applicable
 *
 * Check if an agent is allowed to sign a specific event kind. Returns true if allowed (or if no kind restrictions).
 *
 * Returns: %true if the condition is met, otherwise %false
 *
 * Since: 1.0
 */
bool signet_policy_allowed_kind(SignetPolicyRegistry *pr,
                                 const char *agent_id,
                                 int event_kind);

/* Check rate limit for (agent_id, capability).
 * Returns true if the request is allowed, false if rate-limited.
 * Uses libnostr's token bucket internally. */
/**
 * signet_policy_rate_limit_check:
 * @pr: (not nullable): a #SignetPolicyRegistry
 * @agent_id: (not nullable): agent identifier
 * @capability: (not nullable): capability
 *
 * Check rate limit for (agent_id, capability). Returns true if the request is allowed, false if rate-limited. Uses libnostr's token bucket internally.
 *
 * Returns: %true if the condition is met, otherwise %false
 *
 * Since: 1.0
 */
bool signet_policy_rate_limit_check(SignetPolicyRegistry *pr,
                                     const char *agent_id,
                                     const char *capability);

/* Evaluate a method call against the full policy.
 * Combines capability check, kind check, and rate limiting.
 * method: NIP-46 method name (maps to capability internally).
 * Returns true if allowed. */
/**
 * signet_policy_evaluate:
 * @pr: (not nullable): a #SignetPolicyRegistry
 * @agent_id: (not nullable): agent identifier
 * @method: (not nullable): method name
 * @event_kind: Nostr event kind, or -1 when not applicable
 *
 * Evaluate a method call against the full policy. Combines capability check, kind check, and rate limiting. method: NIP-46 method name (maps to capability internally). Returns true if allowed.
 *
 * Returns: %true if the condition is met, otherwise %false
 *
 * Since: 1.0
 */
bool signet_policy_evaluate(SignetPolicyRegistry *pr,
                             const char *agent_id,
                             const char *method,
                             int event_kind);

/* Map a NIP-46/D-Bus method name to a capability string.
 * Returns static string or NULL if unknown. */
/**
 * signet_method_to_capability:
 * @method: (not nullable): method name
 *
 * Map a NIP-46/D-Bus method name to a capability string. Returns static string or NULL if unknown.
 *
 * Returns: (transfer none) (nullable): a borrowed pointer owned by the callee
 *
 * Since: 1.0
 */
const char *signet_method_to_capability(const char *method);

/* Free a policy struct's contents. Does NOT free the struct itself. */
/**
 * signet_agent_policy_clear:
 * @policy: (nullable): policy
 *
 * Free a policy struct's contents. Does NOT free the struct itself.
 *
 * Since: 1.0
 */
void signet_agent_policy_clear(SignetAgentPolicy *policy);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_CAPABILITY_H */
