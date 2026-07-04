/* SPDX-License-Identifier: MIT
 *
 * revocation.h - Agent revocation for Signet v2.
 *
 * Two revocation paths:
 *   Normal: Fleet Commander removes from NIP-51 list → Signet detects on
 *           next sync (≤30min), closes sessions, burns leases, adds to deny.
 *   Emergency: signet revoke --agent --reason → immediate deny, close all
 *              sessions, NIP-86 banpubkey, operator notified.
 *
 * Authorization precedence: deny list > fleet list > mint table.
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

/* Add a pubkey to the deny list.
 * reason may be NULL.
 * Returns 0 on success, -1 on error. */
/**
 * signet_deny_list_add:
 * @dl: (not nullable): a #SignetDenyList
 * @pubkey_hex: (not nullable): public key in hexadecimal form
 * @agent_id: (not nullable): agent identifier
 * @reason: (nullable): reason
 * @now: current Unix time in seconds
 *
 * Add a pubkey to the deny list. reason may be NULL. Returns 0 on success, -1 on error.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_deny_list_add(SignetDenyList *dl,
                          const char *pubkey_hex,
                          const char *agent_id,
                          const char *reason,
                          int64_t now);

/* Remove a pubkey from the deny list.
 * Returns 0 on success, 1 if not found, -1 on error. */
/**
 * signet_deny_list_remove:
 * @dl: (not nullable): a #SignetDenyList
 * @pubkey_hex: (not nullable): public key in hexadecimal form
 *
 * Remove a pubkey from the deny list. Returns 0 on success, 1 if not found, -1 on error.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_deny_list_remove(SignetDenyList *dl, const char *pubkey_hex);

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

/* Perform normal (sync-triggered) revocation:
 * Same as emergency but without notification urgency.
 * Called when agent is detected as removed from fleet list.
 * Returns 0 on success, -1 on error. */
/**
 * signet_revoke_agent_normal:
 * @store: (nullable): a #SignetStore
 * @keys: (nullable): keys
 * @deny: (nullable): deny
 * @audit: (nullable): audit
 * @agent_id: (not nullable): agent identifier
 * @pubkey_hex: (not nullable): public key in hexadecimal form
 * @now: current Unix time in seconds
 *
 * Perform normal (sync-triggered) revocation: Same as emergency but without notification urgency. Called when agent is detected as removed from fleet list. Returns 0 on success, -1 on error.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_revoke_agent_normal(struct SignetStore *store,
                                struct SignetKeyStore *keys,
                                SignetDenyList *deny,
                                struct SignetAuditLogger *audit,
                                const char *agent_id,
                                const char *pubkey_hex,
                                int64_t now);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_REVOCATION_H */
