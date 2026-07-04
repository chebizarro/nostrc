/* SPDX-License-Identifier: MIT
 *
 * store_audit.h - Hash-chained audit log for Signet v2.
 *
 * Each entry includes prev_hash (entry_hash of previous row) and
 * entry_hash = SHA256(ts || agent_id || operation || detail || prev_hash).
 * Chain integrity is verifiable offline via signet_audit_verify_chain().
 */

#ifndef SIGNET_STORE_AUDIT_H
#define SIGNET_STORE_AUDIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct SignetStore;

/**
 * SignetAuditEntry:
 * @id: record identifier.
 * @ts: ts value.
 * @agent_id: agent identifier.
 * @operation: operation value.
 * @secret_id: secret identifier.
 * @transport: transport value.
 * @detail: detail value.
 * @prev_hash: prev hash value.
 * @entry_hash: entry hash value.
 *
 * Hash-chained audit entry stored in the persistent audit table.
 *
 * Ownership: clear instances with the corresponding *_clear() function to release heap data and wipe secrets where applicable.
 *
 * Since: 1.0
 */
typedef struct {
  int64_t id;
  int64_t ts;
  char *agent_id;
  char *operation;
  char *secret_id;
  char *transport;
  char *detail;
  char *prev_hash;
  char *entry_hash;
} SignetAuditEntry;

/* Append an audit log entry. Computes entry_hash from fields + prev chain.
 * Returns 0 on success, -1 on error. */
/**
 * signet_audit_log_append:
 * @store: (nullable): a #SignetStore
 * @ts: ts
 * @agent_id: (not nullable): agent identifier
 * @operation: (not nullable): operation
 * @secret_id: (nullable): secret id
 * @transport: (nullable): transport
 * @detail_json: (nullable): detail json
 *
 * Append an audit log entry. Computes entry_hash from fields + prev chain. Returns 0 on success, -1 on error.
 *
 * Thread safety: this function is safe to call concurrently; access is serialized internally where required.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_audit_log_append(struct SignetStore *store,
                            int64_t ts,
                            const char *agent_id,
                            const char *operation,
                            const char *secret_id,
                            const char *transport,
                            const char *detail_json);

/* Verify the hash chain integrity from entry 'from_id' to 'to_id'.
 * If to_id is 0, verifies to the last entry.
 * Returns 0 if chain is intact, 1 if broken (sets *out_broken_id), -1 on error. */
/**
 * signet_audit_verify_chain:
 * @store: (nullable): a #SignetStore
 * @from_id: from id
 * @to_id: to id
 * @out_broken_id: (out) (not nullable): return location for broken id
 *
 * Verify the hash chain integrity from entry 'from_id' to 'to_id'. If to_id is 0, verifies to the last entry. Returns 0 if chain is intact, 1 if broken (sets *out_broken_id), -1 on error.
 *
 * Thread safety: this function is safe to call concurrently; access is serialized internally where required.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_audit_verify_chain(struct SignetStore *store,
                              int64_t from_id,
                              int64_t to_id,
                              int64_t *out_broken_id);

/* Get the total number of audit log entries. Returns count, -1 on error. */
/**
 * signet_audit_log_count:
 * @store: (nullable): a #SignetStore
 *
 * Get the total number of audit log entries. Returns count, -1 on error.
 *
 * Thread safety: this function is safe to call concurrently; access is serialized internally where required.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int64_t signet_audit_log_count(struct SignetStore *store);

/* Free an audit entry. Safe on NULL. */
/**
 * signet_audit_entry_clear:
 * @entry: (nullable): entry
 *
 * Free an audit entry. Safe on NULL.
 *
 * Thread safety: this function is safe to call concurrently; access is serialized internally where required.
 *
 * Since: 1.0
 */
void signet_audit_entry_clear(SignetAuditEntry *entry);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_STORE_AUDIT_H */
