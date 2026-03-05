/* SPDX-License-Identifier: MIT
 *
 * mgmt_protocol.h - Signed Nostr management protocol for Signet.
 *
 * Signet management operations are issued as signed Nostr events (no HTTP/REST
 * control plane). The daemon consumes management events and applies mutations
 * (policy updates, revocations) after verifying admin signatures and replay
 * protection.
 *
 * This file defines the management command *content* schema and helpers for:
 * - parsing/validating management command JSON (event content)
 * - admin authorization checks against a configured admin pubkey list
 * - building structured response JSON (event content)
 *
 * NOTE:
 * - NIP-01 event signature verification is performed by the daemon's event
 *   ingestion layer (libnostr event verify). This module assumes the caller
 *   provides the event pubkey and content JSON, and separately enforces that
 *   the event signature is valid before executing state changes.
 */

#ifndef SIGNET_MGMT_PROTOCOL_H
#define SIGNET_MGMT_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIGNET_KIND_MGMT_REQ 37000
#define SIGNET_KIND_MGMT_ACK 37001

typedef enum {
  SIGNET_MGMT_OP_UNKNOWN = 0,

  /* Legacy op strings (kept for compatibility with earlier scaffolding). */
  SIGNET_MGMT_OP_POLICY_SET,
  SIGNET_MGMT_OP_POLICY_UNSET,
  SIGNET_MGMT_OP_CLIENT_REVOKE,
  SIGNET_MGMT_OP_CLIENT_UNREVOKE,

  /* Phase 5 command set (preferred). */
  SIGNET_MGMT_OP_ADD_POLICY,
  SIGNET_MGMT_OP_REVOKE_POLICY,
  SIGNET_MGMT_OP_LIST_POLICIES,
  SIGNET_MGMT_OP_ROTATE_KEY,
  SIGNET_MGMT_OP_HEALTH_CHECK
} SignetMgmtOp;

/* Parsed management request from event content JSON. */
typedef struct {
  SignetMgmtOp op;

  /* Canonical command string (owned, heap). */
  char *command;

  /* Optional identity string (owned, heap). */
  char *identity;

  /* Optional policy object serialized as compact JSON (owned, heap). */
  char *policy_json;
} SignetMgmtRequest;

/* Parse an op/command string into an enum value.
 * Accepts both legacy ("policy.set") and preferred ("add_policy") forms. */
SignetMgmtOp signet_mgmt_op_from_string(const char *op);

/* Convert op to canonical (preferred) string. Returns a static string. */
const char *signet_mgmt_op_to_string(SignetMgmtOp op);

/* Check whether event_pubkey_hex is in the configured admin list.
 * Comparison is case-insensitive and expects 64-hex pubkeys (recommended),
 * but will do best-effort string compare for other forms.
 */
bool signet_mgmt_admin_is_authorized(const char *event_pubkey_hex,
                                     const char *const *admin_pubkeys,
                                     size_t n_admin_pubkeys);

/* Parse and validate management request JSON (event content).
 *
 * Expected content shape:
 * {
 *   "command": "<string>",
 *   "identity": "<string>",   // required for add_policy/revoke_policy/rotate_key
 *   "policy": { ... }         // required for add_policy
 * }
 *
 * Returns 0 on success, -1 on parse/validation error.
 * If out_error is non-NULL, a heap string is returned on error (caller frees).
 */
int signet_mgmt_request_parse_content_json(const char *content_json,
                                          SignetMgmtRequest *out_req,
                                          char **out_error);

/* Free/clear a parsed request. Safe on NULL/empty. */
void signet_mgmt_request_clear(SignetMgmtRequest *req);

/* Build a structured response JSON object (event content).
 *
 * status: "ok" or "error"
 * command: echoed command string (required)
 * code/message: optional diagnostic fields
 * result_json: optional JSON value (object/array/string/number/bool/null) to place under "result"
 *
 * Returns heap string (caller frees) or NULL on OOM/parse error.
 */
char *signet_mgmt_build_response_json(const char *status,
                                      const char *command,
                                      const char *code,
                                      const char *message,
                                      const char *result_json);

/* Convenience: Build a minimal ack payload (legacy helper).
 * Returned string is heap allocated; caller frees. */
char *signet_mgmt_build_ack_json(bool ok, const char *code, const char *message);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_MGMT_PROTOCOL_H */