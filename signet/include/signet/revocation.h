/* SPDX-License-Identifier: MIT
 *
 * revocation.h - Agent revocation for Signet v2.
 *
 * Management-triggered revocation updates the persistent deny list, revokes
 * leases and client bindings, and removes the agent key. Deny-list checks
 * take precedence over other authorization.
 */

#ifndef SIGNET_REVOCATION_H
#define SIGNET_REVOCATION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

struct SignetStore;
struct SignetKeyStore;
struct SignetAuditLogger;

/**
 * SignetDenyList:
 * Opaque persistent deny-list handle.
 *
 * Since: 1.0
 */
typedef struct SignetDenyList SignetDenyList;

/* Create a deny list backed by the given store. Returns NULL on OOM. */
/**
 * signet_deny_list_new:
 * @store: (not nullable): a #SignetStore
 *
 * Create a deny list backed by the given store. Returns NULL on OOM.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetDenyList *signet_deny_list_new(struct SignetStore *store);

/* Free a deny list. Safe on NULL. */
/**
 * signet_deny_list_free:
 * @dl: (nullable): a #SignetDenyList
 *
 * Free a deny list. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_deny_list_free(SignetDenyList *dl);

/* Check if a pubkey is denied. Returns true if in deny list. */
/**
 * signet_deny_list_contains:
 * @dl: (not nullable): a #SignetDenyList
 * @pubkey_hex: (not nullable): public key in hexadecimal form
 *
 * Check if a pubkey is denied. Returns true if in deny list.
 *
 * Returns: %true if the condition is met, otherwise %false
 *
 * Since: 1.0
 */
bool signet_deny_list_contains(SignetDenyList *dl, const char *pubkey_hex);

/* Perform emergency revocation of an agent:
 * 1. Add pubkey to deny list
 * 2. Revoke all leases for the agent
 * 3. Revoke agent key from key store
 * 4. Audit log the revocation
 *
 * Returns 0 on success, -1 on error. */
/**
 * signet_revoke_agent:
 * @store: (nullable): a #SignetStore
 * @keys: (nullable): keys
 * @deny: (nullable): deny
 * @audit: (nullable): audit
 * @agent_id: (not nullable): agent identifier
 * @pubkey_hex: (not nullable): public key in hexadecimal form
 * @reason: (nullable): reason
 * @now: current Unix time in seconds
 *
 * Perform emergency revocation of an agent: 1. Add pubkey to deny list 2. Revoke all leases for the agent 3. Revoke agent key from key store 4. Audit log the revocation.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_revoke_agent(struct SignetStore *store,
                         struct SignetKeyStore *keys,
                         SignetDenyList *deny,
                         struct SignetAuditLogger *audit,
                         const char *agent_id,
                         const char *pubkey_hex,
                         const char *reason,
                         int64_t now);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_REVOCATION_H */
