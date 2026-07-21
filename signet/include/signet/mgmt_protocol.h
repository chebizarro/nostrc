/* SPDX-License-Identifier: MIT
 *
 * mgmt_protocol.h - Nostr-native management protocol for Signet.
 *
 * Management operations are Cascadia ContextVM kind-25910 JSON-RPC intents
 * (no HTTP/REST). The daemon consumes these events and applies mutations
 * after verifying admin signatures and replay protection.
 *
 * Canonical methods:
 *   agent/provision   - create new agent identity
 *   agent/revoke      - destroy agent identity
 *   agent/set-policy  - update agent policy
 *   agent/get-status  - health/status query
 *   agent/list        - enumerate managed agents
 *   agent/rotate-key  - rotate agent keypair
 *   agent/reissue-connect - mint a fresh one-time connect_secret for an
 *                       existing agent (restart recovery; ContextVM-only)
 *   agent/list-clients  - list an agent's persistent NIP-46 client bindings
 *   agent/revoke-client - soft-revoke a persistent NIP-46 client binding
 *
 * Relay transport: NIP-59/NIP-17 gift-wrap kind 1059 carrying the inner
 *                  kind-25910 intent.
 * Authorization: inner sender pubkey must be in provisioner_pubkeys list.
 *                Exception: agent/reissue-connect is also authorized when the
 *                sender IS the target agent (sender pubkey equals the agent's
 *                identity pubkey) — self-service connect_secret recovery. The
 *                self path grants no power over any other agent or method.
 *
 * No legacy management event transport is supported. Operations are keyed by
 * their ContextVM method and replies are NIP-59 gift-wrapped JSON-RPC results.
 */

#ifndef SIGNET_MGMT_PROTOCOL_H
#define SIGNET_MGMT_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "signet/cascadia.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Cascadia canonical management plane compatibility aliases. */
#define SIGNET_KIND_CONTEXTVM_INTENT CAS_INTENT
#define SIGNET_KIND_NIP59_GIFT_WRAP NIP59_GIFT_WRAP

/**
 * SignetMgmtOp:
 * @SIGNET_MGMT_OP_UNKNOWN: signet mgmt op unknown
 * @SIGNET_MGMT_OP_PROVISION_AGENT: signet mgmt op provision agent
 * @SIGNET_MGMT_OP_REVOKE_AGENT: signet mgmt op revoke agent
 * @SIGNET_MGMT_OP_SET_POLICY: signet mgmt op set policy
 * @SIGNET_MGMT_OP_GET_STATUS: signet mgmt op get status
 * @SIGNET_MGMT_OP_LIST_AGENTS: signet mgmt op list agents
 * @SIGNET_MGMT_OP_ROTATE_KEY: signet mgmt op rotate key
 *
 * Supported Nostr-native management operations.
 *
 * Since: 1.0
 */
typedef enum {
  SIGNET_MGMT_OP_UNKNOWN = 0,
  SIGNET_MGMT_OP_PROVISION_AGENT,
  SIGNET_MGMT_OP_REVOKE_AGENT,
  SIGNET_MGMT_OP_SET_POLICY,
  SIGNET_MGMT_OP_GET_STATUS,
  SIGNET_MGMT_OP_LIST_AGENTS,
  SIGNET_MGMT_OP_ROTATE_KEY,
  SIGNET_MGMT_OP_ADOPT_EXISTING,
  SIGNET_MGMT_OP_REISSUE_CONNECT,
  SIGNET_MGMT_OP_LIST_CLIENTS,
  SIGNET_MGMT_OP_REVOKE_CLIENT,
} SignetMgmtOp;

/* Parsed management request from event content JSON. */
/**
 * SignetMgmtRequest:
 * @op: op value.
 * @kind: original event kind.
 * @agent_id: target agent (owned, heap).
 * @policy_json: optional policy object JSON (owned, heap).
 * @request_id: optional correlation ID (owned, heap).
 *
 * Parsed management request content.
 *
 * Since: 1.0
 */
typedef struct {
  SignetMgmtOp op;
  char *agent_id;          /* target agent (owned, heap) */
  char *policy_json;       /* optional policy object JSON (owned, heap) */
  char *request_id;        /* optional correlation ID (owned, heap) */
  char *bootstrap_pubkey;  /* optional delivery bootstrap pubkey */
  bool deliver;            /* provision: send bunker handoff via NIP-17 */
  int64_t delivery_ttl;    /* seconds; capped by handler */
  char *agent_nsec;        /* adopt: supplied secret (nsec or 64-hex) — sensitive, owned */
  char *expected_pubkey;   /* adopt: require derived pubkey to match (owned) */
  char *connect_secret;    /* adopt: optional fixed connect secret (owned) */
  char *client_pubkey;     /* revoke-client: target NIP-46 client pubkey (owned) */
} SignetMgmtRequest;

/* Map op to canonical string name. Returns static string. */
/**
 * signet_mgmt_op_to_string:
 * @op: management operation
 *
 * Map op to canonical string name. Returns static string.
 *
 * Returns: (transfer none) (nullable): a borrowed pointer owned by the callee
 *
 * Since: 1.0
 */
const char *signet_mgmt_op_to_string(SignetMgmtOp op);

/* Check whether event_pubkey_hex is in the provisioner pubkey list.
 * Case-insensitive hex comparison. */
/**
 * signet_mgmt_is_authorized:
 * @event_pubkey_hex: (not nullable): event pubkey hex
 * @provisioner_pubkeys: (not nullable): provisioner pubkeys
 * @n_provisioner_pubkeys: number of elements
 *
 * Check whether event_pubkey_hex is in the provisioner pubkey list. Case-insensitive hex comparison.
 *
 * Returns: %true if the condition is met, otherwise %false
 *
 * Since: 1.0
 */
bool signet_mgmt_is_authorized(const char *event_pubkey_hex,
                               const char *const *provisioner_pubkeys,
                               size_t n_provisioner_pubkeys);

/* Parse normalized ContextVM method parameters.
 * The operation comes from the validated JSON-RPC method name; content_json
 * is the normalized params object plus request_id correlation.
 *
 * Returns 0 on success, -1 on parse/validation error.
 * out_error receives a heap string on error (caller frees). */
/**
 * signet_mgmt_request_parse:
 * @op: validated ContextVM management operation
 * @content_json: (not nullable): content json
 * @out_req: (out) (not nullable): return location for req
 * @out_error: (out) (transfer full) (nullable): return location for a newly allocated error string
 *
 * Parse normalized ContextVM management parameters.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_mgmt_request_parse(SignetMgmtOp op,
                              const char *content_json,
                              SignetMgmtRequest *out_req,
                              char **out_error);

/* Free/clear a parsed request. Safe on NULL. */
/**
 * signet_mgmt_request_clear:
 * @req: (nullable): request data
 *
 * Free/clear a parsed request. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_mgmt_request_clear(SignetMgmtRequest *req);

/* Build ack response JSON.
 * Returns heap string (caller frees with g_free) or NULL on error. */
/**
 * signet_mgmt_build_ack:
 * @request_id: (nullable): request correlation identifier
 * @ok: whether the command succeeded
 * @code: (nullable): machine-readable result code
 * @message: (nullable): human-readable message
 * @result_json: (nullable): optional result JSON object
 *
 * Build ack response JSON. Returns heap string (caller frees with g_free) or NULL on error.
 *
 * Returns: (transfer full) (nullable): a newly allocated string, or %NULL on failure
 *
 * Since: 1.0
 */
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
struct SignetDenyList;
struct SignetReplayCache;

/**
 * SignetMgmtHandler:
 * Opaque management-event executor.
 *
 * Since: 1.0
 */
typedef struct SignetMgmtHandler SignetMgmtHandler;

/**
 * SignetMgmtHandlerConfig:
 * @provisioner_pubkeys: provisioner pubkeys value.
 * @n_provisioner_pubkeys: n provisioner pubkeys value.
 * @bunker_secret_key_hex: for signing ack events.
 * @bunker_pubkey_hex: for addressing.
 * @relay_urls: relay URLs for bunker:// URIs.
 * @n_relay_urls: n relay urls value.
 *
 * Configuration for management command authorization and acknowledgements.
 *
 * Since: 1.0
 */
typedef struct {
  const char *const *provisioner_pubkeys;
  size_t n_provisioner_pubkeys;
  const char *bunker_secret_key_hex;   /* for signing ack events */
  const char *bunker_pubkey_hex;       /* for addressing */
  const char *const *relay_urls;       /* relay URLs for bunker:// URIs */
  size_t n_relay_urls;
} SignetMgmtHandlerConfig;

/* Create a management handler. */
/**
 * signet_mgmt_handler_new:
 * @keys: (not nullable): keys
 * @relays: (not nullable): relays
 * @audit: (not nullable): audit
 * @policy_store: (not nullable): policy store
 * @cfg: (nullable): configuration to use
 *
 * Create a management handler.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetMgmtHandler *signet_mgmt_handler_new(struct SignetKeyStore *keys,
                                           struct SignetRelayPool *relays,
                                           struct SignetAuditLogger *audit,
                                           struct SignetPolicyStore *policy_store,
                                           const SignetMgmtHandlerConfig *cfg);

/* Free a management handler. Safe on NULL. */
/**
 * signet_mgmt_handler_free:
 * @h: (nullable): a #SignetMgmtHandler
 *
 * Free a management handler. Safe on NULL.
 *
 * Since: 1.0
 */
void signet_mgmt_handler_free(SignetMgmtHandler *h);

/* Attach the daemon's live deny list so that revoke_agent can add revoked
 * pubkeys to it (and so deny-list precedence takes effect immediately, without
 * a restart). Pass the SAME instance consulted by the auth/fleet is_denied
 * callback. Safe to call with NULL h or NULL deny. */
/**
 * signet_mgmt_handler_set_deny_list:
 * @h: (not nullable): a #SignetMgmtHandler
 * @deny: (nullable): deny
 *
 * Attach the daemon's live deny list so that revoke_agent can add revoked pubkeys to it (and so deny-list precedence takes effect immediately, without a restart). Pass the SAME instance consulted by the auth/fleet is_denied callback. Safe to call with NULL h or NULL deny.
 *
 * Since: 1.0
 */
void signet_mgmt_handler_set_deny_list(SignetMgmtHandler *h,
                                       struct SignetDenyList *deny);

/* Attach a replay cache so each DELIVERED EVENT ID executes at most once per
 * cache TTL. This suppresses relay redelivery, republishing of the same
 * serialized event, and history replay of non-idempotent commands
 * (rotate-key, reissue-connect, provision). It does NOT deduplicate a client
 * retry that re-signs/re-wraps the same JSON-RPC request — that produces a
 * new event id and executes again. Duplicates are dropped SILENTLY (no error
 * ack): the first delivery already published the authoritative ack, and an
 * error ack sharing its request_id could race ahead of a secret-bearing
 * result; clients recover from a lost ack by sending a new intent.
 * The check is by event id only: gift-wrapped (kind 1059) intents have a
 * NIP-59-randomized outer created_at, so timestamp skew cannot be enforced
 * here — history-replay bounds come from the subscription since-floor and
 * mgmt_accept_after. When a cache is attached, events with a missing/empty
 * event id fail closed (with a replay_invalid error ack). Use a DEDICATED
 * cache instance rather than sharing the NIP-46 one, so signing traffic
 * cannot evict management entries before their TTL. Safe to call with NULL
 * h or NULL replay (NULL disables the check). */
/**
 * signet_mgmt_handler_set_replay_cache:
 * @h: (not nullable): a #SignetMgmtHandler
 * @replay: (nullable): replay cache
 *
 * Attach a replay cache so each management event executes at most once per cache TTL. Safe to call with NULL h or NULL replay.
 *
 * Since: 1.1
 */
void signet_mgmt_handler_set_replay_cache(SignetMgmtHandler *h,
                                          struct SignetReplayCache *replay);

/* Attach a SEPARATE replay cache for self-service (non-provisioner)
 * agent/reissue-connect events. Isolating the domains means an agent
 * flooding unique self-reissue intents can only churn its own cache — it can
 * never evict provisioner event ids from the privileged cache within their
 * TTL and then replay a captured provisioner mutation. Wire BOTH caches; if
 * this one is absent, self-service events fall back to the provisioner
 * cache (functional, but without the isolation guarantee). Safe on NULL. */
/**
 * signet_mgmt_handler_set_self_replay_cache:
 * @h: (not nullable): a #SignetMgmtHandler
 * @replay: (nullable): replay cache for self-service reissue events
 *
 * Attach a dedicated replay cache for self-service reissue events so they cannot evict provisioner entries. Safe to call with NULL h or NULL replay.
 *
 * Since: 1.1
 */
void signet_mgmt_handler_set_self_replay_cache(SignetMgmtHandler *h,
                                               struct SignetReplayCache *replay);

/* Execute an already-normalized ContextVM request. This lower-level entry
 * point exists for transport-independent tests; relay callers use
 * signet_mgmt_handler_handle_intent(). */
int signet_mgmt_handler_handle_request(SignetMgmtHandler *h,
                                       const char *event_pubkey_hex,
                                       const char *params_json,
                                       SignetMgmtOp op,
                                       const char *event_id_hex,
                                       int64_t now);

/* Handle a Cascadia ContextVM kind-25910 JSON-RPC management intent.
 * Plain kind-25910 events are accepted for test/local compatibility; kind-1059
 * gift-wrap callers should pass the unwrapped 25910 JSON as content_json and
 * the inner sender pubkey as event_pubkey_hex.
 */
int signet_mgmt_handler_handle_intent(SignetMgmtHandler *h,
                                      const char *event_pubkey_hex,
                                      const char *content_json,
                                      const char *event_id_hex,
                                      int64_t now);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_MGMT_PROTOCOL_H */
