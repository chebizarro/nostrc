/* SPDX-License-Identifier: MIT
 *
 * audit_logger.h - Structured audit logging for Signet.
 *
 * Signet treats audit logs as security artifacts: append-only JSON Lines records
 * capturing policy decisions, signing operations, key access, and replay rejections.
 *
 * Phase 2:
 * - Thread-safe logger (internal mutex)
 * - Atomic append (single write per record, O_APPEND)
 * - SIGHUP-triggered reopen support for external log rotation
 * - Helpers to build consistent JSON payloads for common audit fields
 *
 * IMPORTANT:
 * - Never log secret material (keys, tokens, ciphertext plaintext, etc.)
 * - The logger does not attempt to redact; callers must ensure payloads are safe.
 */

#ifndef SIGNET_AUDIT_LOGGER_H
#define SIGNET_AUDIT_LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef struct SignetAuditLogger SignetAuditLogger;

typedef enum {
  SIGNET_AUDIT_EVENT_UNSPECIFIED = 0,
  SIGNET_AUDIT_EVENT_STARTUP,
  SIGNET_AUDIT_EVENT_SHUTDOWN,
  SIGNET_AUDIT_EVENT_POLICY_DECISION,
  SIGNET_AUDIT_EVENT_KEY_ACCESS,
  SIGNET_AUDIT_EVENT_SIGN_REQUEST,
  SIGNET_AUDIT_EVENT_SIGN_RESPONSE,
  SIGNET_AUDIT_EVENT_REPLAY_REJECTED,
  SIGNET_AUDIT_EVENT_MGMT_APPLIED,
  SIGNET_AUDIT_EVENT_ERROR
} SignetAuditEventType;

/* Returns a static, canonical string name for an event type. */
const char *signet_audit_event_type_to_string(SignetAuditEventType type);

typedef struct {
  const char *path;      /* If NULL/empty and to_stdout=false, logger is disabled (no-op). */
  bool to_stdout;        /* If true, write JSONL to stdout. */
  bool flush_each_write; /* If true, fsync()/fdatasync() after each record (may be expensive). */
} SignetAuditLoggerConfig;

/* Create an audit logger.
 * Returns NULL on allocation failure or on output open failure (when enabled).
 *
 * If cfg->to_stdout==false and cfg->path is NULL/empty, the logger is created
 * in "disabled" mode (writes become no-ops that return success).
 */
SignetAuditLogger *signet_audit_logger_new(const SignetAuditLoggerConfig *cfg);

/* Close and free logger. Safe on NULL. */
void signet_audit_logger_free(SignetAuditLogger *l);

/* Reopen log file (for external rotation).
 * No-op for stdout-only or disabled loggers.
 * Returns 0 on success, -1 on failure. */
int signet_audit_logger_reopen(SignetAuditLogger *l);

/* SIGHUP integration:
 * - signet_audit_logger_request_reopen() is async-signal-safe and may be called
 *   from a signal handler.
 * - signet_audit_logger_install_sighup_handler() installs a SIGHUP handler that
 *   calls signet_audit_logger_request_reopen().
 *
 * Any subsequent signet_audit_* write will notice the request and reopen the log.
 */
void signet_audit_logger_request_reopen(void);
int signet_audit_logger_install_sighup_handler(void);

/* Write a single record:
 * - json_object MUST be a JSON object string (e.g. {"k":"v"}).
 * - The implementation wraps it into a canonical record containing timestamp
 *   and type, and appends one newline (JSONL).
 *
 * Returns 0 on success, -1 on failure.
 */
int signet_audit_log_json(SignetAuditLogger *l,
                          SignetAuditEventType type,
                          const char *json_object);

/* Common audit fields helper.
 * Any NULL fields are omitted. If event_kind < 0, it is omitted.
 */
typedef struct {
  const char *client_pubkey_hex; /* "client" */
  const char *identity;          /* "identity" */
  const char *method;            /* "method" */
  int event_kind;                /* "event_kind" (omit if <0) */

  /* Decision fields for policy/security events */
  const char *decision;          /* "allow" | "deny" | other (caller-defined) */
  const char *reason_code;       /* stable code string; no secrets */
} SignetAuditCommonFields;

/* Build a payload object with common fields and optional details object.
 *
 * details_json_object:
 * - NULL: no "details" field
 * - JSON object string: included under "details"
 *
 * Returned string is heap-allocated (g_malloc). Free with g_free().
 * Returns NULL on failure.
 */
char *signet_audit_build_common_payload_json(const SignetAuditCommonFields *fields,
                                             const char *details_json_object);

/* Convenience helper: build the payload and write via signet_audit_log_json().
 * Returns 0 on success, -1 on failure.
 */
int signet_audit_log_common(SignetAuditLogger *l,
                            SignetAuditEventType type,
                            const SignetAuditCommonFields *fields,
                            const char *details_json_object);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_AUDIT_LOGGER_H */