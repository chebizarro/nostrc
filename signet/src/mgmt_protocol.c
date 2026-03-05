/* SPDX-License-Identifier: MIT
 *
 * mgmt_protocol.c - Nostr-native management protocol for Signet.
 *
 * Handles management events (kinds 28000-28090) for agent provisioning,
 * revocation, policy updates, and status queries. All operations execute
 * against the SignetKeyStore (SQLCipher + hot cache).
 */

#include "signet/mgmt_protocol.h"
#include "signet/key_store.h"
#include "signet/relay_pool.h"
#include "signet/audit_logger.h"

#include <string.h>
#include <time.h>

#include <glib.h>
#include <json-glib/json-glib.h>

/* libnostr */
#include <nostr-event.h>
#include <nostr-keys.h>

/* ----------------------------- op mapping -------------------------------- */

SignetMgmtOp signet_mgmt_op_from_kind(int kind) {
  switch (kind) {
    case SIGNET_KIND_PROVISION_AGENT: return SIGNET_MGMT_OP_PROVISION_AGENT;
    case SIGNET_KIND_REVOKE_AGENT:   return SIGNET_MGMT_OP_REVOKE_AGENT;
    case SIGNET_KIND_SET_POLICY:     return SIGNET_MGMT_OP_SET_POLICY;
    case SIGNET_KIND_GET_STATUS:     return SIGNET_MGMT_OP_GET_STATUS;
    case SIGNET_KIND_LIST_AGENTS:    return SIGNET_MGMT_OP_LIST_AGENTS;
    case SIGNET_KIND_ROTATE_KEY:     return SIGNET_MGMT_OP_ROTATE_KEY;
    default:                         return SIGNET_MGMT_OP_UNKNOWN;
  }
}

const char *signet_mgmt_op_to_string(SignetMgmtOp op) {
  switch (op) {
    case SIGNET_MGMT_OP_PROVISION_AGENT: return "provision_agent";
    case SIGNET_MGMT_OP_REVOKE_AGENT:   return "revoke_agent";
    case SIGNET_MGMT_OP_SET_POLICY:      return "set_policy";
    case SIGNET_MGMT_OP_GET_STATUS:      return "get_status";
    case SIGNET_MGMT_OP_LIST_AGENTS:     return "list_agents";
    case SIGNET_MGMT_OP_ROTATE_KEY:      return "rotate_key";
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
  memset(req, 0, sizeof(*req));
}

int signet_mgmt_request_parse(int kind,
                              const char *content_json,
                              SignetMgmtRequest *out_req,
                              char **out_error) {
  if (!out_req) {
    if (out_error) *out_error = g_strdup("null output");
    return -1;
  }
  memset(out_req, 0, sizeof(*out_req));

  out_req->op = signet_mgmt_op_from_kind(kind);
  out_req->kind = kind;

  if (out_req->op == SIGNET_MGMT_OP_UNKNOWN) {
    if (out_error) *out_error = g_strdup("unknown management event kind");
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

  /* Validate required fields per op. */
  bool needs_agent_id = (out_req->op == SIGNET_MGMT_OP_PROVISION_AGENT ||
                         out_req->op == SIGNET_MGMT_OP_REVOKE_AGENT ||
                         out_req->op == SIGNET_MGMT_OP_SET_POLICY ||
                         out_req->op == SIGNET_MGMT_OP_ROTATE_KEY);

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

  char **provisioner_pubkeys;
  size_t n_provisioner_pubkeys;

  char *bunker_sk_hex;
  char *bunker_pk_hex;
};

SignetMgmtHandler *signet_mgmt_handler_new(SignetKeyStore *keys,
                                           SignetRelayPool *relays,
                                           SignetAuditLogger *audit,
                                           const SignetMgmtHandlerConfig *cfg) {
  if (!cfg) return NULL;

  SignetMgmtHandler *h = (SignetMgmtHandler *)g_new0(SignetMgmtHandler, 1);
  if (!h) return NULL;

  h->keys = keys;
  h->relays = relays;
  h->audit = audit;

  if (cfg->provisioner_pubkeys && cfg->n_provisioner_pubkeys > 0) {
    h->provisioner_pubkeys = (char **)g_new0(char *, cfg->n_provisioner_pubkeys);
    for (size_t i = 0; i < cfg->n_provisioner_pubkeys; i++) {
      h->provisioner_pubkeys[i] = g_strdup(cfg->provisioner_pubkeys[i]);
    }
    h->n_provisioner_pubkeys = cfg->n_provisioner_pubkeys;
  }

  h->bunker_sk_hex = g_strdup(cfg->bunker_secret_key_hex);
  h->bunker_pk_hex = g_strdup(cfg->bunker_pubkey_hex);

  return h;
}

void signet_mgmt_handler_free(SignetMgmtHandler *h) {
  if (!h) return;

  for (size_t i = 0; i < h->n_provisioner_pubkeys; i++) g_free(h->provisioner_pubkeys[i]);
  g_free(h->provisioner_pubkeys);

  if (h->bunker_sk_hex) {
    memset(h->bunker_sk_hex, 0, strlen(h->bunker_sk_hex));
    g_free(h->bunker_sk_hex);
  }
  g_free(h->bunker_pk_hex);
  g_free(h);
}

/* Publish an ack event to relays. */
static void signet_mgmt_publish_ack(SignetMgmtHandler *h,
                                    const char *recipient_pubkey_hex,
                                    const char *ack_content,
                                    const char *ref_event_id_hex,
                                    int64_t now) {
  if (!h || !h->relays || !h->bunker_sk_hex || !ack_content) return;

  NostrEvent *evt = nostr_event_new();
  if (!evt) return;

  nostr_event_set_kind(evt, SIGNET_KIND_MGMT_ACK);
  nostr_event_set_created_at(evt, now);
  nostr_event_set_content(evt, ack_content);

  NostrTags *tags = nostr_tags_new();
  if (tags) {
    if (recipient_pubkey_hex) {
      const char *p_tag[] = {"p", recipient_pubkey_hex};
      nostr_tags_add(tags, p_tag, 2);
    }
    if (ref_event_id_hex) {
      const char *e_tag[] = {"e", ref_event_id_hex};
      nostr_tags_add(tags, e_tag, 2);
    }
    nostr_event_set_tags(evt, tags);
  }

  if (nostr_event_sign(evt, h->bunker_sk_hex) == 0) {
    char *json = nostr_event_serialize_compact(evt);
    if (json) {
      signet_relay_pool_publish_event_json(h->relays, json);
      free(json);
    }
  }

  nostr_event_free(evt);
}

int signet_mgmt_handler_handle_event(SignetMgmtHandler *h,
                                     const char *event_pubkey_hex,
                                     const char *content_json,
                                     int kind,
                                     const char *event_id_hex,
                                     int64_t now) {
  if (!h) return -1;

  /* 1) Authorization check. */
  if (!signet_mgmt_is_authorized(event_pubkey_hex,
                                 (const char *const *)h->provisioner_pubkeys,
                                 h->n_provisioner_pubkeys)) {
    return -1; /* silently drop unauthorized events */
  }

  /* 2) Parse request. */
  SignetMgmtRequest req;
  char *parse_err = NULL;
  if (signet_mgmt_request_parse(kind, content_json, &req, &parse_err) != 0) {
    char *ack = signet_mgmt_build_ack(NULL, false, "parse_error",
                                       parse_err ? parse_err : "invalid request", NULL);
    if (ack) {
      signet_mgmt_publish_ack(h, event_pubkey_hex, ack, event_id_hex, now);
      g_free(ack);
    }
    g_free(parse_err);
    return -1;
  }

  /* 3) Execute command. */
  bool ok = false;
  const char *code = "internal_error";
  char *message = NULL;
  char *result = NULL;

  switch (req.op) {
    case SIGNET_MGMT_OP_PROVISION_AGENT: {
      char pubkey_hex[65];
      int rc = signet_key_store_provision_agent(h->keys, req.agent_id, pubkey_hex, sizeof(pubkey_hex));
      if (rc == 0) {
        ok = true;
        code = "provisioned";
        message = g_strdup_printf("agent %s provisioned", req.agent_id);
        result = g_strdup_printf("{\"agent_id\":\"%s\",\"pubkey\":\"%s\"}", req.agent_id, pubkey_hex);
      } else {
        code = "provision_failed";
        message = g_strdup("failed to provision agent");
      }
      break;
    }

    case SIGNET_MGMT_OP_REVOKE_AGENT: {
      int rc = signet_key_store_revoke_agent(h->keys, req.agent_id);
      if (rc == 0) {
        ok = true;
        code = "revoked";
        message = g_strdup_printf("agent %s revoked", req.agent_id);
      } else if (rc == 1) {
        code = "not_found";
        message = g_strdup("agent not found");
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
      /* Return list of agent IDs from cache count for now.
       * Full list requires iterating the store — deferred to store API. */
      ok = true;
      code = "ok";
      uint32_t count = signet_key_store_cache_count(h->keys);
      result = g_strdup_printf("{\"count\":%u}", count);
      break;
    }

    case SIGNET_MGMT_OP_SET_POLICY:
    case SIGNET_MGMT_OP_ROTATE_KEY:
      /* These require more complex logic — mark as not-yet-implemented. */
      code = "not_implemented";
      message = g_strdup("command not yet implemented");
      break;

    default:
      code = "unknown_command";
      message = g_strdup("unknown management command");
      break;
  }

  /* 4) Publish ack. */
  char *ack = signet_mgmt_build_ack(req.request_id, ok, code, message, result);
  if (ack) {
    signet_mgmt_publish_ack(h, event_pubkey_hex, ack, event_id_hex, now);
    g_free(ack);
  }

  g_free(message);
  g_free(result);
  signet_mgmt_request_clear(&req);

  return ok ? 0 : -1;
}