/* SPDX-License-Identifier: MIT
 *
 * mgmt_protocol.c - Signed management protocol helpers (Phase 5).
 *
 * This module:
 * - Parses management command JSON (event content)
 * - Validates required fields by command type
 * - Provides admin authorization helper (pubkey whitelist)
 * - Builds response JSON (event content) with json-glib (safe escaping)
 *
 * It does NOT:
 * - Verify NIP-01 event signatures (caller must verify via libnostr)
 * - Sign response events (caller/daemon signs and publishes)
 */

#include "signet/mgmt_protocol.h"

#include <string.h>

#include <glib.h>
#include <json-glib/json-glib.h>

static char *signet_strdup0(const char *s) {
  if (!s || s[0] == '\0') return NULL;
  return g_strdup(s);
}

static gboolean signet_streq_ci(const char *a, const char *b) {
  if (!a || !b) return FALSE;
  return g_ascii_strcasecmp(a, b) == 0;
}

SignetMgmtOp signet_mgmt_op_from_string(const char *op) {
  if (!op) return SIGNET_MGMT_OP_UNKNOWN;

  /* Preferred command set */
  if (strcmp(op, "add_policy") == 0) return SIGNET_MGMT_OP_ADD_POLICY;
  if (strcmp(op, "revoke_policy") == 0) return SIGNET_MGMT_OP_REVOKE_POLICY;
  if (strcmp(op, "list_policies") == 0) return SIGNET_MGMT_OP_LIST_POLICIES;
  if (strcmp(op, "rotate_key") == 0) return SIGNET_MGMT_OP_ROTATE_KEY;
  if (strcmp(op, "health_check") == 0) return SIGNET_MGMT_OP_HEALTH_CHECK;

  /* Legacy */
  if (strcmp(op, "policy.set") == 0) return SIGNET_MGMT_OP_POLICY_SET;
  if (strcmp(op, "policy.unset") == 0) return SIGNET_MGMT_OP_POLICY_UNSET;
  if (strcmp(op, "client.revoke") == 0) return SIGNET_MGMT_OP_CLIENT_REVOKE;
  if (strcmp(op, "client.unrevoke") == 0) return SIGNET_MGMT_OP_CLIENT_UNREVOKE;

  return SIGNET_MGMT_OP_UNKNOWN;
}

const char *signet_mgmt_op_to_string(SignetMgmtOp op) {
  switch (op) {
    case SIGNET_MGMT_OP_ADD_POLICY: return "add_policy";
    case SIGNET_MGMT_OP_REVOKE_POLICY: return "revoke_policy";
    case SIGNET_MGMT_OP_LIST_POLICIES: return "list_policies";
    case SIGNET_MGMT_OP_ROTATE_KEY: return "rotate_key";
    case SIGNET_MGMT_OP_HEALTH_CHECK: return "health_check";

    /* Legacy map to canonical names where possible */
    case SIGNET_MGMT_OP_POLICY_SET: return "add_policy";
    case SIGNET_MGMT_OP_POLICY_UNSET: return "revoke_policy";
    case SIGNET_MGMT_OP_CLIENT_REVOKE: return "client.revoke";
    case SIGNET_MGMT_OP_CLIENT_UNREVOKE: return "client.unrevoke";

    default: return "unknown";
  }
}

bool signet_mgmt_admin_is_authorized(const char *event_pubkey_hex,
                                     const char *const *admin_pubkeys,
                                     size_t n_admin_pubkeys) {
  if (!event_pubkey_hex || event_pubkey_hex[0] == '\0') return false;
  if (!admin_pubkeys || n_admin_pubkeys == 0) return false;

  for (size_t i = 0; i < n_admin_pubkeys; i++) {
    const char *a = admin_pubkeys[i];
    if (!a || a[0] == '\0') continue;
    if (signet_streq_ci(a, event_pubkey_hex)) return true;
  }
  return false;
}

void signet_mgmt_request_clear(SignetMgmtRequest *req) {
  if (!req) return;
  g_clear_pointer(&req->command, g_free);
  g_clear_pointer(&req->identity, g_free);
  g_clear_pointer(&req->policy_json, g_free);
  req->op = SIGNET_MGMT_OP_UNKNOWN;
}

static int signet_mgmt_error(char **out_error, const char *msg) {
  if (out_error) *out_error = msg ? g_strdup(msg) : g_strdup("management parse error");
  return -1;
}

static char *signet_json_node_to_compact_string(JsonNode *node) {
  if (!node) return NULL;

  JsonGenerator *gen = json_generator_new();
  if (!gen) return NULL;

  json_generator_set_pretty(gen, FALSE);
  json_generator_set_root(gen, node);
  char *s = json_generator_to_data(gen, NULL);

  g_object_unref(gen);
  return s; /* must be g_free() */
}

int signet_mgmt_request_parse_content_json(const char *content_json,
                                          SignetMgmtRequest *out_req,
                                          char **out_error) {
  if (!content_json || !out_req) return signet_mgmt_error(out_error, "missing content JSON");

  signet_mgmt_request_clear(out_req);

  JsonParser *p = json_parser_new();
  if (!p) return signet_mgmt_error(out_error, "OOM creating JSON parser");

  GError *err = NULL;
  if (!json_parser_load_from_data(p, content_json, -1, &err)) {
    if (err) {
      int rc = signet_mgmt_error(out_error, err->message ? err->message : "invalid JSON");
      g_error_free(err);
      g_object_unref(p);
      return rc;
    }
    g_object_unref(p);
    return signet_mgmt_error(out_error, "invalid JSON");
  }

  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(p);
    return signet_mgmt_error(out_error, "content must be a JSON object");
  }

  JsonObject *o = json_node_get_object(root);
  if (!o) {
    g_object_unref(p);
    return signet_mgmt_error(out_error, "content must be a JSON object");
  }

  const char *cmd = NULL;
  if (json_object_has_member(o, "command")) cmd = json_object_get_string_member(o, "command");
  if (!cmd || cmd[0] == '\0') {
    g_object_unref(p);
    return signet_mgmt_error(out_error, "missing required field: command");
  }

  out_req->op = signet_mgmt_op_from_string(cmd);
  out_req->command = g_strdup(cmd);
  if (!out_req->command) {
    g_object_unref(p);
    return signet_mgmt_error(out_error, "OOM copying command");
  }

  /* Optional identity */
  if (json_object_has_member(o, "identity")) {
    const char *ident = json_object_get_string_member(o, "identity");
    out_req->identity = signet_strdup0(ident);
  }

  /* Optional policy */
  if (json_object_has_member(o, "policy")) {
    JsonNode *pn = json_object_get_member(o, "policy");
    if (pn && JSON_NODE_HOLDS_OBJECT(pn)) {
      out_req->policy_json = signet_json_node_to_compact_string(pn);
      if (!out_req->policy_json) {
        g_object_unref(p);
        return signet_mgmt_error(out_error, "OOM serializing policy");
      }
    } else if (pn) {
      g_object_unref(p);
      return signet_mgmt_error(out_error, "policy must be a JSON object");
    }
  }

  /* Validation rules */
  switch (out_req->op) {
    case SIGNET_MGMT_OP_ADD_POLICY:
      if (!out_req->identity || out_req->identity[0] == '\0') {
        g_object_unref(p);
        return signet_mgmt_error(out_error, "add_policy requires identity");
      }
      if (!out_req->policy_json || out_req->policy_json[0] == '\0') {
        g_object_unref(p);
        return signet_mgmt_error(out_error, "add_policy requires policy object");
      }
      break;

    case SIGNET_MGMT_OP_REVOKE_POLICY:
    case SIGNET_MGMT_OP_ROTATE_KEY:
      if (!out_req->identity || out_req->identity[0] == '\0') {
        g_object_unref(p);
        return signet_mgmt_error(out_error, "command requires identity");
      }
      break;

    case SIGNET_MGMT_OP_LIST_POLICIES:
    case SIGNET_MGMT_OP_HEALTH_CHECK:
      /* no extra required fields */
      break;

    case SIGNET_MGMT_OP_POLICY_SET:
    case SIGNET_MGMT_OP_POLICY_UNSET:
    case SIGNET_MGMT_OP_CLIENT_REVOKE:
    case SIGNET_MGMT_OP_CLIENT_UNREVOKE:
      /* Legacy ops: treat as syntactically valid; semantics handled elsewhere. */
      break;

    default:
      g_object_unref(p);
      return signet_mgmt_error(out_error, "unknown command");
  }

  g_object_unref(p);
  return 0;
}

char *signet_mgmt_build_response_json(const char *status,
                                      const char *command,
                                      const char *code,
                                      const char *message,
                                      const char *result_json) {
  if (!status || !command) return NULL;

  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;

  json_builder_begin_object(b);

  json_builder_set_member_name(b, "status");
  json_builder_add_string_value(b, status);

  json_builder_set_member_name(b, "command");
  json_builder_add_string_value(b, command);

  if (code) {
    json_builder_set_member_name(b, "code");
    json_builder_add_string_value(b, code);
  }

  if (message) {
    json_builder_set_member_name(b, "message");
    json_builder_add_string_value(b, message);
  }

  if (result_json) {
    JsonParser *p = json_parser_new();
    if (!p) {
      g_object_unref(b);
      return NULL;
    }

    GError *err = NULL;
    if (json_parser_load_from_data(p, result_json, -1, &err)) {
      JsonNode *r = json_parser_get_root(p);
      if (r) {
        json_builder_set_member_name(b, "result");
        json_builder_add_value(b, json_node_copy(r));
      }
    } else {
      if (err) g_error_free(err);
      /* If result_json is invalid JSON, omit result rather than emitting unsafe text. */
    }

    g_object_unref(p);
  }

  json_builder_end_object(b);

  JsonGenerator *g = json_generator_new();
  if (!g) {
    g_object_unref(b);
    return NULL;
  }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  json_generator_set_pretty(g, FALSE);

  char *out = json_generator_to_data(g, NULL);

  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);

  return out; /* must be g_free() */
}

char *signet_mgmt_build_ack_json(bool ok, const char *code, const char *message) {
  /* Backwards compatible: old code expected {"ok":bool,"code":"..","message":".."} */
  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;

  json_builder_begin_object(b);

  json_builder_set_member_name(b, "ok");
  json_builder_add_boolean_value(b, ok ? TRUE : FALSE);

  json_builder_set_member_name(b, "code");
  json_builder_add_string_value(b, code ? code : "");

  json_builder_set_member_name(b, "message");
  json_builder_add_string_value(b, message ? message : "");

  json_builder_end_object(b);

  JsonGenerator *g = json_generator_new();
  if (!g) {
    g_object_unref(b);
    return NULL;
  }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  json_generator_set_pretty(g, FALSE);

  char *out = json_generator_to_data(g, NULL);

  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);

  return out; /* must be g_free() */
}