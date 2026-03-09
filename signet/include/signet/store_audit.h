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
int signet_audit_verify_chain(struct SignetStore *store,
                              int64_t from_id,
                              int64_t to_id,
                              int64_t *out_broken_id);

/* Get the total number of audit log entries. Returns count, -1 on error. */
int64_t signet_audit_log_count(struct SignetStore *store);

/* Free an audit entry. Safe on NULL. */
void signet_audit_entry_clear(SignetAuditEntry *entry);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_STORE_AUDIT_H */
