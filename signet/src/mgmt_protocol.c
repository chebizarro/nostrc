/* SPDX-License-Identifier: MIT
 *
 * mgmt_protocol.c - Nostr-native management protocol for Signet.
 *
 * Handles canonical ContextVM management methods for agent provisioning,
 * revocation, policy updates, and status queries. All operations execute
 * against the SignetKeyStore (SQLCipher + hot cache).
 */

#include "signet/mgmt_protocol.h"
#include "signet/cascadia.h"
#include "signet/key_store.h"
#include "signet/policy_store.h"
#include "signet/relay_pool.h"
#include "signet/audit_logger.h"
#include "signet/revocation.h"
#include "signet/replay_cache.h"
#include "signet/store.h"
#include "signet/bootstrap_delivery.h"
#include "signet/util.h"

#include <nostr/nip17/nip17.h>
#include <nostr/nip44/nip44.h>
#include <nostr/nip19/nip19.h>
#include <secure_buf.h>
#include <sodium.h>

#include <string.h>
#include <time.h>

#include <glib.h>
#include <json-glib/json-glib.h>

/* libnostr */
#include <nostr-event.h>
#include <nostr-keys.h>

/* ---- small helpers (also in nip46_server.c; duplicated to stay static) ---- */

static void signet_mgmt_memzero(void *p, size_t n) {
  if (p && n) secure_wipe(p, n);
}

#define signet_mgmt_hex_to_bytes32 signet_hex_to_bytes32

const char *signet_mgmt_op_to_string(SignetMgmtOp op) {
  switch (op) {
    case SIGNET_MGMT_OP_PROVISION_AGENT: return "provision_agent";
    case SIGNET_MGMT_OP_REVOKE_AGENT:   return "revoke_agent";
    case SIGNET_MGMT_OP_SET_POLICY:      return "set_policy";
    case SIGNET_MGMT_OP_GET_STATUS:      return "get_status";
    case SIGNET_MGMT_OP_LIST_AGENTS:     return "list_agents";
    case SIGNET_MGMT_OP_ROTATE_KEY:      return "rotate_key";
    case SIGNET_MGMT_OP_ADOPT_EXISTING:  return "adopt_existing";
    case SIGNET_MGMT_OP_REISSUE_CONNECT: return "reissue_connect";
    case SIGNET_MGMT_OP_LIST_CLIENTS:    return "list_clients";
    case SIGNET_MGMT_OP_REVOKE_CLIENT:   return "revoke_client";
    default:                             return "unknown";
  }
}

/* ----------------------------- authorization ----------------------------- */

bool signet_mgmt_is_authorized(const char *event_pubkey_hex,
                               const char *const *provisioner_pubkeys,
                               size_t n_provisioner_pubkeys) {
  if (!event_pubkey_hex || !event_pubkey_hex[0]) return false;
  if (!provisioner_pubkeys || n_provisioner_pubkeys == 0) return false;

  for (size_t i = 0; i < n_provisioner_pubkeys; i++) {
    const char *pk = provisioner_pubkeys[i];
    if (!pk || !pk[0]) continue;
    if (g_ascii_strcasecmp(pk, event_pubkey_hex) == 0) return true;
  }
  return false;
}

/* ------------------------------ parsing ---------------------------------- */

void signet_mgmt_request_clear(SignetMgmtRequest *req) {
  if (!req) return;
  g_free(req->agent_id);
  g_free(req->policy_json);
  g_free(req->request_id);
  g_free(req->bootstrap_pubkey);
  /* agent_nsec/connect_secret carry secret material — wipe before free. */
  if (req->agent_nsec) { secure_wipe(req->agent_nsec, strlen(req->agent_nsec)); g_free(req->agent_nsec); }
  if (req->connect_secret) { secure_wipe(req->connect_secret, strlen(req->connect_secret)); g_free(req->connect_secret); }
  g_free(req->expected_pubkey);
  g_free(req->client_pubkey);
  memset(req, 0, sizeof(*req));
}

int signet_mgmt_request_parse(SignetMgmtOp op,
                              const char *content_json,
                              SignetMgmtRequest *out_req,
                              char **out_error) {
  if (!out_req) {
    if (out_error) *out_error = g_strdup("null output");
    return -1;
  }
  memset(out_req, 0, sizeof(*out_req));

  out_req->op = op;

  if (out_req->op == SIGNET_MGMT_OP_UNKNOWN) {
    if (out_error) *out_error = g_strdup("unknown ContextVM management method");
    return -1;
  }

  if (!content_json || !content_json[0]) {
    /* Some commands (get_status, list_agents) may have empty content. */
    if (out_req->op == SIGNET_MGMT_OP_GET_STATUS ||
        out_req->op == SIGNET_MGMT_OP_LIST_AGENTS) {
      return 0;
    }
    if (out_error) *out_error = g_strdup("missing content JSON");
    return -1;
  }

  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, content_json, -1, NULL)) {
    if (out_error) *out_error = g_strdup("invalid JSON");
    return -1;
  }

  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    if (out_error) *out_error = g_strdup("content must be a JSON object");
    return -1;
  }

  JsonObject *o = json_node_get_object(root);

  /* Extract common fields. */
  if (json_object_has_member(o, "agent_id")) {
    const char *v = json_object_get_string_member(o, "agent_id");
    if (v && v[0]) out_req->agent_id = g_strdup(v);
  }

  if (json_object_has_member(o, "request_id")) {
    const char *v = json_object_get_string_member(o, "request_id");
    if (v && v[0]) out_req->request_id = g_strdup(v);
  }

  if (json_object_has_member(o, "policy")) {
    JsonNode *pn = json_object_get_member(o, "policy");
    if (pn && JSON_NODE_HOLDS_OBJECT(pn)) {
      JsonGenerator *gen = json_generator_new();
      json_generator_set_pretty(gen, FALSE);
      json_generator_set_root(gen, pn);
      out_req->policy_json = json_generator_to_data(gen, NULL);
      g_object_unref(gen);
    }
  }

  if (json_object_has_member(o, "deliver")) {
    out_req->deliver = json_object_get_boolean_member(o, "deliver");
  }
  if (json_object_has_member(o, "bootstrap_pubkey")) {
    const char *v = json_object_get_string_member(o, "bootstrap_pubkey");
    if (v && v[0]) out_req->bootstrap_pubkey = g_strdup(v);
  }
  if (json_object_has_member(o, "delivery_ttl")) {
    out_req->delivery_ttl = json_object_get_int_member(o, "delivery_ttl");
  }
  if (json_object_has_member(o, "agent_nsec")) {
    const char *v = json_object_get_string_member(o, "agent_nsec");
    if (v && v[0]) out_req->agent_nsec = g_strdup(v);
  }
  if (json_object_has_member(o, "expected_pubkey")) {
    const char *v = json_object_get_string_member(o, "expected_pubkey");
    if (v && v[0]) out_req->expected_pubkey = g_strdup(v);
  }
  if (json_object_has_member(o, "connect_secret")) {
    const char *v = json_object_get_string_member(o, "connect_secret");
    if (v && v[0]) out_req->connect_secret = g_strdup(v);
  }
  if (json_object_has_member(o, "client_pubkey")) {
    const char *v = json_object_get_string_member(o, "client_pubkey");
    if (v && v[0]) out_req->client_pubkey = g_strdup(v);
  }

  /* Validate required fields per op. */
  bool needs_agent_id = (out_req->op == SIGNET_MGMT_OP_PROVISION_AGENT ||
                         out_req->op == SIGNET_MGMT_OP_REVOKE_AGENT ||
                         out_req->op == SIGNET_MGMT_OP_SET_POLICY ||
                         out_req->op == SIGNET_MGMT_OP_ROTATE_KEY ||
                         out_req->op == SIGNET_MGMT_OP_ADOPT_EXISTING ||
                         out_req->op == SIGNET_MGMT_OP_REISSUE_CONNECT ||
                         out_req->op == SIGNET_MGMT_OP_LIST_CLIENTS);

  if (needs_agent_id && (!out_req->agent_id || !out_req->agent_id[0])) {
    if (out_error) *out_error = g_strdup("agent_id is required");
    signet_mgmt_request_clear(out_req);
    return -1;
  }

  if (out_req->op == SIGNET_MGMT_OP_SET_POLICY &&
      (!out_req->policy_json || !out_req->policy_json[0])) {
    if (out_error) *out_error = g_strdup("set_policy requires policy object");
    signet_mgmt_request_clear(out_req);
    return -1;
  }

  if (out_req->op == SIGNET_MGMT_OP_ADOPT_EXISTING &&
      (!out_req->agent_nsec || !out_req->agent_nsec[0])) {
    if (out_error) *out_error = g_strdup("adopt_existing requires agent_nsec");
    signet_mgmt_request_clear(out_req);
    return -1;
  }

  if (out_req->op == SIGNET_MGMT_OP_REVOKE_CLIENT &&
      (!out_req->client_pubkey || !out_req->client_pubkey[0])) {
    if (out_error) *out_error = g_strdup("revoke_client requires client_pubkey");
    signet_mgmt_request_clear(out_req);
    return -1;
  }

  return 0;
}

/* ----------------------------- response building ------------------------- */

char *signet_mgmt_build_ack(const char *request_id,
                            bool ok,
                            const char *code,
                            const char *message,
                            const char *result_json) {
  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;

  json_builder_begin_object(b);

  json_builder_set_member_name(b, "ok");
  json_builder_add_boolean_value(b, ok ? TRUE : FALSE);

  if (request_id) {
    json_builder_set_member_name(b, "request_id");
    json_builder_add_string_value(b, request_id);
  }

  if (code) {
    json_builder_set_member_name(b, "code");
    json_builder_add_string_value(b, code);
  }

  if (message) {
    json_builder_set_member_name(b, "message");
    json_builder_add_string_value(b, message);
  }

  if (result_json) {
    g_autoptr(JsonParser) rp = json_parser_new();
    if (json_parser_load_from_data(rp, result_json, -1, NULL)) {
      JsonNode *r = json_parser_get_root(rp);
      if (r) {
        json_builder_set_member_name(b, "result");
        json_builder_add_value(b, json_node_copy(r));
      }
    }
  }

  json_builder_end_object(b);

  JsonGenerator *g = json_generator_new();
  if (!g) { g_object_unref(b); return NULL; }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  json_generator_set_pretty(g, FALSE);
  char *out = json_generator_to_data(g, NULL);

  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);
  return out;
}

/* ==================== Management Handler ================================= */

struct SignetMgmtHandler {
  SignetKeyStore *keys;
  SignetRelayPool *relays;
  SignetAuditLogger *audit;
  SignetPolicyStore *policy_store;
  SignetDenyList *deny;   /* shared live deny list (owned by daemon) */
  SignetReplayCache *replay;      /* provisioner replay cache (owned by daemon) */
  SignetReplayCache *replay_self; /* self-service replay cache (owned by daemon) */

  char **provisioner_pubkeys;
  size_t n_provisioner_pubkeys;

  char *bunker_sk_hex;
  char *bunker_pk_hex;

  char **relay_urls;
  size_t n_relay_urls;
};

SignetMgmtHandler *signet_mgmt_handler_new(SignetKeyStore *keys,
                                           SignetRelayPool *relays,
                                           SignetAuditLogger *audit,
                                           SignetPolicyStore *policy_store,
                                           const SignetMgmtHandlerConfig *cfg) {
  if (!cfg) return NULL;

  SignetMgmtHandler *h = (SignetMgmtHandler *)g_new0(SignetMgmtHandler, 1);
  if (!h) return NULL;

  h->keys = keys;
  h->relays = relays;
  h->audit = audit;
  h->policy_store = policy_store;

  if (cfg->provisioner_pubkeys && cfg->n_provisioner_pubkeys > 0) {
    h->provisioner_pubkeys = (char **)g_new0(char *, cfg->n_provisioner_pubkeys);
    for (size_t i = 0; i < cfg->n_provisioner_pubkeys; i++) {
      h->provisioner_pubkeys[i] = g_strdup(cfg->provisioner_pubkeys[i]);
    }
    h->n_provisioner_pubkeys = cfg->n_provisioner_pubkeys;
  }

  if (cfg->bunker_secret_key_hex) {
    size_t sk_hex_len = strlen(cfg->bunker_secret_key_hex);
    h->bunker_sk_hex = (char *)sodium_malloc(sk_hex_len + 1);
    if (!h->bunker_sk_hex) {
      signet_mgmt_handler_free(h);
      return NULL;
    }
    memcpy(h->bunker_sk_hex, cfg->bunker_secret_key_hex, sk_hex_len + 1);
  }
  h->bunker_pk_hex = g_strdup(cfg->bunker_pubkey_hex);

  if (cfg->relay_urls && cfg->n_relay_urls > 0) {
    h->relay_urls = (char **)g_new0(char *, cfg->n_relay_urls);
    for (size_t i = 0; i < cfg->n_relay_urls; i++) {
      h->relay_urls[i] = g_strdup(cfg->relay_urls[i]);
    }
    h->n_relay_urls = cfg->n_relay_urls;
  }

  return h;
}

void signet_mgmt_handler_free(SignetMgmtHandler *h) {
  if (!h) return;

  for (size_t i = 0; i < h->n_provisioner_pubkeys; i++) g_free(h->provisioner_pubkeys[i]);
  g_free(h->provisioner_pubkeys);

  if (h->bunker_sk_hex) {
    sodium_memzero(h->bunker_sk_hex, strlen(h->bunker_sk_hex));
    sodium_free(h->bunker_sk_hex);
  }
  g_free(h->bunker_pk_hex);
  for (size_t i = 0; i < h->n_relay_urls; i++) g_free(h->relay_urls[i]);
  g_free(h->relay_urls);
  g_free(h);
}

void signet_mgmt_handler_set_replay_cache(SignetMgmtHandler *h,
                                          SignetReplayCache *replay) {
  if (!h) return;
  h->replay = replay;
}

void signet_mgmt_handler_set_self_replay_cache(SignetMgmtHandler *h,
                                               SignetReplayCache *replay) {
  if (!h) return;
  h->replay_self = replay;
}

void signet_mgmt_handler_set_deny_list(SignetMgmtHandler *h, SignetDenyList *deny) {
  if (!h) return;
  h->deny = deny;
}

static char *signet_mgmt_ack_to_jsonrpc(const char *ack_content) {
  if (!ack_content) return NULL;
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, ack_content, -1, NULL)) return g_strdup(ack_content);
  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return g_strdup(ack_content);
  JsonObject *o = json_node_get_object(root);
  if (json_object_has_member(o, "jsonrpc")) return g_strdup(ack_content);

  const char *rid = json_object_has_member(o, "request_id") ? json_object_get_string_member(o, "request_id") : NULL;
  bool ok = json_object_has_member(o, "ok") ? json_object_get_boolean_member(o, "ok") : false;
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc"); json_builder_add_string_value(b, "2.0");
  if (ok) {
    json_builder_set_member_name(b, "result");
    if (json_object_has_member(o, "result")) json_builder_add_value(b, json_node_copy(json_object_get_member(o, "result")));
    else json_builder_add_value(b, json_node_copy(root));
  } else {
    json_builder_set_member_name(b, "error");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "code"); json_builder_add_int_value(b, -32000);
    json_builder_set_member_name(b, "message");
    json_builder_add_string_value(b, json_object_has_member(o, "message") ? json_object_get_string_member(o, "message") : "Signet management command failed");
    if (json_object_has_member(o, "code")) {
      json_builder_set_member_name(b, "data"); json_builder_add_string_value(b, json_object_get_string_member(o, "code"));
    }
    json_builder_end_object(b);
  }
  json_builder_set_member_name(b, "id");
  if (rid) json_builder_add_string_value(b, rid); else json_builder_add_null_value(b);
  json_builder_end_object(b);
  JsonNode *out = json_builder_get_root(b);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, out); json_generator_set_pretty(gen, FALSE);
  char *s = json_generator_to_data(gen, NULL);
  json_node_free(out);
  return s;
}

static void signet_mgmt_publish_contextvm_reply(SignetMgmtHandler *h,
                                                const char *recipient_pubkey_hex,
                                                const char *ack_content) {
  if (!h || !h->relays || !h->bunker_sk_hex || !recipient_pubkey_hex || !ack_content) return;
  char *jsonrpc = signet_mgmt_ack_to_jsonrpc(ack_content);
  if (!jsonrpc) return;
  NostrEvent *gift = nostr_nip17_wrap_dm(h->bunker_sk_hex, recipient_pubkey_hex, jsonrpc);
  /* jsonrpc may embed a fresh connect_secret / bunker URI — wipe before free. */
  secure_wipe(jsonrpc, strlen(jsonrpc));
  g_free(jsonrpc);
  if (!gift) return;
  char *event_json = nostr_event_serialize_compact(gift);
  nostr_event_free(gift);
  if (event_json) {
    (void)signet_relay_pool_publish_event_json(h->relays, event_json);
    free(event_json);
  }
}

/* Publish a canonical NIP-59 gift-wrapped ContextVM result. */
static void signet_mgmt_publish_ack(SignetMgmtHandler *h,
                                    const char *recipient_pubkey_hex,
                                    const char *ack_content,
                                    const char *ref_event_id_hex,
                                    int64_t now) {
  (void)ref_event_id_hex;
  (void)now;
  signet_mgmt_publish_contextvm_reply(h, recipient_pubkey_hex, ack_content);
}

int signet_mgmt_handler_handle_request(SignetMgmtHandler *h,
                                       const char *event_pubkey_hex,
                                       const char *content_json,
                                       SignetMgmtOp op,
                                       const char *event_id_hex,
                                       int64_t now);

static SignetMgmtOp signet_mgmt_op_from_contextvm_method(const char *method) {
  if (!method) return SIGNET_MGMT_OP_UNKNOWN;
  if (strcmp(method, "agent/provision") == 0) return SIGNET_MGMT_OP_PROVISION_AGENT;
  if (strcmp(method, "agent/revoke") == 0) return SIGNET_MGMT_OP_REVOKE_AGENT;
  if (strcmp(method, "agent/set-policy") == 0) return SIGNET_MGMT_OP_SET_POLICY;
  if (strcmp(method, "agent/get-status") == 0) return SIGNET_MGMT_OP_GET_STATUS;
  if (strcmp(method, "agent/list") == 0) return SIGNET_MGMT_OP_LIST_AGENTS;
  if (strcmp(method, "agent/rotate-key") == 0) return SIGNET_MGMT_OP_ROTATE_KEY;
  if (strcmp(method, "agent/adopt-existing") == 0) return SIGNET_MGMT_OP_ADOPT_EXISTING;
  if (strcmp(method, "agent/reissue-connect") == 0) return SIGNET_MGMT_OP_REISSUE_CONNECT;
  if (strcmp(method, "agent/list-clients") == 0) return SIGNET_MGMT_OP_LIST_CLIENTS;
  if (strcmp(method, "agent/revoke-client") == 0) return SIGNET_MGMT_OP_REVOKE_CLIENT;
  return SIGNET_MGMT_OP_UNKNOWN;
}

static char *signet_mgmt_contextvm_params_json(const char *content_json, SignetMgmtOp *out_op) {
  if (out_op) *out_op = SIGNET_MGMT_OP_UNKNOWN;
  if (!content_json || !content_json[0]) return NULL;

  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, content_json, -1, NULL)) return NULL;
  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;
  JsonObject *o = json_node_get_object(root);
  const char *method = json_object_has_member(o, "method") ? json_object_get_string_member(o, "method") : NULL;
  SignetMgmtOp op = signet_mgmt_op_from_contextvm_method(method);
  if (op == SIGNET_MGMT_OP_UNKNOWN) return NULL;
  if (out_op) *out_op = op;

  JsonNode *params = json_object_has_member(o, "params") ? json_object_get_member(o, "params") : NULL;
  JsonObject *po = (params && JSON_NODE_HOLDS_OBJECT(params)) ? json_node_get_object(params) : NULL;

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  if (json_object_has_member(o, "id")) {
    json_builder_set_member_name(b, "request_id");
    JsonNode *idn = json_object_get_member(o, "id");
    if (JSON_NODE_HOLDS_VALUE(idn) && json_node_get_value_type(idn) == G_TYPE_STRING)
      json_builder_add_string_value(b, json_node_get_string(idn));
    else if (JSON_NODE_HOLDS_VALUE(idn) && json_node_get_value_type(idn) == G_TYPE_INT64)
      json_builder_add_int_value(b, json_node_get_int(idn));
    else
      json_builder_add_value(b, json_node_copy(idn));
  }
  if (po && json_object_has_member(po, "agent_id")) {
    json_builder_set_member_name(b, "agent_id");
    json_builder_add_string_value(b, json_object_get_string_member(po, "agent_id"));
  }
  if (po && json_object_has_member(po, "agent")) {
    json_builder_set_member_name(b, "agent_id");
    json_builder_add_string_value(b, json_object_get_string_member(po, "agent"));
  }
  if (po && json_object_has_member(po, "policy")) {
    json_builder_set_member_name(b, "policy");
    json_builder_add_value(b, json_node_copy(json_object_get_member(po, "policy")));
  }
  if (po && json_object_has_member(po, "deliver")) {
    json_builder_set_member_name(b, "deliver");
    json_builder_add_boolean_value(b, json_object_get_boolean_member(po, "deliver"));
  }
  if (po && json_object_has_member(po, "bootstrap_pubkey")) {
    json_builder_set_member_name(b, "bootstrap_pubkey");
    json_builder_add_string_value(b, json_object_get_string_member(po, "bootstrap_pubkey"));
  }
  if (po && json_object_has_member(po, "delivery_ttl")) {
    json_builder_set_member_name(b, "delivery_ttl");
    json_builder_add_int_value(b, json_object_get_int_member(po, "delivery_ttl"));
  }
  if (po && json_object_has_member(po, "agent_nsec")) {
    json_builder_set_member_name(b, "agent_nsec");
    json_builder_add_string_value(b, json_object_get_string_member(po, "agent_nsec"));
  }
  if (po && json_object_has_member(po, "expected_pubkey")) {
    json_builder_set_member_name(b, "expected_pubkey");
    json_builder_add_string_value(b, json_object_get_string_member(po, "expected_pubkey"));
  }
  if (po && json_object_has_member(po, "connect_secret")) {
    json_builder_set_member_name(b, "connect_secret");
    json_builder_add_string_value(b, json_object_get_string_member(po, "connect_secret"));
  }
  if (po && json_object_has_member(po, "client_pubkey")) {
    json_builder_set_member_name(b, "client_pubkey");
    json_builder_add_string_value(b, json_object_get_string_member(po, "client_pubkey"));
  }
  json_builder_end_object(b);

  JsonNode *out = json_builder_get_root(b);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, out);
  json_generator_set_pretty(gen, FALSE);
  char *s = json_generator_to_data(gen, NULL);
  json_node_free(out);
  return s;
}

int signet_mgmt_handler_handle_intent(SignetMgmtHandler *h,
                                      const char *event_pubkey_hex,
                                      const char *content_json,
                                      const char *event_id_hex,
                                      int64_t now) {
  if (!h) return -1;

  SignetMgmtOp op = SIGNET_MGMT_OP_UNKNOWN;
  char *params_json = signet_mgmt_contextvm_params_json(content_json, &op);

  /* agent/reissue-connect additionally supports SELF-SERVICE authorization:
   * a non-provisioner sender may reissue its own connect_secret. That check
   * needs the parsed agent_id, so for this one method the sender check is
   * deferred to the normalized request execution path. Every other method (and
   * every unknown method) remains provisioner-only and is rejected here. */
  bool defer_auth = (op == SIGNET_MGMT_OP_REISSUE_CONNECT);
  if (!defer_auth &&
      !signet_mgmt_is_authorized(event_pubkey_hex,
                                 (const char *const *)h->provisioner_pubkeys,
                                 h->n_provisioner_pubkeys)) {
    char *err = g_strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32002,\"message\":\"sender is not an authorized Signet provisioner\"},\"id\":null}");
    signet_mgmt_publish_ack(h, event_pubkey_hex, err, event_id_hex, now);
    g_free(err);
    if (params_json) { secure_wipe(params_json, strlen(params_json)); g_free(params_json); }
    return -1;
  }

  if (!params_json || op == SIGNET_MGMT_OP_UNKNOWN) {
    char *err = g_strdup("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32601,\"message\":\"unknown Signet ContextVM method\"},\"id\":null}");
    signet_mgmt_publish_ack(h, event_pubkey_hex, err, event_id_hex, now);
    g_free(err);
    if (params_json) { secure_wipe(params_json, strlen(params_json)); g_free(params_json); }
    return -1;
  }

  char *encrypted = NULL;
  uint8_t sk[32], pk[32];
  if (signet_mgmt_hex_to_bytes32(h->bunker_sk_hex, sk) &&
      signet_mgmt_hex_to_bytes32(event_pubkey_hex, pk)) {
    (void)nostr_nip44_encrypt_v2(sk, pk, (const uint8_t *)params_json,
                                 strlen(params_json), &encrypted);
  }
  signet_mgmt_memzero(sk, sizeof(sk));
  /* params_json may embed agent_nsec/connect_secret (adopt-existing). */
  secure_wipe(params_json, strlen(params_json));
  g_free(params_json);
  if (!encrypted) return -1;
  int rc = signet_mgmt_handler_handle_request(h, event_pubkey_hex, encrypted,
                                              op, event_id_hex, now);
  free(encrypted);
  return rc;
}

static void signet_mgmt_publish_cas_audit(SignetMgmtHandler *h,
                                           const char *audit_type,
                                           const char *agent_id,
                                           const char *status,
                                           int64_t now) {
  if (!h || !h->relays || !h->bunker_sk_hex || !audit_type) return;
  NostrEvent *evt = nostr_event_new();
  if (!evt) return;
  nostr_event_set_kind(evt, CAS_AUDIT);
  nostr_event_set_created_at(evt, now);
  /* JsonBuilder, not printf: agent_id comes from request params, and quotes/
   * control characters must not yield malformed (or misleading) bunker-signed
   * audit content. */
  char *content = NULL;
  {
    JsonBuilder *ab = json_builder_new();
    if (ab) {
      json_builder_begin_object(ab);
      json_builder_set_member_name(ab, "domain");
      json_builder_add_string_value(ab, "signet");
      json_builder_set_member_name(ab, "type");
      json_builder_add_string_value(ab, audit_type);
      json_builder_set_member_name(ab, "agent");
      json_builder_add_string_value(ab, agent_id ? agent_id : "");
      json_builder_set_member_name(ab, "status");
      json_builder_add_string_value(ab, status ? status : "ok");
      json_builder_end_object(ab);
      JsonNode *aroot = json_builder_get_root(ab);
      JsonGenerator *agen = json_generator_new();
      if (agen && aroot) {
        json_generator_set_root(agen, aroot);
        json_generator_set_pretty(agen, FALSE);
        content = json_generator_to_data(agen, NULL);
      }
      if (agen) g_object_unref(agen);
      if (aroot) json_node_free(aroot);
      g_object_unref(ab);
    }
  }
  nostr_event_set_content(evt, content ? content : "{}");
  g_free(content);
  NostrTags *tags = nostr_tags_new(0);
  if (tags) {
    NostrTag *domain = nostr_tag_new("domain", "signet", NULL);
    NostrTag *type = nostr_tag_new("type", audit_type, NULL);
    if (domain) nostr_tags_append(tags, domain);
    if (type) nostr_tags_append(tags, type);
    if (agent_id && agent_id[0]) {
      NostrTag *agent = nostr_tag_new("agent", agent_id, NULL);
      if (agent) nostr_tags_append(tags, agent);
    }
    nostr_event_set_tags(evt, tags);
  }
  if (nostr_event_sign(evt, h->bunker_sk_hex) == 0) {
    char *json = nostr_event_serialize_compact(evt);
    if (json) { (void)signet_relay_pool_publish_event_json(h->relays, json); free(json); }
  }
  nostr_event_free(evt);
}

int signet_mgmt_handler_handle_request(SignetMgmtHandler *h,
                                       const char *event_pubkey_hex,
                                       const char *content_json,
                                       SignetMgmtOp op,
                                       const char *event_id_hex,
                                       int64_t now) {
  if (!h) return -1;

  /* 1) Authorization check. agent/reissue-connect alone also admits the
   * target agent itself; its sender-vs-agent check runs after parse (the
   * agent_id is needed) in the execution case below. The sender pubkey is
   * trustworthy on every ingress path: plain events are Schnorr-verified
   * before dispatch (relay_pool NPA-01) and gift-wrapped intents take the
   * authenticated seal author from nostr_nip17_decrypt_dm. */
  bool sender_is_provisioner =
      signet_mgmt_is_authorized(event_pubkey_hex,
                                (const char *const *)h->provisioner_pubkeys,
                                h->n_provisioner_pubkeys);
  if (!sender_is_provisioner && op != SIGNET_MGMT_OP_REISSUE_CONNECT) {
    return -1; /* silently drop unauthorized events */
  }

  /* 1b) NIP-44 v2 decrypt the event content.
   * Management events with content MUST decrypt successfully.
   * Plaintext fallback is intentionally rejected. */
  char *decrypted_content = NULL;
  const char *effective_content = content_json;

  if (content_json && content_json[0]) {
    if (!h->bunker_sk_hex || !event_pubkey_hex) {
      return -1;
    }

    uint8_t sk[32], pk[32];
    if (!signet_mgmt_hex_to_bytes32(h->bunker_sk_hex, sk) ||
        !signet_mgmt_hex_to_bytes32(event_pubkey_hex, pk)) {
      signet_mgmt_memzero(sk, sizeof(sk));
      return -1;
    }

    uint8_t *pt = NULL;
    size_t pt_len = 0;
    int drc = nostr_nip44_decrypt_v2(sk, pk, content_json, &pt, &pt_len);
    signet_mgmt_memzero(sk, sizeof(sk));
    if (drc != 0 || !pt || pt_len == 0) {
      if (pt) secure_wipe(pt, pt_len);
      free(pt);
      return -1;
    }

    decrypted_content = (char *)malloc(pt_len + 1);
    if (!decrypted_content) {
      secure_wipe(pt, pt_len);
      free(pt);
      return -1;
    }
    memcpy(decrypted_content, pt, pt_len);
    decrypted_content[pt_len] = '\0';
    effective_content = decrypted_content;
    /* pt is the decrypted plaintext (may hold agent_nsec) — wipe before free. */
    secure_wipe(pt, pt_len);
    free(pt);
  }

  /* 2) Parse request. */
  SignetMgmtRequest req;
  char *parse_err = NULL;
  if (signet_mgmt_request_parse(op, effective_content, &req, &parse_err) != 0) {
    char *ack = signet_mgmt_build_ack(NULL, false, "parse_error",
                                       parse_err ? parse_err : "invalid request", NULL);
    if (ack) {
      signet_mgmt_publish_ack(h, event_pubkey_hex, ack, event_id_hex, now);
      g_free(ack);
    }
    g_free(parse_err);
    if (decrypted_content) { secure_wipe(decrypted_content, strlen(decrypted_content)); free(decrypted_content); }
    return -1;
  }

  /* 2a) Deferred self-service authorization for agent/reissue-connect: a
   * non-provisioner sender must BE the target agent — the signed sender
   * pubkey must equal the agent's identity pubkey, resolved from custody
   * (never from request params). This runs BEFORE the replay mark so
   * unauthorized floods cannot churn the replay cache and evict legitimate
   * management event ids within their TTL. An unknown agent_id gets the same
   * "unauthorized" as a mismatch so a stranger cannot probe which agent_ids
   * exist. (Senders of every other operation were already gated above.) */
  if (req.op == SIGNET_MGMT_OP_REISSUE_CONNECT && !sender_is_provisioner) {
    char self_pk[65] = {0};
    bool known = signet_key_store_get_agent_pubkey(h->keys, req.agent_id,
                                                   self_pk, sizeof(self_pk));
    if (!known || !event_pubkey_hex || !event_pubkey_hex[0] ||
        g_ascii_strcasecmp(self_pk, event_pubkey_hex) != 0) {
      /* Encrypted ack only — deliberately NO public CAS audit here: this is
       * an unauthenticated claim about an agent_id, and publishing
       * bunker-signed audits for arbitrary claims would hand strangers an
       * audit-spam/poisoning primitive. */
      char *uack = signet_mgmt_build_ack(req.request_id, false, "unauthorized",
                                         "sender is neither a provisioner nor the target agent",
                                         NULL);
      if (uack) {
        signet_mgmt_publish_ack(h, event_pubkey_hex, uack, event_id_hex, now);
        g_free(uack);
      }
      if (decrypted_content) { secure_wipe(decrypted_content, strlen(decrypted_content)); free(decrypted_content); }
      signet_mgmt_request_clear(&req);
      return -1;
    }
  }

  /* 2a-bis) Suspension gate for reissue-connect, BOTH auth paths, also before
   * the replay mark: a deny-listed agent must not obtain a fresh
   * connect_secret (self-service cannot bypass suspension; a provisioner
   * must lift the deny entry first). Keyed by PUBKEY only, like the
   * adopt-existing gate — deny_list_remove clears only the pubkey cache key,
   * so matching agent_id would keep blocking an un-suspended agent until
   * restart. */
  if (req.op == SIGNET_MGMT_OP_REISSUE_CONNECT && h->deny) {
    char target_pk[65] = {0};
    bool have_pk = signet_key_store_get_agent_pubkey(
        h->keys, req.agent_id, target_pk, sizeof(target_pk));
    if ((have_pk && signet_deny_list_contains(h->deny, target_pk)) ||
        (event_pubkey_hex && event_pubkey_hex[0] &&
         signet_deny_list_contains(h->deny, event_pubkey_hex))) {
      char *dack = signet_mgmt_build_ack(req.request_id, false, "deny_listed",
                                         "agent or sender is deny-listed", NULL);
      if (dack) {
        signet_mgmt_publish_ack(h, event_pubkey_hex, dack, event_id_hex, now);
        g_free(dack);
      }
      signet_mgmt_publish_cas_audit(h, "reissue_connect", req.agent_id,
                                    "deny_listed", now);
      if (decrypted_content) { secure_wipe(decrypted_content, strlen(decrypted_content)); free(decrypted_content); }
      signet_mgmt_request_clear(&req);
      return -1;
    }
  }

  /* 2b) Replay protection: each delivered event id executes at most once per
   * cache TTL. This runs AFTER parse (so the error ack can carry request_id)
   * and BEFORE execute (so non-idempotent commands — rotate-key,
   * reissue-connect, provision — never run twice on relay redelivery,
   * republish of the same serialized event, or history replay).
   * Duplicate suppression is by delivered event id only:
   * gift-wrapped intents carry a NIP-59-randomized outer created_at, so `now`
   * is passed for the timestamp and skew is effectively not enforced here
   * (history bounds come from the subscription since-floor and
   * mgmt_accept_after). Fail closed on a missing event id. */
  /* Self-service events are marked in their OWN replay domain so an agent
   * flooding unique self-reissue intents can never evict provisioner event
   * ids from the privileged cache within their TTL (replay-then-re-execute).
   * Falls back to the provisioner cache only if no self cache was wired. */
  SignetReplayCache *replay_domain =
      sender_is_provisioner ? h->replay
                            : (h->replay_self ? h->replay_self : h->replay);
  if (replay_domain) {
    SignetReplayResult rr = signet_replay_check_and_mark(
        replay_domain, event_id_hex, now, now);
    if (rr == SIGNET_REPLAY_DUPLICATE) {
      /* Silently drop duplicates (like unauthorized events). The FIRST
       * delivery already published the authoritative ack — publishing an
       * error ack here with the same request_id could race ahead of the
       * success ack and starve the client of a secret-bearing result
       * (reissue-connect/provision). Clients recover from a lost ack by
       * sending a NEW intent (new event id). */
      if (decrypted_content) { secure_wipe(decrypted_content, strlen(decrypted_content)); free(decrypted_content); }
      signet_mgmt_request_clear(&req);
      return -1;
    }
    if (rr != SIGNET_REPLAY_OK) {
      /* Malformed (missing event id): fail closed, but tell the sender. */
      char *rack = signet_mgmt_build_ack(req.request_id, false, "replay_invalid",
                                         "management event rejected by replay protection",
                                         NULL);
      if (rack) {
        signet_mgmt_publish_ack(h, event_pubkey_hex, rack, event_id_hex, now);
        g_free(rack);
      }
      if (decrypted_content) { secure_wipe(decrypted_content, strlen(decrypted_content)); free(decrypted_content); }
      signet_mgmt_request_clear(&req);
      return -1;
    }
  }

  /* 3) Execute command. */
  bool ok = false;
  const char *code = "internal_error";
  char *message = NULL;
  char *result = NULL;

  switch (req.op) {
    case SIGNET_MGMT_OP_PROVISION_AGENT: {
      char pubkey_hex[65];
      char *bunker_uri = NULL;
      int rc = signet_key_store_provision_agent(
          h->keys, req.agent_id,
          h->bunker_pk_hex,
          (const char *const *)h->relay_urls, h->n_relay_urls,
          pubkey_hex, sizeof(pubkey_hex),
          &bunker_uri);
      if (rc == 0) {
        ok = true;
        code = "provisioned";
        message = g_strdup_printf("agent %s provisioned", req.agent_id);
        if (bunker_uri) {
          /* Escape the bunker URI for JSON embedding */
          char *escaped_uri = g_strescape(bunker_uri, NULL);
          result = g_strdup_printf("{\"agent_id\":\"%s\",\"pubkey\":\"%s\",\"bunker_uri\":\"%s\"}",
                                   req.agent_id, pubkey_hex, escaped_uri ? escaped_uri : bunker_uri);
          g_free(escaped_uri);
        } else {
          result = g_strdup_printf("{\"agent_id\":\"%s\",\"pubkey\":\"%s\"}", req.agent_id, pubkey_hex);
        }
        if (req.deliver && req.bootstrap_pubkey && req.bootstrap_pubkey[0] && bunker_uri) {
          int64_t ttl = req.delivery_ttl > 0 ? req.delivery_ttl : 600;
          if (ttl > 900) ttl = 900;
          SignetBootstrapMessage dm = {0};
          dm.agent_id = req.agent_id;
          dm.bunker_uri = bunker_uri;
          dm.relay_urls = h->relay_urls;
          dm.n_relay_urls = h->n_relay_urls;
          dm.expires_at = now + ttl;
          if (signet_bootstrap_send(h->bunker_sk_hex, req.bootstrap_pubkey, &dm, h->relays) == 0) {
            char *old = result;
            result = g_strdup_printf("{\"agent_id\":\"%s\",\"pubkey\":\"%s\",\"bunker_uri\":\"%s\",\"delivered\":true,\"delivery_expires_at\":%" G_GINT64_FORMAT "}",
                                     req.agent_id, pubkey_hex, bunker_uri, (gint64)dm.expires_at);
            g_free(old);
          }
        }
        signet_mgmt_publish_cas_audit(h, "provision", req.agent_id, "ok", now);
        g_free(bunker_uri);
      } else {
        code = "provision_failed";
        message = g_strdup("failed to provision agent");
      }
      break;
    }

    case SIGNET_MGMT_OP_ADOPT_EXISTING: {
      /* Decode the supplied secret: nsec bech32 or 64-char hex. */
      uint8_t sk_raw[32];
      bool decoded = false;
      const char *sec = req.agent_nsec;
      if (sec && strncmp(sec, "nsec1", 5) == 0) {
        decoded = (nostr_nip19_decode_nsec(sec, sk_raw) == 0);
      } else if (sec && strlen(sec) == 64) {
        size_t bin_len = 0;
        decoded = (sodium_hex2bin(sk_raw, sizeof(sk_raw), sec, 64, NULL, &bin_len, NULL) == 0 &&
                   bin_len == 32);
      }
      if (!decoded) {
        code = "invalid_secret";
        message = g_strdup("invalid agent_nsec");
        sodium_memzero(sk_raw, sizeof(sk_raw));
        break;
      }

      /* Derive the pubkey so we can run the deny-list gate before storing. */
      char derived_pk[65] = {0};
      {
        char sk_hex[65];
        for (int i = 0; i < 32; i++) sprintf(sk_hex + i * 2, "%02x", sk_raw[i]);
        sk_hex[64] = '\0';
        char *pk = nostr_key_get_public(sk_hex);
        sodium_memzero(sk_hex, sizeof(sk_hex));
        if (pk && strlen(pk) == 64) g_strlcpy(derived_pk, pk, sizeof(derived_pk));
        free(pk);
      }
      if (!derived_pk[0]) {
        code = "invalid_secret";
        message = g_strdup("could not derive pubkey");
        sodium_memzero(sk_raw, sizeof(sk_raw));
        break;
      }

      /* expected_pubkey must match exactly when provided. */
      if (req.expected_pubkey && req.expected_pubkey[0] &&
          g_ascii_strcasecmp(derived_pk, req.expected_pubkey) != 0) {
        code = "pubkey_mismatch";
        message = g_strdup("derived pubkey does not match expected_pubkey");
        sodium_memzero(sk_raw, sizeof(sk_raw));
        break;
      }

      /* Reject deny-listed pubkeys. */
      if (h->deny && signet_deny_list_contains(h->deny, derived_pk)) {
        code = "deny_listed";
        message = g_strdup("pubkey is deny-listed");
        signet_mgmt_publish_cas_audit(h, "adopt_existing", req.agent_id, "deny_listed", now);
        sodium_memzero(sk_raw, sizeof(sk_raw));
        break;
      }

      char pubkey_hex[65];
      char *bunker_uri = NULL;
      SignetAdoptResult ar = signet_key_store_adopt_agent(
          h->keys, req.agent_id, sk_raw,
          req.expected_pubkey, req.connect_secret,
          h->bunker_pk_hex,
          (const char *const *)h->relay_urls, h->n_relay_urls,
          pubkey_hex, &bunker_uri);
      sodium_memzero(sk_raw, sizeof(sk_raw));

      if (ar == SIGNET_ADOPT_OK) {
        ok = true;
        code = "adopted";
        message = g_strdup_printf("agent %s adopted", req.agent_id);
        if (bunker_uri) {
          char *escaped_uri = g_strescape(bunker_uri, NULL);
          result = g_strdup_printf(
              "{\"agent_id\":\"%s\",\"pubkey\":\"%s\",\"adopted\":true,\"bunker_uri\":\"%s\"}",
              req.agent_id, pubkey_hex, escaped_uri ? escaped_uri : bunker_uri);
          g_free(escaped_uri);
        } else {
          result = g_strdup_printf(
              "{\"agent_id\":\"%s\",\"pubkey\":\"%s\",\"adopted\":true}",
              req.agent_id, pubkey_hex);
        }
        if (req.deliver && req.bootstrap_pubkey && req.bootstrap_pubkey[0] && bunker_uri) {
          int64_t ttl = req.delivery_ttl > 0 ? req.delivery_ttl : 600;
          if (ttl > 900) ttl = 900;
          SignetBootstrapMessage dm = {0};
          dm.agent_id = req.agent_id;
          dm.bunker_uri = bunker_uri;
          dm.relay_urls = h->relay_urls;
          dm.n_relay_urls = h->n_relay_urls;
          dm.expires_at = now + ttl;
          if (signet_bootstrap_send(h->bunker_sk_hex, req.bootstrap_pubkey, &dm, h->relays) == 0) {
            char *old = result;
            result = g_strdup_printf(
                "{\"agent_id\":\"%s\",\"pubkey\":\"%s\",\"adopted\":true,\"bunker_uri\":\"%s\",\"delivered\":true,\"delivery_expires_at\":%" G_GINT64_FORMAT "}",
                req.agent_id, pubkey_hex, bunker_uri, (gint64)dm.expires_at);
            g_free(old);
          }
        }
        signet_mgmt_publish_cas_audit(h, "adopt_existing", req.agent_id, "ok", now);
      } else {
        switch (ar) {
          case SIGNET_ADOPT_ERR_INVALID_SECRET:  code = "invalid_secret"; break;
          case SIGNET_ADOPT_ERR_PUBKEY_MISMATCH: code = "pubkey_mismatch"; break;
          case SIGNET_ADOPT_ERR_AGENT_EXISTS:    code = "agent_exists"; break;
          case SIGNET_ADOPT_ERR_PUBKEY_EXISTS:   code = "pubkey_exists"; break;
          default:                               code = "adopt_failed"; break;
        }
        message = g_strdup_printf("failed to adopt agent (%s)", code);
        signet_mgmt_publish_cas_audit(h, "adopt_existing", req.agent_id, code, now);
      }
      g_free(bunker_uri);
      break;
    }

    case SIGNET_MGMT_OP_REVOKE_AGENT: {
      /* Resolve the agent pubkey BEFORE revocation wipes the key, so we can
       * add it to the deny list. Absence of a pubkey means the agent is not
       * known -> not_found. */
      char agent_pubkey_hex[65];
      bool have_pk = signet_key_store_get_agent_pubkey(
          h->keys, req.agent_id, agent_pubkey_hex, sizeof(agent_pubkey_hex));

      if (!have_pk) {
        code = "not_found";
        message = g_strdup("agent not found");
        break;
      }

      SignetStore *base_store = signet_key_store_get_store(h->keys);
      int rc;
      if (base_store) {
        /* Full revocation: deny list + lease burn + key wipe + audit.
         * Deny-list precedence then rejects any residual session/lease. */
        rc = signet_revoke_agent(base_store, h->keys, h->deny, h->audit,
                                 req.agent_id, agent_pubkey_hex,
                                 "management revoke_agent", now);
      } else {
        /* Cache-only mode: no persistent store/deny list is available. */
        rc = signet_key_store_revoke_agent(h->keys, req.agent_id);
        if (rc == 1) rc = 0; /* pubkey resolved above, so it existed */
      }

      if (rc == 0) {
        ok = true;
        signet_mgmt_publish_cas_audit(h, "revoke", req.agent_id, "ok", now);
        code = "revoked";
        message = base_store
            ? g_strdup_printf("agent %s revoked (deny-listed, leases burned)", req.agent_id)
            : g_strdup_printf("agent %s revoked (cache-only)", req.agent_id);
      } else {
        code = "revoke_failed";
        message = g_strdup("failed to revoke agent");
      }
      break;
    }

    case SIGNET_MGMT_OP_GET_STATUS: {
      ok = true;
      code = "ok";
      uint32_t cache_count = signet_key_store_cache_count(h->keys);
      bool db_open = signet_key_store_is_open(h->keys);
      bool relays_ok = signet_relay_pool_is_connected(h->relays);
      result = g_strdup_printf("{\"db_open\":%s,\"agents\":%u,\"relays_connected\":%s}",
                                db_open ? "true" : "false",
                                cache_count,
                                relays_ok ? "true" : "false");
      break;
    }

    case SIGNET_MGMT_OP_LIST_AGENTS: {
      char **agent_ids = NULL;
      size_t agent_count = 0;
      int lrc = signet_key_store_list_agents(h->keys, &agent_ids, &agent_count);
      if (lrc == 0) {
        ok = true;
        code = "ok";
        /* Build JSON array of agent IDs. */
        GString *arr = g_string_new("[");
        for (size_t i = 0; i < agent_count; i++) {
          if (i > 0) g_string_append_c(arr, ',');
          g_string_append_printf(arr, "\"%s\"", agent_ids[i]);
        }
        g_string_append_c(arr, ']');
        result = g_strdup_printf("{\"count\":%zu,\"agents\":%s}",
                                  agent_count, arr->str);
        g_string_free(arr, TRUE);
        g_strfreev(agent_ids);
      } else {
        code = "list_failed";
        message = g_strdup("failed to list agents");
      }
      break;
    }

    case SIGNET_MGMT_OP_SET_POLICY: {
      if (!h->policy_store) {
        code = "no_policy_store";
        message = g_strdup("policy store not configured");
        break;
      }
      /* Parse and apply the submitted policy JSON for the target agent. */
      char *policy_err = NULL;
      int prc = signet_policy_store_set_identity_json(
          h->policy_store, req.agent_id, req.policy_json, now, &policy_err);
      if (prc == 0) {
        ok = true;
        if (policy_err) {
          /* In-memory update succeeded but durable persistence FAILED. Signal
           * this distinctly (not "policy_set") so automation does not assume
           * the change survives a restart. */
          code = "policy_set_not_persisted";
          message = g_strdup_printf("policy updated in memory for agent %s but NOT persisted: %s",
                                    req.agent_id, policy_err);
        } else {
          code = "policy_set";
          message = g_strdup_printf("policy updated for agent %s", req.agent_id);
        }
        result = g_strdup(req.policy_json);
      } else {
        code = "policy_parse_failed";
        message = g_strdup_printf("failed to parse/apply policy: %s",
                                   policy_err ? policy_err : "unknown error");
      }
      g_free(policy_err);
      break;
    }

    case SIGNET_MGMT_OP_ROTATE_KEY: {
      char new_pubkey_hex[65];
      int rrc = signet_key_store_rotate_agent(h->keys, req.agent_id,
                                              new_pubkey_hex, sizeof(new_pubkey_hex));
      if (rrc == 0) {
        ok = true;
        signet_mgmt_publish_cas_audit(h, "rotate", req.agent_id, "ok", now);
        code = "key_rotated";
        message = g_strdup_printf("key rotated for agent %s", req.agent_id);
        result = g_strdup_printf("{\"agent_id\":\"%s\",\"new_pubkey\":\"%s\"}",
                                  req.agent_id, new_pubkey_hex);
      } else if (rrc == 1) {
        code = "not_found";
        message = g_strdup("agent not found");
      } else {
        code = "rotate_failed";
        message = g_strdup("failed to rotate key");
      }
      break;
    }

    case SIGNET_MGMT_OP_REISSUE_CONNECT: {
      /* Authorization already enforced in step 2a: a non-provisioner sender
       * reaching this point IS the target agent. */
      const char *auth_path = sender_is_provisioner ? "provisioner" : "self";

      char user_pubkey_hex[65] = {0};
      char *fresh_secret = NULL;
      char *bunker_uri = NULL;
      /* Self-service pins the authenticated sender as the required identity;
       * the key store re-verifies it ATOMICALLY with the mutation so a
       * concurrent rotate between step 2a and here cannot let the superseded
       * key mint a secret for the new identity. */
      int rrc = signet_key_store_reissue_connect_secret(
          h->keys, req.agent_id,
          sender_is_provisioner ? NULL : event_pubkey_hex,
          h->bunker_pk_hex,
          (const char *const *)h->relay_urls, h->n_relay_urls,
          user_pubkey_hex, &fresh_secret, &bunker_uri);
      if (rrc == 0 && fresh_secret) {
        ok = true;
        code = "connect_reissued";
        message = g_strdup_printf("fresh connect_secret issued for agent %s", req.agent_id);
        /* Build the result with JsonBuilder (not printf) so agent_id and the
         * bunker URI are always correctly JSON-escaped — a malformed result
         * would be silently dropped by signet_mgmt_build_ack and the fresh
         * secret never disclosed. */
        JsonBuilder *rb = json_builder_new();
        if (rb) {
          json_builder_begin_object(rb);
          json_builder_set_member_name(rb, "agent_id");
          json_builder_add_string_value(rb, req.agent_id);
          json_builder_set_member_name(rb, "bunker_pubkey");
          json_builder_add_string_value(rb, h->bunker_pk_hex ? h->bunker_pk_hex : "");
          json_builder_set_member_name(rb, "user_pubkey");
          json_builder_add_string_value(rb, user_pubkey_hex);
          json_builder_set_member_name(rb, "connect_secret");
          json_builder_add_string_value(rb, fresh_secret);
          json_builder_set_member_name(rb, "bunker_uri");
          if (bunker_uri) json_builder_add_string_value(rb, bunker_uri);
          else json_builder_add_null_value(rb);
          json_builder_set_member_name(rb, "issued_at");
          json_builder_add_int_value(rb, now);
          json_builder_end_object(rb);
          JsonNode *rroot = json_builder_get_root(rb);
          JsonGenerator *rgen = json_generator_new();
          if (rgen && rroot) {
            json_generator_set_root(rgen, rroot);
            json_generator_set_pretty(rgen, FALSE);
            result = json_generator_to_data(rgen, NULL);
          }
          if (rgen) g_object_unref(rgen);
          if (rroot) json_node_free(rroot);
          g_object_unref(rb);
        }
        signet_mgmt_publish_cas_audit(h, "reissue_connect", req.agent_id,
                                      (strcmp(auth_path, "self") == 0)
                                          ? "ok_self" : "ok_provisioner",
                                      now);
      } else if (rrc == 1) {
        code = "not_found";
        message = g_strdup("agent not found");
        signet_mgmt_publish_cas_audit(h, "reissue_connect", req.agent_id, "not_found", now);
      } else if (rrc == 2) {
        /* Identity changed between authorization and execution (concurrent
         * rotate). The superseded key is no longer this agent. */
        code = "unauthorized";
        message = g_strdup("sender is neither a provisioner nor the target agent");
      } else {
        code = "reissue_failed";
        message = g_strdup("failed to reissue connect secret");
        signet_mgmt_publish_cas_audit(h, "reissue_connect", req.agent_id, "error", now);
      }
      if (fresh_secret) { secure_wipe(fresh_secret, strlen(fresh_secret)); g_free(fresh_secret); }
      if (bunker_uri) { secure_wipe(bunker_uri, strlen(bunker_uri)); g_free(bunker_uri); }
      break;
    }

    case SIGNET_MGMT_OP_LIST_CLIENTS: {
      SignetStore *base_store = signet_key_store_get_store(h->keys);
      if (!base_store) {
        code = "no_store";
        message = g_strdup("persistent store not available");
        break;
      }
      SignetClientBinding *list = NULL;
      size_t count = 0;
      if (signet_store_list_clients(base_store, req.agent_id, &list, &count) != 0) {
        code = "list_failed";
        message = g_strdup("failed to list client bindings");
        break;
      }
      JsonBuilder *lb = json_builder_new();
      if (lb) {
        json_builder_begin_object(lb);
        json_builder_set_member_name(lb, "agent_id");
        json_builder_add_string_value(lb, req.agent_id);
        json_builder_set_member_name(lb, "count");
        json_builder_add_int_value(lb, (gint64)count);
        json_builder_set_member_name(lb, "clients");
        json_builder_begin_array(lb);
        for (size_t i = 0; i < count; i++) {
          json_builder_begin_object(lb);
          json_builder_set_member_name(lb, "client_pubkey");
          json_builder_add_string_value(lb, list[i].client_pubkey ? list[i].client_pubkey : "");
          json_builder_set_member_name(lb, "bound_at");
          json_builder_add_int_value(lb, list[i].bound_at);
          json_builder_set_member_name(lb, "last_used");
          json_builder_add_int_value(lb, list[i].last_used);
          json_builder_set_member_name(lb, "active");
          json_builder_add_boolean_value(lb, list[i].revoked_at == 0);
          if (list[i].revoked_at != 0) {
            json_builder_set_member_name(lb, "revoked_at");
            json_builder_add_int_value(lb, list[i].revoked_at);
          }
          json_builder_end_object(lb);
        }
        json_builder_end_array(lb);
        json_builder_end_object(lb);
        JsonNode *lroot = json_builder_get_root(lb);
        JsonGenerator *lgen = json_generator_new();
        if (lgen && lroot) {
          json_generator_set_root(lgen, lroot);
          json_generator_set_pretty(lgen, FALSE);
          result = json_generator_to_data(lgen, NULL);
        }
        if (lgen) g_object_unref(lgen);
        if (lroot) json_node_free(lroot);
        g_object_unref(lb);
      }
      signet_client_binding_list_free(list, count);
      if (result) {
        ok = true;
        code = "ok";
      } else {
        code = "internal_error";
        message = g_strdup("failed to build client list");
      }
      break;
    }

    case SIGNET_MGMT_OP_REVOKE_CLIENT: {
      SignetStore *base_store = signet_key_store_get_store(h->keys);
      if (!base_store) {
        code = "no_store";
        message = g_strdup("persistent store not available");
        break;
      }
      int crc = signet_store_revoke_client(base_store, req.client_pubkey, now);
      if (crc == 0) {
        ok = true;
        code = "client_revoked";
        message = g_strdup("client binding revoked");
        signet_mgmt_publish_cas_audit(h, "revoke_client",
                                      req.agent_id ? req.agent_id : "", "ok", now);
      } else if (crc == 1) {
        code = "not_found";
        message = g_strdup("client binding not found or already revoked");
      } else {
        code = "revoke_client_failed";
        message = g_strdup("failed to revoke client binding");
      }
      break;
    }

    default:
      code = "unknown_command";
      message = g_strdup("unknown management command");
      break;
  }

  /* 4) Publish ack. */
  char *ack = signet_mgmt_build_ack(req.request_id, ok, code, message, result);
  if (ack) {
    signet_mgmt_publish_ack(h, event_pubkey_hex, ack, event_id_hex, now);
    /* ack may embed secrets carried in result — wipe before free. */
    secure_wipe(ack, strlen(ack));
    g_free(ack);
  }

  g_free(message);
  /* result may embed a fresh connect_secret/bunker URI (reissue-connect,
   * provision, adopt-existing) — wipe before free. */
  if (result) { secure_wipe(result, strlen(result)); }
  g_free(result);
  /* decrypted_content may hold agent_nsec/connect_secret (adopt-existing). */
  if (decrypted_content) { secure_wipe(decrypted_content, strlen(decrypted_content)); free(decrypted_content); }
  signet_mgmt_request_clear(&req);

  return ok ? 0 : -1;
}
