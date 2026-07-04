/* SPDX-License-Identifier: MIT
 *
 * store_leases.h - Time-bound credential lease management for Signet v2.
 */

#ifndef SIGNET_STORE_LEASES_H
#define SIGNET_STORE_LEASES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct SignetStore;

/**
 * SignetLeaseRecord:
 * @lease_id: lease identifier.
 * @secret_id: secret identifier.
 * @agent_id: agent identifier.
 * @issued_at: issue time as Unix seconds.
 * @expires_at: expiry time as Unix seconds, or 0 for no expiry.
 * @revoked_at: 0 if active.
 * @metadata: JSON string, may be NULL.
 *
 * Time-bound credential or session lease record.
 *
 * Ownership: clear instances with the corresponding *_clear() function to release heap data and wipe secrets where applicable.
 *
 * Since: 1.0
 */
typedef struct {
  char *lease_id;
  char *secret_id;
  char *agent_id;
  int64_t issued_at;
  int64_t expires_at;
  int64_t revoked_at;    /* 0 if active */
  char *metadata;         /* JSON string, may be NULL */
} SignetLeaseRecord;

/* Issue a new lease. Returns 0 on success, -1 on error. */
/**
 * signet_store_issue_lease:
 * @store: (nullable): a #SignetStore
 * @lease_id: (not nullable): lease id
 * @secret_id: (nullable): secret id
 * @agent_id: (not nullable): agent identifier
 * @issued_at: issued at
 * @expires_at: expires at
 * @metadata_json: (nullable): metadata json
 *
 * Issue a new lease. Returns 0 on success, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_issue_lease(struct SignetStore *store,
                             const char *lease_id,
                             const char *secret_id,
                             const char *agent_id,
                             int64_t issued_at,
                             int64_t expires_at,
                             const char *metadata_json);

/* Revoke a lease. Returns 0 on success, 1 if not found, -1 on error. */
/**
 * signet_store_revoke_lease:
 * @store: (nullable): a #SignetStore
 * @lease_id: (not nullable): lease id
 * @now: current Unix time in seconds
 *
 * Revoke a lease. Returns 0 on success, 1 if not found, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_revoke_lease(struct SignetStore *store,
                              const char *lease_id,
                              int64_t now);

/* Revoke all leases for an agent. Returns count revoked, -1 on error. */
/**
 * signet_store_revoke_agent_leases:
 * @store: (nullable): a #SignetStore
 * @agent_id: (not nullable): agent identifier
 * @now: current Unix time in seconds
 *
 * Revoke all leases for an agent. Returns count revoked, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_revoke_agent_leases(struct SignetStore *store,
                                     const char *agent_id,
                                     int64_t now);

/**
 * signet_lease_list_free:
 * @store: (nullable): a #SignetStore
 * @agent_id: (nullable): agent identifier
 * @now: current Unix time in seconds
 * @out_leases: (out) (transfer full) (nullable): return location for leases
 * @out_count: (out) (nullable): return location for the number of elements
 *
 * signet lease list free.
 *
 * Returns: Caller frees with result
 *
 * Since: 1.0
 */
/* List active leases for an agent. Caller frees with signet_lease_list_free().
 * Returns 0 on success, -1 on error. */
/**
 * signet_store_list_active_leases:
 * @store: (not nullable): a #SignetStore
 * @agent_id: (not nullable): agent identifier
 * @now: current Unix time in seconds
 * @out_leases: (out) (transfer full) (not nullable): return location for a lease array
 * @out_count: (out) (not nullable): number of elements
 *
 * List active leases for an agent. Caller frees with signet_lease_list_free(). Returns 0 on success, -1 on error.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_list_active_leases(struct SignetStore *store,
                                    const char *agent_id,
                                    int64_t now,
                                    SignetLeaseRecord **out_leases,
                                    size_t *out_count);

/**
 * signet_store_get_active_session_by_token:
 * @store: (not nullable): a #SignetStore
 * @session_token: (not nullable): raw session token
 * @now: current Unix time in seconds
 * @out_rec: (out) (not nullable): lease record to populate
 *
 * Looks up an active session lease by raw session token.
 * Clear @out_rec with signet_lease_record_clear() on success.
 *
 * Returns: 0 on success, 1 if no active lease matches, or -1 on error
 *
 * Since: 1.0
 */
int signet_store_get_active_session_by_token(struct SignetStore *store,
                                             const char *session_token,
                                             int64_t now,
                                             SignetLeaseRecord *out_rec);

/* Count all active leases across all agents. Returns count, -1 on error. */
/**
 * signet_store_count_active_leases:
 * @store: (nullable): a #SignetStore
 * @now: current Unix time in seconds
 *
 * Count all active leases across all agents. Returns count, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_count_active_leases(struct SignetStore *store, int64_t now);

/* Clean up expired leases older than cutoff. Returns count deleted, -1 on error. */
/**
 * signet_store_cleanup_expired_leases:
 * @store: (nullable): a #SignetStore
 * @cutoff: cutoff
 *
 * Clean up expired leases older than cutoff. Returns count deleted, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_cleanup_expired_leases(struct SignetStore *store, int64_t cutoff);

/* Free a lease record. Safe on NULL. */
/**
 * signet_lease_record_clear:
 * @rec: (nullable): rec
 *
 * Free a lease record. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_lease_record_clear(SignetLeaseRecord *rec);

/* Free a lease list. Safe on NULL. */
/**
 * signet_lease_list_free:
 * @leases: (nullable): leases
 * @count: count
 *
 * Free a lease list. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_lease_list_free(SignetLeaseRecord *leases, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_STORE_LEASES_H */
