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
#include "signet/policy_store.h"
#include "signet/relay_pool.h"
#include "signet/audit_logger.h"
#include "signet/util.h"

#include <nostr/nip44/nip44.h>
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
  SignetPolicyStore *policy_store;

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

  /* NIP-44 v2 encrypt the ack content to the recipient. */
  char *encrypted_content = NULL;
  if (recipient_pubkey_hex && recipient_pubkey_hex[0]) {
    uint8_t sk[32], pk[32];
    if (signet_mgmt_hex_to_bytes32(h->bunker_sk_hex, sk) &&
        signet_mgmt_hex_to_bytes32(recipient_pubkey_hex, pk)) {
      int erc = nostr_nip44_encrypt_v2(sk, pk,
                                       (const uint8_t *)ack_content, strlen(ack_content),
                                       &encrypted_content);
      signet_mgmt_memzero(sk, 32);
      if (erc != 0) encrypted_content = NULL;
    }
  }
  nostr_event_set_content(evt, encrypted_content ? encrypted_content : ack_content);

  NostrTags *tags = nostr_tags_new(0);
  if (tags) {
    if (recipient_pubkey_hex) {
      NostrTag *p_tag = nostr_tag_new("p", recipient_pubkey_hex, NULL);
      if (p_tag) {
        nostr_tags_append(tags, p_tag);
      }
    }
    if (ref_event_id_hex) {
      NostrTag *e_tag = nostr_tag_new("e", ref_event_id_hex, NULL);
      if (e_tag) {
        nostr_tags_append(tags, e_tag);
      }
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

  free(encrypted_content);
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
      free(pt);
      return -1;
    }

    decrypted_content = (char *)malloc(pt_len + 1);
    if (!decrypted_content) {
      free(pt);
      return -1;
    }
    memcpy(decrypted_content, pt, pt_len);
    decrypted_content[pt_len] = '\0';
    effective_content = decrypted_content;
    free(pt);
  }

  /* 2) Parse request. */
  SignetMgmtRequest req;
  char *parse_err = NULL;
  if (signet_mgmt_request_parse(kind, effective_content, &req, &parse_err) != 0) {
    char *ack = signet_mgmt_build_ack(NULL, false, "parse_error",
                                       parse_err ? parse_err : "invalid request", NULL);
    if (ack) {
      signet_mgmt_publish_ack(h, event_pubkey_hex, ack, event_id_hex, now);
      g_free(ack);
    }
    g_free(parse_err);
    free(decrypted_content);
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
        g_free(bunker_uri);
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
        code = "policy_set";
        if (policy_err) {
          /* In-memory update succeeded but persistence failed. */
          message = g_strdup_printf("policy updated for agent %s (warning: %s)",
                                    req.agent_id, policy_err);
        } else {
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
  free(decrypted_content);
  signet_mgmt_request_clear(&req);

  return ok ? 0 : -1;
}