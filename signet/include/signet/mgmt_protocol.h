/* SPDX-License-Identifier: MIT
 *
 * mgmt_protocol.h - Nostr-native management protocol for Signet.
 *
 * Management operations are signed Nostr events (no HTTP/REST).
 * The daemon consumes these events and applies mutations after verifying
 * admin signatures and replay protection.
 *
 * Event kinds (design spec):
 *   28000 - provision_agent (create new agent identity)
 *   28010 - revoke_agent (destroy agent identity)
 *   28020 - set_policy (update agent policy)
 *   28030 - get_status (health/status query)
 *   28040 - list_agents (enumerate managed agents)
 *   28050 - rotate_key (rotate agent keypair)
 *   28090 - ack (response to any management command)
 *
 * Transport: NIP-44 v2 encrypted to bunker pubkey (decrypt on receive,
 *            encrypt on ack; falls back to plaintext for backward compat).
 * Authorization: event.pubkey must be in provisioner_pubkeys list.
 */

#ifndef SIGNET_MGMT_PROTOCOL_H
#define SIGNET_MGMT_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Nostr event kinds for management protocol. */
#define SIGNET_KIND_PROVISION_AGENT 28000
#define SIGNET_KIND_REVOKE_AGENT   28010
#define SIGNET_KIND_SET_POLICY     28020
#define SIGNET_KIND_GET_STATUS     28030
#define SIGNET_KIND_LIST_AGENTS    28040
#define SIGNET_KIND_ROTATE_KEY     28050
#define SIGNET_KIND_MGMT_ACK       28090

typedef enum {
  SIGNET_MGMT_OP_UNKNOWN = 0,
  SIGNET_MGMT_OP_PROVISION_AGENT,
  SIGNET_MGMT_OP_REVOKE_AGENT,
  SIGNET_MGMT_OP_SET_POLICY,
  SIGNET_MGMT_OP_GET_STATUS,
  SIGNET_MGMT_OP_LIST_AGENTS,
  SIGNET_MGMT_OP_ROTATE_KEY,
} SignetMgmtOp;

/* Parsed management request from event content JSON. */
typedef struct {
  SignetMgmtOp op;
  int kind;                /* original event kind */

  char *agent_id;          /* target agent (owned, heap) */
  char *policy_json;       /* optional policy object JSON (owned, heap) */
  char *request_id;        /* optional correlation ID (owned, heap) */
} SignetMgmtRequest;

/* Map event kind to management op. */
SignetMgmtOp signet_mgmt_op_from_kind(int kind);

/* Map op to canonical string name. Returns static string. */
const char *signet_mgmt_op_to_string(SignetMgmtOp op);

/* Check whether event_pubkey_hex is in the provisioner pubkey list.
 * Case-insensitive hex comparison. */
bool signet_mgmt_is_authorized(const char *event_pubkey_hex,
                               const char *const *provisioner_pubkeys,
                               size_t n_provisioner_pubkeys);

/* Parse management request JSON content.
 * Expected shape depends on kind:
 *   28000: {"agent_id": "...", "request_id": "..."}
 *   28010: {"agent_id": "...", "request_id": "..."}
 *   28020: {"agent_id": "...", "policy": {...}, "request_id": "..."}
 *   28030: {"request_id": "..."}
 *   28040: {"request_id": "..."}
 *   28050: {"agent_id": "...", "request_id": "..."}
 *
 * Returns 0 on success, -1 on parse/validation error.
 * out_error receives a heap string on error (caller frees). */
int signet_mgmt_request_parse(int kind,
                              const char *content_json,
                              SignetMgmtRequest *out_req,
                              char **out_error);

/* Free/clear a parsed request. Safe on NULL. */
void signet_mgmt_request_clear(SignetMgmtRequest *req);

/* Build ack response JSON.
 * Returns heap string (caller frees with g_free) or NULL on error. */
char *signet_mgmt_build_ack(const char *request_id,
                            bool ok,
                            const char *code,
                            const char *message,
                            const char *result_json);

/* ---- Management handler (executes commands against key store) ---- */

struct SignetKeyStore;
struct SignetRelayPool;
struct SignetAuditLogger;
struct SignetPolicyStore;

typedef struct SignetMgmtHandler SignetMgmtHandler;

typedef struct {
  const char *const *provisioner_pubkeys;
  size_t n_provisioner_pubkeys;
  const char *bunker_secret_key_hex;   /* for signing ack events */
  const char *bunker_pubkey_hex;       /* for addressing */
  const char *const *relay_urls;       /* relay URLs for bunker:// URIs */
  size_t n_relay_urls;
} SignetMgmtHandlerConfig;

/* Create a management handler. */
SignetMgmtHandler *signet_mgmt_handler_new(struct SignetKeyStore *keys,
                                           struct SignetRelayPool *relays,
                                           struct SignetAuditLogger *audit,
                                           struct SignetPolicyStore *policy_store,
                                           const SignetMgmtHandlerConfig *cfg);

/* Free a management handler. Safe on NULL. */
void signet_mgmt_handler_free(SignetMgmtHandler *h);

/* Handle an incoming management event.
 * Verifies authorization, parses, executes, publishes ack.
 * Returns 0 on success (ack published), -1 on error. */
int signet_mgmt_handler_handle_event(SignetMgmtHandler *h,
                                     const char *event_pubkey_hex,
                                     const char *content_json,
                                     int kind,
                                     const char *event_id_hex,
                                     int64_t now);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_MGMT_PROTOCOL_H */