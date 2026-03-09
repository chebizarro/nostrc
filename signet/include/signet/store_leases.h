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
int signet_store_issue_lease(struct SignetStore *store,
                             const char *lease_id,
                             const char *secret_id,
                             const char *agent_id,
                             int64_t issued_at,
                             int64_t expires_at,
                             const char *metadata_json);

/* Revoke a lease. Returns 0 on success, 1 if not found, -1 on error. */
int signet_store_revoke_lease(struct SignetStore *store,
                              const char *lease_id,
                              int64_t now);

/* Revoke all leases for an agent. Returns count revoked, -1 on error. */
int signet_store_revoke_agent_leases(struct SignetStore *store,
                                     const char *agent_id,
                                     int64_t now);

/* List active leases for an agent. Caller frees with signet_lease_list_free().
 * Returns 0 on success, -1 on error. */
int signet_store_list_active_leases(struct SignetStore *store,
                                    const char *agent_id,
                                    int64_t now,
                                    SignetLeaseRecord **out_leases,
                                    size_t *out_count);

/* Count all active leases across all agents. Returns count, -1 on error. */
int signet_store_count_active_leases(struct SignetStore *store, int64_t now);

/* Clean up expired leases older than cutoff. Returns count deleted, -1 on error. */
int signet_store_cleanup_expired_leases(struct SignetStore *store, int64_t cutoff);

/* Free a lease record. Safe on NULL. */
void signet_lease_record_clear(SignetLeaseRecord *rec);

/* Free a lease list. Safe on NULL. */
void signet_lease_list_free(SignetLeaseRecord *leases, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_STORE_LEASES_H */
