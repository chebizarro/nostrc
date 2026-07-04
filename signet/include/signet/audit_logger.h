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

/**
 * SignetAuditLogger:
 * Opaque structured JSON audit logger.
 *
 * Since: 1.0
 */
typedef struct SignetAuditLogger SignetAuditLogger;

/**
 * SignetAuditEventType:
 * @SIGNET_AUDIT_EVENT_UNSPECIFIED: signet audit event unspecified
 * @SIGNET_AUDIT_EVENT_STARTUP: signet audit event startup
 * @SIGNET_AUDIT_EVENT_SHUTDOWN: signet audit event shutdown
 * @SIGNET_AUDIT_EVENT_POLICY_DECISION: signet audit event policy decision
 * @SIGNET_AUDIT_EVENT_KEY_ACCESS: signet audit event key access
 * @SIGNET_AUDIT_EVENT_SIGN_REQUEST: signet audit event sign request
 * @SIGNET_AUDIT_EVENT_SIGN_RESPONSE: signet audit event sign response
 * @SIGNET_AUDIT_EVENT_REPLAY_REJECTED: signet audit event replay rejected
 * @SIGNET_AUDIT_EVENT_MGMT_APPLIED: signet audit event mgmt applied
 * @SIGNET_AUDIT_EVENT_PASSKEY_CEREMONY: signet audit event passkey ceremony
 * @SIGNET_AUDIT_EVENT_ERROR: signet audit event error
 *
 * Canonical audit event categories.
 *
 * Since: 1.0
 */
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
  SIGNET_AUDIT_EVENT_PASSKEY_CEREMONY,
  SIGNET_AUDIT_EVENT_ERROR
} SignetAuditEventType;

/* Returns a static, canonical string name for an event type. */
/**
 * signet_audit_event_type_to_string:
 * @type: event type
 *
 * Returns a static, canonical string name for an event type.
 *
 * Thread safety: access is serialized internally where required.
 *
 * Returns: (transfer none) (nullable): a borrowed pointer owned by the callee
 *
 * Since: 1.0
 */
const char *signet_audit_event_type_to_string(SignetAuditEventType type);

/**
 * SignetAuditLoggerConfig:
 * @path: If NULL/empty and to_stdout=false, logger is disabled (no-op).
 * @to_stdout: If true, write JSONL to stdout.
 * @flush_each_write: If true, fsync()/fdatasync() after each record (may be expensive).
 *
 * Configuration for structured audit-log output.
 *
 * Since: 1.0
 */
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
/**
 * signet_audit_logger_new:
 * @cfg: (nullable): configuration to use
 *
 * Create an audit logger. Returns NULL on allocation failure or on output open failure (when enabled).  If cfg->to_stdout==false and cfg->path is NULL/empty, the logger is created in "disabled" mode (writes become no-ops that return success).
 *
 * Thread safety: access is serialized internally where required.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetAuditLogger *signet_audit_logger_new(const SignetAuditLoggerConfig *cfg);

/* Close and free logger. Safe on NULL. */
/**
 * signet_audit_logger_free:
 * @l: (nullable): a #SignetAuditLogger
 *
 * Close and free logger. Safe on NULL.
 *
 * Thread safety: this function is safe to call concurrently; access is serialized internally where required.
 *
 * Since: 1.0
 */
void signet_audit_logger_free(SignetAuditLogger *l);

/* Reopen log file (for external rotation).
 * No-op for stdout-only or disabled loggers.
 * Returns 0 on success, -1 on failure. */
/**
 * signet_audit_logger_reopen:
 * @l: (not nullable): a #SignetAuditLogger
 *
 * Reopen log file (for external rotation). No-op for stdout-only or disabled loggers. Returns 0 on success, -1 on failure.
 *
 * Thread safety: this function is safe to call concurrently; access is serialized internally where required.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_audit_logger_reopen(SignetAuditLogger *l);

/**
 * signet_audit_logger_request_reopen:
 *
 * Requests that audit loggers reopen their output file on the next write.
 * This function is async-signal-safe and may be called from a signal handler.
 *
 * Thread safety: this function is safe to call concurrently.
 *
 * Since: 1.0
 */
void signet_audit_logger_request_reopen(void);
/**
 * signet_audit_logger_install_sighup_handler:
 *
 * Installs a SIGHUP handler that calls signet_audit_logger_request_reopen().
 * Subsequent audit writes notice the request and reopen the log.
 *
 * Returns: 0 on success, or -1 on failure
 *
 * Since: 1.0
 */
int signet_audit_logger_install_sighup_handler(void);

/* Write a single record:
 * - json_object MUST be a JSON object string (e.g. {"k":"v"}).
 * - The implementation wraps it into a canonical record containing timestamp
 *   and type, and appends one newline (JSONL).
 *
 * Returns 0 on success, -1 on failure.
 */
/**
 * signet_audit_log_json:
 * @l: (not nullable): a #SignetAuditLogger
 * @type: audit event type
 * @json_object: (not nullable): JSON object string
 *
 * Write a single record: - json_object MUST be a JSON object string (e.g. {"k":"v"}). - The implementation wraps it into a canonical record containing timestamp   and type, and appends one newline (JSONL).
 *
 * Thread safety: this function is safe to call concurrently; access is serialized internally where required.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_audit_log_json(SignetAuditLogger *l,
                          SignetAuditEventType type,
                          const char *json_object);

/* Common audit fields helper.
 * Any NULL fields are omitted. If event_kind < 0, it is omitted.
 */
/**
 * SignetAuditCommonFields:
 * @client_pubkey_hex: "client".
 * @identity: "identity".
 * @method: "method".
 * @event_kind: "event_kind" (omit if <0).
 * @decision: "allow" | "deny" | other (caller-defined).
 * @reason_code: stable code string; no secrets.
 *
 * Common non-secret audit fields used across records.
 *
 * Since: 1.0
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
/**
 * signet_audit_build_common_payload_json:
 * @fields: (not nullable): common audit fields
 * @details_json_object: (nullable): optional JSON object string
 *
 * Build a payload object with common fields and optional details object.  details_json_object: - NULL: no "details" field - JSON object string: included under "details"  Returned string is heap-allocated (g_malloc). Free with g_free(). Returns NULL on failure.
 *
 * Thread safety: access is serialized internally where required.
 *
 * Returns: (transfer full) (nullable): a newly allocated string, or %NULL on failure
 *
 * Since: 1.0
 */
char *signet_audit_build_common_payload_json(const SignetAuditCommonFields *fields,
                                             const char *details_json_object);

/**
 * signet_audit_log_json:
 * @l: (not nullable): a #SignetAuditLogger
 * @type: audit event type
 * @fields: (not nullable): fields
 * @details_json_object: (nullable): optional JSON object string for details
 *
 * signet audit log json.
 *
 * Thread safety: this function is safe to call concurrently; access is serialized internally where required.
 *
 * Returns: build the payload and write via result
 *
 * Since: 1.0
 */
/* Convenience helper: build the payload and write via signet_audit_log_json().
 * Returns 0 on success, -1 on failure.
 */
/**
 * signet_audit_log_common:
 * @l: (nullable): a #SignetAuditLogger
 * @type: event type
 * @fields: (not nullable): common audit fields
 * @details_json_object: (nullable): optional JSON object string
 *
 * Convenience helper: build the payload and write via signet_audit_log_json(). Returns 0 on success, -1 on failure.
 *
 * Thread safety: access is serialized internally where required.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_audit_log_common(SignetAuditLogger *l,
                            SignetAuditEventType type,
                            const SignetAuditCommonFields *fields,
                            const char *details_json_object);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_AUDIT_LOGGER_H */
