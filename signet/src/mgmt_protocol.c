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
#include "signet/signet_config.h"
#include "signet/store.h"
#include "signet/store_audit.h"
#include "signet/store_secrets.h"
#include "signet/credential_access.h"
#include "signet/capability.h"
#include "signet/bootstrap_delivery.h"
#include "signet/util.h"
#ifdef SIGNET_ENABLE_TEST_HOOKS
#include "mgmt_protocol_test_hooks.h"
#endif

#include <nostr/nip17/nip17.h>
#include <nostr/nip44/nip44.h>
#include <nostr/nip19/nip19.h>
#include <secure_buf.h>
#include <sodium.h>

#include <limits.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <json-glib/json-glib.h>

/* libnostr */
#include <nostr-event.h>
#include <nostr-keys.h>

/* ---- small helpers (also in nip46_server.c; duplicated to stay static) ---- */

static void signet_mgmt_chain_audit(SignetMgmtHandler *h,
                                    const char *operation,
                                    const char *identity,
                                    const char *secret_id,
                                    const char *decision,
                                    const char *reason,
                                    int64_t now);
static void signet_mgmt_chain_audit_ex(SignetMgmtHandler *h,
                                       const char *operation,
                                       const char *identity,
                                       const char *secret_id,
                                       const char *decision,
                                       const char *reason,
                                       const char *auth,
                                       const char *owner,
                                       int64_t now);

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
    case SIGNET_MGMT_OP_RESTORE_EXISTING:return "restore_existing";
    case SIGNET_MGMT_OP_REISSUE_CONNECT: return "reissue_connect";
    case SIGNET_MGMT_OP_LIST_CLIENTS:    return "list_clients";
    case SIGNET_MGMT_OP_REVOKE_CLIENT:   return "revoke_client";
    case SIGNET_MGMT_OP_CREATE_CREDENTIAL: return "create_credential";
    case SIGNET_MGMT_OP_IMPORT_CREDENTIAL: return "import_credential";
    case SIGNET_MGMT_OP_LIST_CREDENTIALS: return "list_credentials";
    case SIGNET_MGMT_OP_INSPECT_CREDENTIAL: return "inspect_credential";
    case SIGNET_MGMT_OP_DELIVER_CREDENTIAL: return "deliver_credential";
    case SIGNET_MGMT_OP_ROTATE_CREDENTIAL: return "rotate_credential";
    case SIGNET_MGMT_OP_REVOKE_CREDENTIAL: return "revoke_credential";
    case SIGNET_MGMT_OP_DELETE_CREDENTIAL: return "delete_credential";
    case SIGNET_MGMT_OP_GRANT_PROVISIONER: return "grant_provisioner";
    case SIGNET_MGMT_OP_REVOKE_PROVISIONER: return "revoke_provisioner";
    case SIGNET_MGMT_OP_WRITER_ACQUIRE: return "writer_acquire";
    case SIGNET_MGMT_OP_WRITER_REVOKE: return "writer_revoke";
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
  g_free(req->provisioner_pubkey);
  g_free(req->writer_pubkey);
  g_free(req->credential_id);
  g_free(req->secret_type);
  g_free(req->label);
  if (req->payload_b64) {
    secure_wipe(req->payload_b64, strlen(req->payload_b64));
    g_free(req->payload_b64);
  }
  g_free(req->credential_policy_id);
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
  if (json_object_has_member(o, "provisioner_pubkey")) {
    const char *v = json_object_get_string_member(o, "provisioner_pubkey");
    if (v && v[0]) out_req->provisioner_pubkey = g_strdup(v);
  }
  if (json_object_has_member(o, "writer_pubkey")) {
    const char *v = json_object_get_string_member(o, "writer_pubkey");
    if (v && v[0]) out_req->writer_pubkey = g_strdup(v);
  }
  if (json_object_has_member(o, "credential_id")) {
    const char *v = json_object_get_string_member(o, "credential_id");
    if (v && v[0]) out_req->credential_id = g_strdup(v);
  }
  if (json_object_has_member(o, "secret_type")) {
    const char *v = json_object_get_string_member(o, "secret_type");
    if (v && v[0]) out_req->secret_type = g_strdup(v);
  }
  if (json_object_has_member(o, "label")) {
    const char *v = json_object_get_string_member(o, "label");
    if (v && v[0]) out_req->label = g_strdup(v);
  }
  if (json_object_has_member(o, "payload_b64")) {
    const char *v = json_object_get_string_member(o, "payload_b64");
    if (v && v[0]) out_req->payload_b64 = g_strdup(v);
  }
  if (json_object_has_member(o, "policy_id")) {
    const char *v = json_object_get_string_member(o, "policy_id");
    if (v && v[0]) out_req->credential_policy_id = g_strdup(v);
  }
  if (json_object_has_member(o, "expires_at")) {
    out_req->credential_expires_at =
        (int64_t)json_object_get_int_member(o, "expires_at");
    out_req->has_credential_expires_at = true;
  }
  if (json_object_has_member(o, "expected_version")) {
    JsonNode *vn = json_object_get_member(o, "expected_version");
    gint64 v = 0;
    if (vn && JSON_NODE_HOLDS_VALUE(vn) &&
        json_node_get_value_type(vn) == G_TYPE_INT64)
      v = json_node_get_int(vn);
    if (op != SIGNET_MGMT_OP_DELIVER_CREDENTIAL) {
      if (out_error)
        *out_error =
            g_strdup("expected_version applies only to credential/deliver");
      signet_mgmt_request_clear(out_req);
      return -1;
    }
    if (v < 1 || v > INT_MAX) {
      if (out_error)
        *out_error = g_strdup("expected_version must be a positive integer");
      signet_mgmt_request_clear(out_req);
      return -1;
    }
    out_req->expected_version = (int)v;
  }

  /* Validate required fields per op. */
  bool needs_agent_id = (out_req->op == SIGNET_MGMT_OP_PROVISION_AGENT ||
                         out_req->op == SIGNET_MGMT_OP_REVOKE_AGENT ||
                         out_req->op == SIGNET_MGMT_OP_SET_POLICY ||
                         out_req->op == SIGNET_MGMT_OP_ROTATE_KEY ||
                         out_req->op == SIGNET_MGMT_OP_ADOPT_EXISTING ||
                         out_req->op == SIGNET_MGMT_OP_RESTORE_EXISTING ||
                         out_req->op == SIGNET_MGMT_OP_REISSUE_CONNECT ||
                         out_req->op == SIGNET_MGMT_OP_LIST_CLIENTS ||
                         out_req->op == SIGNET_MGMT_OP_CREATE_CREDENTIAL ||
                         out_req->op == SIGNET_MGMT_OP_IMPORT_CREDENTIAL ||
                         out_req->op == SIGNET_MGMT_OP_DELIVER_CREDENTIAL);
  needs_agent_id = needs_agent_id ||
                   out_req->op == SIGNET_MGMT_OP_WRITER_ACQUIRE ||
                   out_req->op == SIGNET_MGMT_OP_WRITER_REVOKE;

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

  if ((out_req->op == SIGNET_MGMT_OP_ADOPT_EXISTING ||
       out_req->op == SIGNET_MGMT_OP_RESTORE_EXISTING) &&
      (!out_req->agent_nsec || !out_req->agent_nsec[0])) {
    if (out_error) *out_error = g_strdup("identity restore/adopt requires agent_nsec");
    signet_mgmt_request_clear(out_req);
    return -1;
  }

  if (out_req->op == SIGNET_MGMT_OP_REVOKE_CLIENT &&
      (!out_req->client_pubkey || !out_req->client_pubkey[0])) {
    if (out_error) *out_error = g_strdup("revoke_client requires client_pubkey");
    signet_mgmt_request_clear(out_req);
    return -1;
  }

  bool provisioner_mutation =
      out_req->op == SIGNET_MGMT_OP_GRANT_PROVISIONER ||
      out_req->op == SIGNET_MGMT_OP_REVOKE_PROVISIONER;
  if (provisioner_mutation &&
      (!out_req->provisioner_pubkey ||
       !signet_hex_to_bytes32(out_req->provisioner_pubkey, (uint8_t[32]){0}))) {
    if (out_error) *out_error = g_strdup(
        "grant/revoke provisioner requires a 64-hex provisioner_pubkey");
    signet_mgmt_request_clear(out_req);
    return -1;
  }

  if (out_req->op == SIGNET_MGMT_OP_WRITER_ACQUIRE) {
    uint8_t checked[32];
    if (!out_req->writer_pubkey ||
        !signet_hex_to_bytes32(out_req->writer_pubkey, checked)) {
      if (out_error) *out_error = g_strdup(
          "writer-acquire requires writer_pubkey (64 hex)");
      signet_mgmt_request_clear(out_req);
      return -1;
    }
  }

  bool create_or_import =
      out_req->op == SIGNET_MGMT_OP_CREATE_CREDENTIAL ||
      out_req->op == SIGNET_MGMT_OP_IMPORT_CREDENTIAL;
  if (create_or_import) {
    SignetSecretType parsed_type;
    if (!out_req->secret_type ||
        !signet_secret_type_parse(out_req->secret_type, &parsed_type) ||
        (parsed_type != SIGNET_SECRET_API_TOKEN &&
         parsed_type != SIGNET_SECRET_CREDENTIAL) ||
        !out_req->label || !out_req->payload_b64) {
      if (out_error)
        *out_error = g_strdup("credential create/import requires agent_id, "
                              "api_token|credential type, label, and payload");
      signet_mgmt_request_clear(out_req);
      return -1;
    }
  }

  bool needs_credential_id =
      out_req->op == SIGNET_MGMT_OP_INSPECT_CREDENTIAL ||
      out_req->op == SIGNET_MGMT_OP_DELIVER_CREDENTIAL ||
      out_req->op == SIGNET_MGMT_OP_ROTATE_CREDENTIAL ||
      out_req->op == SIGNET_MGMT_OP_REVOKE_CREDENTIAL ||
      out_req->op == SIGNET_MGMT_OP_DELETE_CREDENTIAL;
  if (needs_credential_id &&
      (!out_req->credential_id || !out_req->credential_id[0])) {
    if (out_error) *out_error = g_strdup("credential_id is required");
    signet_mgmt_request_clear(out_req);
    return -1;
  }
  if (out_req->op == SIGNET_MGMT_OP_ROTATE_CREDENTIAL &&
      (!out_req->payload_b64 || !out_req->payload_b64[0])) {
    if (out_error) *out_error = g_strdup("credential rotate requires payload");
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

/* Defaults for signet_mgmt_handler_set_requester_rate_limit(). */
#define SIGNET_MGMT_REQUESTER_BURST 60
#define SIGNET_MGMT_REQUESTER_PER_MINUTE 60
#define SIGNET_MGMT_REQUESTER_MAX_BUCKETS 4096

typedef struct {
  double tokens;
  int64_t updated_at;
} SignetMgmtBucket;

struct SignetMgmtHandler {
  SignetKeyStore *keys;
  SignetRelayPool *relays;
  SignetAuditLogger *audit;
  SignetPolicyStore *policy_store;
  SignetDenyList *deny;   /* shared live deny list (owned by daemon) */
  SignetPolicyRegistry *cap_policy; /* shared live capability registry */
  SignetReplayCache *replay;      /* provisioner replay cache (owned by daemon) */
  SignetReplayCache *replay_self; /* self-service replay cache (owned by daemon) */

  /* Per-sender token buckets for non-provisioner senders (see
   * signet_mgmt_requester_admit). */
  GMutex requester_mu;
  GHashTable *requester_buckets; /* sender hex -> SignetMgmtBucket* */
  double requester_burst;
  double requester_per_second;

  SignetStore *provisioner_store; /* borrowed through key store */

  char *bunker_sk_hex;
  char *bunker_pk_hex;

  char **relay_urls;
  size_t n_relay_urls;

  /* Config file to mirror provisioner authorization into (may be NULL).
   * Decision D2 makes config the desired state, so a runtime grant/revoke
   * that is not written back here would be undone — or worse, reversed — by
   * the next SIGHUP reload. */
  char *config_path;

#ifdef SIGNET_ENABLE_TEST_HOOKS
  SignetMgmtReplyHook reply_hook;
  void *reply_hook_data;
#endif
};

#ifdef SIGNET_ENABLE_TEST_HOOKS
void signet_mgmt_handler_set_reply_hook(SignetMgmtHandler *h,
                                        SignetMgmtReplyHook hook, void *data) {
  if (!h) return;
  h->reply_hook = hook;
  h->reply_hook_data = data;
}
#endif

/* Mirror the persisted provisioner set into the config file so config and
 * SQLCipher state cannot disagree. Returns 0 on success, -1 on failure (which
 * is logged but never fails the mutation: the persisted revocation is already
 * in force, and its tombstone independently blocks resurrection on reload). */
static int signet_mgmt_writeback_provisioners(SignetMgmtHandler *h) {
  if (!h || !h->config_path || !h->config_path[0]) return 0;

  char **pubkeys = NULL;
  size_t count = 0;
  if (signet_store_list_provisioners(h->provisioner_store, &pubkeys, &count) != 0) {
    g_warning("[signetd] provisioner config write-back skipped: cannot read "
              "persisted provisioner set");
    return -1;
  }

  char err[256];
  int rc = signet_config_write_provisioners(
      h->config_path, (const char *const *)pubkeys, count, err, sizeof(err));
  signet_store_free_provisioner_list(pubkeys, count);

  if (rc != 0) {
    g_critical("[signetd] provisioner config write-back to '%s' FAILED: %s. "
               "The persisted revocation tombstone still prevents a reload "
               "from restoring revoked authority, but config and store now "
               "disagree — fix the config file permissions and re-issue.",
               h->config_path, err);
    return -1;
  }

  g_message("[signetd] provisioner set written back to %s (%zu authorized)",
            h->config_path, count);
  return 0;
}

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
  g_mutex_init(&h->requester_mu);
  h->requester_buckets =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  h->requester_burst = SIGNET_MGMT_REQUESTER_BURST;
  h->requester_per_second = SIGNET_MGMT_REQUESTER_PER_MINUTE / 60.0;

  h->provisioner_store = keys ? signet_key_store_get_store(keys) : NULL;
  if (h->provisioner_store && cfg->provisioner_pubkeys &&
      cfg->n_provisioner_pubkeys > 0 &&
      signet_store_seed_provisioners(
          h->provisioner_store, cfg->provisioner_pubkeys,
          cfg->n_provisioner_pubkeys, (int64_t)time(NULL)) < 0) {
    signet_mgmt_handler_free(h);
    return NULL;
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

  if (h->bunker_sk_hex) {
    sodium_memzero(h->bunker_sk_hex, strlen(h->bunker_sk_hex));
    sodium_free(h->bunker_sk_hex);
  }
  g_free(h->bunker_pk_hex);
  for (size_t i = 0; i < h->n_relay_urls; i++) g_free(h->relay_urls[i]);
  g_free(h->relay_urls);
  g_free(h->config_path);
  g_clear_pointer(&h->requester_buckets, g_hash_table_destroy);
  g_mutex_clear(&h->requester_mu);
  g_free(h);
}

void signet_mgmt_handler_set_requester_rate_limit(SignetMgmtHandler *h,
                                                  unsigned burst,
                                                  unsigned per_minute) {
  if (!h) return;
  g_mutex_lock(&h->requester_mu);
  h->requester_burst = burst;
  h->requester_per_second = per_minute / 60.0;
  g_hash_table_remove_all(h->requester_buckets);
  g_mutex_unlock(&h->requester_mu);
}

/* Take one token from a non-provisioner sender's bucket. Returns false when
 * the sender is over its rate. The table is bounded: past
 * SIGNET_MGMT_REQUESTER_MAX_BUCKETS senders, fully refilled buckets are
 * dropped, then (if still full) all of them — a sender that rotates keys
 * gets a fresh bucket per key anyway, so the bound protects memory, not the
 * rate. */
static bool signet_mgmt_requester_admit(SignetMgmtHandler *h,
                                        const char *sender, int64_t now) {
  if (!sender || !sender[0]) return false;
  g_mutex_lock(&h->requester_mu);
  SignetMgmtBucket *b = g_hash_table_lookup(h->requester_buckets, sender);
  if (!b) {
    if (g_hash_table_size(h->requester_buckets) >=
        SIGNET_MGMT_REQUESTER_MAX_BUCKETS) {
      GHashTableIter it;
      gpointer v;
      g_hash_table_iter_init(&it, h->requester_buckets);
      while (g_hash_table_iter_next(&it, NULL, &v)) {
        SignetMgmtBucket *o = v;
        double refilled = o->tokens +
            (double)(now - o->updated_at) * h->requester_per_second;
        if (refilled >= h->requester_burst) g_hash_table_iter_remove(&it);
      }
      if (g_hash_table_size(h->requester_buckets) >=
          SIGNET_MGMT_REQUESTER_MAX_BUCKETS)
        g_hash_table_remove_all(h->requester_buckets);
    }
    b = g_new0(SignetMgmtBucket, 1);
    b->tokens = h->requester_burst;
    b->updated_at = now;
    g_hash_table_insert(h->requester_buckets, g_strdup(sender), b);
  }
  if (now > b->updated_at) {
    b->tokens += (double)(now - b->updated_at) * h->requester_per_second;
    if (b->tokens > h->requester_burst) b->tokens = h->requester_burst;
    b->updated_at = now;
  }
  bool ok = b->tokens >= 1.0;
  if (ok) b->tokens -= 1.0;
  g_mutex_unlock(&h->requester_mu);
  return ok;
}

void signet_mgmt_handler_set_config_path(SignetMgmtHandler *h,
                                         const char *config_path) {
  if (!h) return;
  g_free(h->config_path);
  h->config_path = (config_path && config_path[0]) ? g_strdup(config_path) : NULL;
}

int signet_mgmt_handler_set_relay_urls(SignetMgmtHandler *h,
                                       const char *const *relay_urls,
                                       size_t n_relay_urls) {
  if (!h) return -1;

  char **fresh = NULL;
  if (relay_urls && n_relay_urls > 0) {
    fresh = (char **)g_new0(char *, n_relay_urls);
    for (size_t i = 0; i < n_relay_urls; i++)
      fresh[i] = g_strdup(relay_urls[i] ? relay_urls[i] : "");
  }

  for (size_t i = 0; i < h->n_relay_urls; i++) g_free(h->relay_urls[i]);
  g_free(h->relay_urls);
  h->relay_urls = fresh;
  h->n_relay_urls = fresh ? n_relay_urls : 0;
  return 0;
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

void signet_mgmt_handler_set_policy_registry(
    SignetMgmtHandler *h, SignetPolicyRegistry *policy) {
  if (!h) return;
  h->cap_policy = policy;
}

/* Convert an internal ack to a JSON-RPC response. rpc_id, when given, is the
 * request's own JSON-RPC id and is echoed verbatim (string or integer);
 * otherwise the ack's string request_id is used. */
static char *signet_mgmt_ack_to_jsonrpc(const char *ack_content,
                                        const JsonNode *rpc_id) {
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
  if (rpc_id) json_builder_add_value(b, json_node_copy((JsonNode *)rpc_id));
  else if (rid) json_builder_add_string_value(b, rid);
  else json_builder_add_null_value(b);
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
                                                const char *ack_content,
                                                const JsonNode *rpc_id) {
  if (!h || !h->relays || !h->bunker_sk_hex || !recipient_pubkey_hex || !ack_content) return;
  char *jsonrpc = signet_mgmt_ack_to_jsonrpc(ack_content, rpc_id);
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

typedef enum {
  SIGNET_MGMT_REPLY_LEGACY_ACK,
  SIGNET_MGMT_REPLY_CONTEXTVM,
} SignetMgmtReplyTransport;

/* Publish either a legacy encrypted kind-28090 ack or a canonical NIP-59
 * gift-wrapped ContextVM result, as selected by the current invocation. */
static void signet_mgmt_publish_ack(SignetMgmtHandler *h,
                                    const char *recipient_pubkey_hex,
                                    const char *ack_content,
                                    const char *ref_event_id_hex,
                                    int64_t now,
                                    SignetMgmtReplyTransport reply_transport,
                                    const JsonNode *rpc_id) {
#ifdef SIGNET_ENABLE_TEST_HOOKS
  if (h && h->reply_hook && ack_content) {
    char *observed = reply_transport == SIGNET_MGMT_REPLY_CONTEXTVM
        ? signet_mgmt_ack_to_jsonrpc(ack_content, rpc_id)
        : g_strdup(ack_content);
    if (observed) {
      h->reply_hook(h->reply_hook_data, recipient_pubkey_hex, observed);
      secure_wipe(observed, strlen(observed));
      g_free(observed);
    }
  }
#endif
  if (!h || !h->relays || !h->bunker_sk_hex || !ack_content) return;

  if (reply_transport == SIGNET_MGMT_REPLY_CONTEXTVM) {
    signet_mgmt_publish_contextvm_reply(h, recipient_pubkey_hex, ack_content,
                                        rpc_id);
    return;
  }

  NostrEvent *evt = nostr_event_new();
  if (!evt) return;

  nostr_event_set_kind(evt, 28090);
  nostr_event_set_created_at(evt, now);

  char *encrypted_content = NULL;
  if (recipient_pubkey_hex && recipient_pubkey_hex[0]) {
    uint8_t sk[32] = {0};
    uint8_t pk[32] = {0};
    if (signet_mgmt_hex_to_bytes32(h->bunker_sk_hex, sk) &&
        signet_mgmt_hex_to_bytes32(recipient_pubkey_hex, pk)) {
      if (nostr_nip44_encrypt_v2(sk, pk,
                                 (const uint8_t *)ack_content,
                                 strlen(ack_content),
                                 &encrypted_content) != 0) {
        encrypted_content = NULL;
      }
    }
    signet_mgmt_memzero(sk, sizeof(sk));
  }

  /* Fail closed rather than exposing management results in plaintext. */
  if (!encrypted_content) {
    g_warning("[signetd] mgmt ack encryption failed; refusing to publish plaintext ack");
    nostr_event_free(evt);
    return;
  }
  nostr_event_set_content(evt, encrypted_content);

  NostrTags *tags = nostr_tags_new(0);
  if (tags) {
    if (recipient_pubkey_hex) {
      NostrTag *p_tag = nostr_tag_new("p", recipient_pubkey_hex, NULL);
      if (p_tag) nostr_tags_append(tags, p_tag);
    }
    if (ref_event_id_hex) {
      NostrTag *e_tag = nostr_tag_new("e", ref_event_id_hex, NULL);
      if (e_tag) nostr_tags_append(tags, e_tag);
    }
    nostr_event_set_tags(evt, tags);
  }

  if (nostr_event_sign(evt, h->bunker_sk_hex) == 0) {
    char *json = nostr_event_serialize_compact(evt);
    if (json) {
      (void)signet_relay_pool_publish_event_json(h->relays, json);
      free(json);
    }
  }

  secure_wipe(encrypted_content, strlen(encrypted_content));
  free(encrypted_content);
  nostr_event_free(evt);
}

static int signet_mgmt_handler_handle_request_ex(
    SignetMgmtHandler *h,
    const char *event_pubkey_hex,
    const char *content_json,
    SignetMgmtOp op,
    const char *event_id_hex,
    int64_t now,
    SignetMgmtReplyTransport reply_transport,
    const JsonNode *rpc_id,
    int64_t event_created_at);

/* Credential methods a non-provisioner may call when the credential's owner
 * grants it exactly credential.deliver (see signet_mgmt_delivery_grant_refusal). */
static bool signet_mgmt_op_delivery_grant_eligible(SignetMgmtOp op) {
  return op == SIGNET_MGMT_OP_INSPECT_CREDENTIAL ||
         op == SIGNET_MGMT_OP_LIST_CREDENTIALS ||
         op == SIGNET_MGMT_OP_DELIVER_CREDENTIAL;
}

static SignetMgmtOp signet_mgmt_op_from_contextvm_method(const char *method) {
  if (!method) return SIGNET_MGMT_OP_UNKNOWN;
  if (strcmp(method, "agent/provision") == 0) return SIGNET_MGMT_OP_PROVISION_AGENT;
  if (strcmp(method, "agent/revoke") == 0) return SIGNET_MGMT_OP_REVOKE_AGENT;
  if (strcmp(method, "agent/set-policy") == 0) return SIGNET_MGMT_OP_SET_POLICY;
  if (strcmp(method, "agent/get-status") == 0) return SIGNET_MGMT_OP_GET_STATUS;
  if (strcmp(method, "agent/list") == 0) return SIGNET_MGMT_OP_LIST_AGENTS;
  if (strcmp(method, "agent/rotate-key") == 0) return SIGNET_MGMT_OP_ROTATE_KEY;
  if (strcmp(method, "agent/adopt-existing") == 0) return SIGNET_MGMT_OP_ADOPT_EXISTING;
  if (strcmp(method, "agent/restore-existing") == 0) return SIGNET_MGMT_OP_RESTORE_EXISTING;
  if (strcmp(method, "agent/reissue-connect") == 0) return SIGNET_MGMT_OP_REISSUE_CONNECT;
  if (strcmp(method, "agent/list-clients") == 0) return SIGNET_MGMT_OP_LIST_CLIENTS;
  if (strcmp(method, "agent/revoke-client") == 0) return SIGNET_MGMT_OP_REVOKE_CLIENT;
  if (strcmp(method, "credential/create") == 0) return SIGNET_MGMT_OP_CREATE_CREDENTIAL;
  if (strcmp(method, "credential/import") == 0) return SIGNET_MGMT_OP_IMPORT_CREDENTIAL;
  if (strcmp(method, "credential/list") == 0) return SIGNET_MGMT_OP_LIST_CREDENTIALS;
  if (strcmp(method, "credential/inspect") == 0) return SIGNET_MGMT_OP_INSPECT_CREDENTIAL;
  if (strcmp(method, "credential/deliver") == 0) return SIGNET_MGMT_OP_DELIVER_CREDENTIAL;
  if (strcmp(method, "credential/rotate") == 0) return SIGNET_MGMT_OP_ROTATE_CREDENTIAL;
  if (strcmp(method, "credential/revoke") == 0) return SIGNET_MGMT_OP_REVOKE_CREDENTIAL;
  if (strcmp(method, "credential/delete") == 0) return SIGNET_MGMT_OP_DELETE_CREDENTIAL;
  if (strcmp(method, "config/grant-provisioner") == 0) return SIGNET_MGMT_OP_GRANT_PROVISIONER;
  if (strcmp(method, "config/revoke-provisioner") == 0) return SIGNET_MGMT_OP_REVOKE_PROVISIONER;
  if (strcmp(method, "agent/writer-acquire") == 0) return SIGNET_MGMT_OP_WRITER_ACQUIRE;
  if (strcmp(method, "agent/writer-revoke") == 0) return SIGNET_MGMT_OP_WRITER_REVOKE;
  return SIGNET_MGMT_OP_UNKNOWN;
}

/* Longest string JSON-RPC id echoed back; a longer one is answered with id
 * null rather than reflected. */
#define SIGNET_MGMT_MAX_RPC_ID_LEN 256

/* Normalize a ContextVM JSON-RPC request into the params object that
 * signet_mgmt_request_parse() reads. *out_rpc_id receives a copy of the
 * request's id when it is a string (at most SIGNET_MGMT_MAX_RPC_ID_LEN bytes)
 * or an integer — also for an unknown method, so its refusal can echo it —
 * and NULL otherwise; free it with json_node_unref(). */
static char *signet_mgmt_contextvm_params_json(const char *content_json,
                                               SignetMgmtOp *out_op,
                                               JsonNode **out_rpc_id) {
  if (out_op) *out_op = SIGNET_MGMT_OP_UNKNOWN;
  if (out_rpc_id) *out_rpc_id = NULL;
  if (!content_json || !content_json[0]) return NULL;

  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, content_json, -1, NULL)) return NULL;
  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;
  JsonObject *o = json_node_get_object(root);
  JsonNode *idn = json_object_get_member(o, "id");
  if (out_rpc_id && idn && JSON_NODE_HOLDS_VALUE(idn)) {
    GType t = json_node_get_value_type(idn);
    const char *ids = t == G_TYPE_STRING ? json_node_get_string(idn) : NULL;
    if (t == G_TYPE_INT64 ||
        (ids && strlen(ids) <= SIGNET_MGMT_MAX_RPC_ID_LEN))
      *out_rpc_id = json_node_copy(idn);
  }
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
  if (po && json_object_has_member(po, "provisioner_pubkey")) {
    json_builder_set_member_name(b, "provisioner_pubkey");
    json_builder_add_string_value(b, json_object_get_string_member(po, "provisioner_pubkey"));
  }
  if (po && json_object_has_member(po, "writer_pubkey")) {
    json_builder_set_member_name(b, "writer_pubkey");
    json_builder_add_string_value(b, json_object_get_string_member(po, "writer_pubkey"));
  }
  if (po && json_object_has_member(po, "credential_id")) {
    json_builder_set_member_name(b, "credential_id");
    json_builder_add_string_value(b, json_object_get_string_member(po, "credential_id"));
  }
  if (po && json_object_has_member(po, "secret_type")) {
    json_builder_set_member_name(b, "secret_type");
    json_builder_add_string_value(b, json_object_get_string_member(po, "secret_type"));
  }
  if (po && json_object_has_member(po, "label")) {
    json_builder_set_member_name(b, "label");
    json_builder_add_string_value(b, json_object_get_string_member(po, "label"));
  }
  if (po && json_object_has_member(po, "payload_b64")) {
    json_builder_set_member_name(b, "payload_b64");
    json_builder_add_string_value(b, json_object_get_string_member(po, "payload_b64"));
  }
  if (po && json_object_has_member(po, "policy_id")) {
    json_builder_set_member_name(b, "policy_id");
    json_builder_add_string_value(b, json_object_get_string_member(po, "policy_id"));
  }
  if (po && json_object_has_member(po, "expires_at")) {
    json_builder_set_member_name(b, "expires_at");
    json_builder_add_int_value(b, json_object_get_int_member(po, "expires_at"));
  }
  if (po && json_object_has_member(po, "expected_version")) {
    /* Verbatim: signet_mgmt_request_parse() rejects a non-integer. */
    json_builder_set_member_name(b, "expected_version");
    json_builder_add_value(
        b, json_node_copy(json_object_get_member(po, "expected_version")));
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

/* A JSON-RPC error response to a ContextVM request that never reached the
 * request path. rpc_id is the sender's own request id (NULL -> null); data is
 * a stable reason code. */
static char *signet_mgmt_contextvm_error_json(int code, const char *message,
                                              const char *data,
                                              const JsonNode *rpc_id) {
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc");
  json_builder_add_string_value(b, "2.0");
  json_builder_set_member_name(b, "error");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "code");
  json_builder_add_int_value(b, code);
  json_builder_set_member_name(b, "message");
  json_builder_add_string_value(b, message);
  json_builder_set_member_name(b, "data");
  json_builder_add_string_value(b, data);
  json_builder_end_object(b);
  json_builder_set_member_name(b, "id");
  if (rpc_id) json_builder_add_value(b, json_node_copy((JsonNode *)rpc_id));
  else json_builder_add_null_value(b);
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
                                      int64_t event_created_at,
                                      int64_t now) {
  if (!h) return -1;

  SignetMgmtOp op = SIGNET_MGMT_OP_UNKNOWN;
  JsonNode *rpc_id = NULL;
  char *params_json =
      signet_mgmt_contextvm_params_json(content_json, &op, &rpc_id);
  int rc = -1;

  /* Two kinds of non-provisioner request are authorized on the parsed
   * request instead of here: agent/reissue-connect by the target agent
   * itself, and credential/inspect|list|deliver by the credential owner's
   * exact credential.deliver grant. Every other method (and every unknown
   * method) remains provisioner-only and is refused here. A refusal echoes
   * the sender's own JSON-RPC id so the client can match it to its request;
   * it says nothing beyond the refusal itself. */
  bool defer_auth = op == SIGNET_MGMT_OP_REISSUE_CONNECT ||
                    signet_mgmt_op_delivery_grant_eligible(op);
  if (!defer_auth &&
      !signet_store_is_provisioner(h->provisioner_store, event_pubkey_hex)) {
    signet_mgmt_chain_audit(h, "mgmt_unauthorized", event_pubkey_hex, NULL,
                            "deny", "not_provisioner", now);
    char *err = signet_mgmt_contextvm_error_json(
        -32002, "sender is not an authorized Signet provisioner",
        "not_provisioner", rpc_id);
    signet_mgmt_publish_ack(h, event_pubkey_hex, err, event_id_hex, now,
                            SIGNET_MGMT_REPLY_CONTEXTVM, rpc_id);
    g_free(err);
    goto out;
  }

  if (!params_json || op == SIGNET_MGMT_OP_UNKNOWN) {
    char *err = signet_mgmt_contextvm_error_json(
        -32601, "unknown Signet ContextVM method", "unknown_method", rpc_id);
    signet_mgmt_publish_ack(h, event_pubkey_hex, err, event_id_hex, now,
                            SIGNET_MGMT_REPLY_CONTEXTVM, rpc_id);
    g_free(err);
    goto out;
  }

  {
    char *encrypted = NULL;
    uint8_t sk[32], pk[32];
    if (signet_mgmt_hex_to_bytes32(h->bunker_sk_hex, sk) &&
        signet_mgmt_hex_to_bytes32(event_pubkey_hex, pk)) {
      (void)nostr_nip44_encrypt_v2(sk, pk, (const uint8_t *)params_json,
                                   strlen(params_json), &encrypted);
    }
    signet_mgmt_memzero(sk, sizeof(sk));
    if (encrypted) {
      rc = signet_mgmt_handler_handle_request_ex(
          h, event_pubkey_hex, encrypted, op, event_id_hex, now,
          SIGNET_MGMT_REPLY_CONTEXTVM, rpc_id, event_created_at);
      free(encrypted);
    }
  }

out:
  /* params_json may embed agent_nsec/connect_secret (adopt-existing). */
  if (params_json) {
    secure_wipe(params_json, strlen(params_json));
    g_free(params_json);
  }
  if (rpc_id) json_node_unref(rpc_id);
  return rc;
}

/* Append a redacted entry to the store's hash-chained audit log for a
 * management outcome. identity is the acting/subject identity (sender pubkey
 * for unauthorized events, agent_id for executed mutations); secret_id is the
 * affected credential id when applicable. detail carries only enumerated,
 * non-secret fields (decision + stable reason code) — never request payloads.
 * Silently a no-op in cache-only mode (no persistent store). */
static void signet_mgmt_chain_audit(SignetMgmtHandler *h,
                                    const char *operation,
                                    const char *identity,
                                    const char *secret_id,
                                    const char *decision,
                                    const char *reason,
                                    int64_t now) {
  signet_mgmt_chain_audit_ex(h, operation, identity, secret_id, decision,
                             reason, NULL, NULL, now);
}

/* As signet_mgmt_chain_audit, plus auth: the authorization path of a
 * credential method ("provisioner" or "delivery_grant"), and owner: the
 * credential owner a request targeted (an agent id), each or NULL. */
static void signet_mgmt_chain_audit_ex(SignetMgmtHandler *h,
                                       const char *operation,
                                       const char *identity,
                                       const char *secret_id,
                                       const char *decision,
                                       const char *reason,
                                       const char *auth,
                                       const char *owner,
                                       int64_t now) {
  if (!h || !h->keys || !operation) return;
  SignetStore *base_store = signet_key_store_get_store(h->keys);
  if (!base_store) return;
  char *detail = NULL;
  {
    JsonBuilder *b = json_builder_new();
    if (b) {
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "decision");
      json_builder_add_string_value(b, decision ? decision : "deny");
      json_builder_set_member_name(b, "reason");
      json_builder_add_string_value(b, reason ? reason : "unknown");
      if (auth) {
        json_builder_set_member_name(b, "auth");
        json_builder_add_string_value(b, auth);
      }
      if (owner) {
        json_builder_set_member_name(b, "owner");
        json_builder_add_string_value(b, owner);
      }
      json_builder_end_object(b);
      JsonNode *root = json_builder_get_root(b);
      JsonGenerator *gen = json_generator_new();
      if (root && gen) {
        json_generator_set_root(gen, root);
        json_generator_set_pretty(gen, FALSE);
        detail = json_generator_to_data(gen, NULL);
      }
      if (gen) g_object_unref(gen);
      if (root) json_node_free(root);
      g_object_unref(b);
    }
  }
  (void)signet_audit_log_append(base_store, now,
                                (identity && identity[0]) ? identity : "unknown",
                                operation, secret_id, "contextvm", detail);
  g_free(detail);
}

/* Map a management op to its chain-audit operation name, or NULL for
 * read-only operations that do not need a mutation audit. */
static const char *signet_mgmt_op_audit_name(SignetMgmtOp op) {
  switch (op) {
    case SIGNET_MGMT_OP_PROVISION_AGENT:    return "mgmt_provision_agent";
    case SIGNET_MGMT_OP_ADOPT_EXISTING:     return "mgmt_adopt_existing";
    case SIGNET_MGMT_OP_RESTORE_EXISTING:   return "mgmt_restore_existing";
    case SIGNET_MGMT_OP_REVOKE_AGENT:       return "mgmt_revoke_agent";
    case SIGNET_MGMT_OP_ROTATE_KEY:         return "mgmt_rotate_key";
    case SIGNET_MGMT_OP_REISSUE_CONNECT:    return "mgmt_reissue_connect";
    case SIGNET_MGMT_OP_SET_POLICY:         return "mgmt_set_policy";
    case SIGNET_MGMT_OP_REVOKE_CLIENT:      return "mgmt_revoke_client";
    case SIGNET_MGMT_OP_CREATE_CREDENTIAL:  return "credential_create";
    case SIGNET_MGMT_OP_IMPORT_CREDENTIAL:  return "credential_import";
    case SIGNET_MGMT_OP_DELIVER_CREDENTIAL: return "credential_deliver";
    case SIGNET_MGMT_OP_ROTATE_CREDENTIAL:  return "credential_rotate";
    case SIGNET_MGMT_OP_REVOKE_CREDENTIAL:  return "credential_revoke";
    case SIGNET_MGMT_OP_DELETE_CREDENTIAL:  return "credential_delete";
    case SIGNET_MGMT_OP_GRANT_PROVISIONER:  return "mgmt_grant_provisioner";
    case SIGNET_MGMT_OP_REVOKE_PROVISIONER: return "mgmt_revoke_provisioner";
    case SIGNET_MGMT_OP_WRITER_ACQUIRE: return "writer_acquire";
    case SIGNET_MGMT_OP_WRITER_REVOKE: return "writer_revoke";
    default:                                return NULL;
  }
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

static void signet_mgmt_add_secret_metadata(JsonBuilder *b,
                                              const SignetSecretMetadata *m) {
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "credential_id");
  json_builder_add_string_value(b, m->id ? m->id : "");
  json_builder_set_member_name(b, "agent_id");
  json_builder_add_string_value(b, m->agent_id ? m->agent_id : "");
  json_builder_set_member_name(b, "secret_type");
  json_builder_add_string_value(b, signet_secret_type_to_string(m->secret_type));
  json_builder_set_member_name(b, "label");
  json_builder_add_string_value(b, m->label ? m->label : "");
  json_builder_set_member_name(b, "policy_id");
  if (m->policy_id) json_builder_add_string_value(b, m->policy_id);
  else json_builder_add_null_value(b);
  json_builder_set_member_name(b, "provenance");
  json_builder_add_string_value(b, m->provenance ? m->provenance : "legacy");
  json_builder_set_member_name(b, "created_by");
  if (m->created_by) json_builder_add_string_value(b, m->created_by);
  else json_builder_add_null_value(b);
  json_builder_set_member_name(b, "created_at");
  json_builder_add_int_value(b, m->created_at);
  json_builder_set_member_name(b, "rotated_at");
  if (m->rotated_at) json_builder_add_int_value(b, m->rotated_at);
  else json_builder_add_null_value(b);
  json_builder_set_member_name(b, "expires_at");
  if (m->expires_at) json_builder_add_int_value(b, m->expires_at);
  else json_builder_add_null_value(b);
  json_builder_set_member_name(b, "revoked_at");
  if (m->revoked_at) json_builder_add_int_value(b, m->revoked_at);
  else json_builder_add_null_value(b);
  json_builder_set_member_name(b, "version");
  json_builder_add_int_value(b, m->version);
  json_builder_set_member_name(b, "active_version");
  json_builder_add_int_value(b, m->active_version);
  json_builder_set_member_name(b, "status");
  json_builder_add_string_value(b, signet_secret_status_to_string(m->status));
  json_builder_end_object(b);
}

static char *signet_mgmt_secret_metadata_json(
    const SignetSecretMetadata *metadata) {
  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;
  signet_mgmt_add_secret_metadata(b, metadata);
  JsonNode *root = json_builder_get_root(b);
  JsonGenerator *gen = json_generator_new();
  char *json = NULL;
  if (root && gen) {
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, FALSE);
    json = json_generator_to_data(gen, NULL);
  }
  if (gen) g_object_unref(gen);
  if (root) json_node_free(root);
  g_object_unref(b);
  return json;
}

static char *signet_mgmt_secret_metadata_list_json(
    const SignetSecretMetadata *metadata, size_t count) {
  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "count");
  json_builder_add_int_value(b, (gint64)count);
  json_builder_set_member_name(b, "credentials");
  json_builder_begin_array(b);
  for (size_t i = 0; i < count; i++)
    signet_mgmt_add_secret_metadata(b, &metadata[i]);
  json_builder_end_array(b);
  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  JsonGenerator *gen = json_generator_new();
  char *json = NULL;
  if (root && gen) {
    json_generator_set_root(gen, root);
    json_generator_set_pretty(gen, FALSE);
    json = json_generator_to_data(gen, NULL);
  }
  if (gen) g_object_unref(gen);
  if (root) json_node_free(root);
  g_object_unref(b);
  return json;
}

static int signet_mgmt_decode_secret_payload(const char *payload_b64,
                                              uint8_t **out_payload,
                                              size_t *out_len) {
  if (!payload_b64 || !out_payload || !out_len) return -1;
  *out_payload = NULL;
  *out_len = 0;
  size_t encoded_len = strlen(payload_b64);
  if (encoded_len == 0 || encoded_len > 1398108u) return -1;
  size_t capacity = (encoded_len / 4u) * 3u + 3u;
  if (capacity > 1024u * 1024u + 2u) return -1;
  uint8_t *payload = sodium_malloc(capacity);
  if (!payload) return -1;
  size_t decoded_len = 0;
  if (sodium_base642bin(payload, capacity, payload_b64, encoded_len,
                        NULL, &decoded_len, NULL,
                        sodium_base64_VARIANT_ORIGINAL) != 0 ||
      decoded_len == 0 || decoded_len > 1024u * 1024u) {
    sodium_free(payload);
    return -1;
  }
  *out_payload = payload;
  *out_len = decoded_len;
  return 0;
}

/* Authorize a non-provisioner's credential/inspect|list|deliver by the exact
 * credential.deliver grant (signet_policy_store_has_exact_delivery_grant) of
 * the credential owner: the request's agent_id for list and deliver, the
 * stored owner of credential_id for inspect. Returns NULL when authorized,
 * else the refusal code. A sender without a grant gets "no_capability"
 * whether or not the credential exists, so the refusal is no existence
 * oracle. Deny > grant: a deny-listed sender or owner is refused.
 * credential/deliver is then authorized again, with ownership, lease and
 * audit, by signet_credential_access_acquire(). */
static const char *signet_mgmt_delivery_grant_refusal(
    SignetMgmtHandler *h, const SignetMgmtRequest *req, const char *sender,
    int64_t now, char **out_owner) {
  *out_owner = NULL;
  if (!sender || !sender[0]) return "no_capability";
  if (h->deny && signet_deny_list_contains(h->deny, sender))
    return "deny_listed";
  char *owner = NULL;
  if (req->op == SIGNET_MGMT_OP_INSPECT_CREDENTIAL) {
    SignetStore *store = signet_key_store_get_store(h->keys);
    if (!store) return "no_store";
    SignetSecretMetadata m;
    memset(&m, 0, sizeof(m));
    SignetSecretResult src =
        signet_store_get_secret_metadata(store, req->credential_id, now, &m);
    if (src == SIGNET_SECRET_OK) owner = g_strdup(m.agent_id);
    signet_secret_metadata_clear(&m);
    if (src != SIGNET_SECRET_OK && src != SIGNET_SECRET_NOT_FOUND)
      return "credential_store_failed";
  } else if (req->agent_id) {
    owner = g_strdup(req->agent_id);
  }
  *out_owner = owner;
  /* A missing credential still costs the policy lookup an existing one
   * does, so the two refusals take comparable work. */
  bool granted = h->policy_store &&
      signet_policy_store_has_exact_delivery_grant(
          h->policy_store, (owner && owner[0]) ? owner : "\x01no-owner",
          sender, now);
  if (!owner || !owner[0] || !granted) return "no_capability";
  char owner_pk[65] = {0};
  if (h->deny &&
      signet_key_store_get_agent_pubkey(h->keys, owner, owner_pk,
                                        sizeof(owner_pk)) &&
      signet_deny_list_contains(h->deny, owner_pk))
    return "deny_listed";
  return NULL;
}

/* A request-supplied id recorded in the audit chain: bounded, never echoed
 * at arbitrary length. */
static const char *signet_mgmt_audit_target(const char *id) {
  if (!id || !id[0]) return NULL;
  return strlen(id) <= 128 ? id : "overlong";
}

static int signet_mgmt_handler_handle_request_ex(
    SignetMgmtHandler *h,
    const char *event_pubkey_hex,
    const char *content_json,
    SignetMgmtOp op,
    const char *event_id_hex,
    int64_t now,
    SignetMgmtReplyTransport reply_transport,
    const JsonNode *rpc_id,
    int64_t event_created_at) {
  if (!h) return -1;

  /* 1) Authorization check. agent/reissue-connect also admits the target
   * agent itself, and credential/inspect|list|deliver a sender holding the
   * owner's exact delivery grant; both checks need the parsed request and
   * run after parse (2a, 2a-grant), before the replay mark. The sender
   * pubkey is trustworthy on every ingress path: plain events are
   * Schnorr-verified before dispatch (relay_pool NPA-01) and gift-wrapped
   * intents take the authenticated seal author from nostr_nip17_decrypt_dm. */
  bool sender_is_provisioner =
      signet_store_is_provisioner(h->provisioner_store, event_pubkey_hex);
  bool grant_path = !sender_is_provisioner &&
                    signet_mgmt_op_delivery_grant_eligible(op);
  if (!sender_is_provisioner && op != SIGNET_MGMT_OP_REISSUE_CONNECT &&
      !grant_path) {
    /* Silently drop unauthorized events (no ack — do not confirm the bunker
     * exists) but record the attempt in the tamper-evident audit chain. */
    signet_mgmt_chain_audit(h, "mgmt_unauthorized", event_pubkey_hex, NULL,
                            "deny", "not_provisioner", now);
    return -1;
  }

  /* 1a) A non-provisioner sender (delivery-grant requester or self-service
   * agent) is rate-limited before anything costly — decryption, store reads,
   * policy reload, audit. The refusal is not audited, so it stays cheap. */
  if (!sender_is_provisioner &&
      !signet_mgmt_requester_admit(h, event_pubkey_hex, now)) {
    char *rl = signet_mgmt_build_ack(NULL, false, "rate_limited",
                                     "too many requests from this sender",
                                     NULL);
    if (rl) {
      signet_mgmt_publish_ack(h, event_pubkey_hex, rl, event_id_hex, now,
                              reply_transport, rpc_id);
      g_free(rl);
    }
    return -1;
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
      signet_mgmt_publish_ack(h, event_pubkey_hex, ack, event_id_hex, now,
                              reply_transport, rpc_id);
      g_free(ack);
    }
    g_free(parse_err);
    if (decrypted_content) { secure_wipe(decrypted_content, strlen(decrypted_content)); free(decrypted_content); }
    return -1;
  }

  /* 2a-grant) Delivery-grant authorization for a non-provisioner's
   * credential/inspect|list|deliver, BEFORE the replay mark (as in 2a) so
   * senders without a grant cannot churn the replay cache. */
  if (grant_path) {
    g_autofree char *target_owner = NULL;
    const char *gcode = signet_mgmt_delivery_grant_refusal(
        h, &req, event_pubkey_hex, now, &target_owner);
    if (gcode) {
      /* Record what was asked for (ids only, no secret material). */
      signet_mgmt_chain_audit_ex(
          h,
          req.op == SIGNET_MGMT_OP_DELIVER_CREDENTIAL ? "credential_deliver"
                                                      : "mgmt_unauthorized",
          event_pubkey_hex, signet_mgmt_audit_target(req.credential_id),
          "deny", gcode, "delivery_grant",
          signet_mgmt_audit_target(target_owner), now);
      char *gack = signet_mgmt_build_ack(
          req.request_id, false, gcode,
          strcmp(gcode, "no_capability") == 0
              ? "credential is not delivered to this requester"
              : strcmp(gcode, "deny_listed") == 0
                    ? "requester or credential owner is deny-listed"
                    : "credential store unavailable",
          NULL);
      if (gack) {
        signet_mgmt_publish_ack(h, event_pubkey_hex, gack, event_id_hex, now,
                                reply_transport, rpc_id);
        g_free(gack);
      }
      if (decrypted_content) { secure_wipe(decrypted_content, strlen(decrypted_content)); free(decrypted_content); }
      signet_mgmt_request_clear(&req);
      return -1;
    }
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
      signet_mgmt_chain_audit(h, "mgmt_unauthorized", event_pubkey_hex, NULL,
                              "deny", "not_target_agent", now);
      char *uack = signet_mgmt_build_ack(req.request_id, false, "unauthorized",
                                         "sender is neither a provisioner nor the target agent",
                                         NULL);
      if (uack) {
        signet_mgmt_publish_ack(h, event_pubkey_hex, uack, event_id_hex, now,
                                reply_transport, rpc_id);
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
        signet_mgmt_publish_ack(h, event_pubkey_hex, dack, event_id_hex, now,
                                reply_transport, rpc_id);
        g_free(dack);
      }
      signet_mgmt_publish_cas_audit(h, "reissue_connect", req.agent_id,
                                    "deny_listed", now);
      signet_mgmt_chain_audit(h, "mgmt_reissue_connect", event_pubkey_hex, NULL,
                              "deny", "deny_listed", now);
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
  /* Self-service and delivery-grant events are marked in their OWN replay
   * domain, and every id is charged to its authenticated sender's quota, so
   * no sender can evict another's event ids (replay-then-re-execute). With a
   * skew window, the request's own created_at bounds how long it can be
   * replayed at all; a refuse_when_full cache refuses rather than forgets.
   * Falls back to the provisioner cache only if no self cache was wired. */
  SignetReplayCache *replay_domain =
      sender_is_provisioner ? h->replay
                            : (h->replay_self ? h->replay_self : h->replay);
  if (replay_domain) {
    SignetReplayResult rr = signet_replay_check_and_mark_scoped(
        replay_domain, event_pubkey_hex, event_id_hex, event_created_at, now);
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
      /* Malformed (missing event id), outside the clock window, or no replay
       * room left for this sender: fail closed, but tell the sender. */
      const char *rcode = "replay_invalid";
      const char *rmsg = "management event rejected by replay protection";
      if (rr == SIGNET_REPLAY_TOO_OLD || rr == SIGNET_REPLAY_TOO_FAR_IN_FUTURE) {
        rcode = "request_time_skew";
        rmsg = "request created_at is outside Signet's accepted clock window";
      } else if (rr == SIGNET_REPLAY_FULL) {
        rcode = "replay_quota_exceeded";
        rmsg = "too many recent requests from this sender";
      }
      char *rack = signet_mgmt_build_ack(req.request_id, false, rcode, rmsg,
                                         NULL);
      if (rack) {
        signet_mgmt_publish_ack(h, event_pubkey_hex, rack, event_id_hex, now,
                                reply_transport, rpc_id);
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
  char *audit_secret_id = NULL; /* affected credential id for chain audit */
  const char *audit_code = NULL; /* chain-audit reason when the reply's differs */

  switch (req.op) {
    case SIGNET_MGMT_OP_WRITER_ACQUIRE: {
      int64_t epoch = 0;
      if (signet_key_store_writer_acquire(h->keys, req.agent_id,
          req.writer_pubkey, &epoch) == 0) {
        ok = true;
        code = "writer_acquired";
        message = g_strdup("writer lease acquired");
        result = g_strdup_printf("{\"epoch\":%lld}", (long long)epoch);
      } else {
        code = "writer_acquire_failed";
        message = g_strdup("writer lease acquisition failed");
      }
      break;
    }
    case SIGNET_MGMT_OP_WRITER_REVOKE: {
      int64_t epoch = 0;
      if (signet_key_store_writer_revoke(h->keys, req.agent_id,
                                         &epoch) == 0) {
        ok = true;
        code = "writer_revoked";
        message = g_strdup("writer lease revoked");
        result = g_strdup_printf("{\"epoch\":%lld}", (long long)epoch);
      } else {
        code = "writer_revoke_failed";
        message = g_strdup("writer lease revocation failed");
      }
      break;
    }
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

    case SIGNET_MGMT_OP_ADOPT_EXISTING:
    case SIGNET_MGMT_OP_RESTORE_EXISTING: {
      const bool restoring = req.op == SIGNET_MGMT_OP_RESTORE_EXISTING;
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
        signet_mgmt_publish_cas_audit(
            h, restoring ? "restore_existing" : "adopt_existing",
            req.agent_id, "deny_listed", now);
        sodium_memzero(sk_raw, sizeof(sk_raw));
        break;
      }

      char pubkey_hex[65];
      char *bunker_uri = NULL;
      SignetAdoptResult ar;
      if (restoring) {
        ar = signet_key_store_restore_agent(
            h->keys, req.agent_id, sk_raw, req.expected_pubkey, pubkey_hex);
      } else {
        ar = signet_key_store_adopt_agent(
            h->keys, req.agent_id, sk_raw,
            req.expected_pubkey, req.connect_secret,
            h->bunker_pk_hex,
            (const char *const *)h->relay_urls, h->n_relay_urls,
            pubkey_hex, &bunker_uri);
      }
      sodium_memzero(sk_raw, sizeof(sk_raw));

      if (ar == SIGNET_ADOPT_OK) {
        ok = true;
        code = restoring ? "restored" : "adopted";
        message = g_strdup_printf("agent %s %s", req.agent_id,
                                  restoring ? "restored" : "adopted");
        if (bunker_uri) {
          char *escaped_uri = g_strescape(bunker_uri, NULL);
          result = g_strdup_printf(
              "{\"agent_id\":\"%s\",\"pubkey\":\"%s\",\"adopted\":true,\"bunker_uri\":\"%s\"}",
              req.agent_id, pubkey_hex, escaped_uri ? escaped_uri : bunker_uri);
          g_free(escaped_uri);
        } else {
          result = g_strdup_printf(
              restoring
                ? "{\"agent_id\":\"%s\",\"pubkey\":\"%s\",\"restored\":true}"
                : "{\"agent_id\":\"%s\",\"pubkey\":\"%s\",\"adopted\":true}",
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
        signet_mgmt_publish_cas_audit(
            h, restoring ? "restore_existing" : "adopt_existing",
            req.agent_id, "ok", now);
      } else {
        switch (ar) {
          case SIGNET_ADOPT_ERR_INVALID_SECRET:  code = "invalid_secret"; break;
          case SIGNET_ADOPT_ERR_PUBKEY_MISMATCH: code = "pubkey_mismatch"; break;
          case SIGNET_ADOPT_ERR_AGENT_EXISTS:    code = "agent_exists"; break;
          case SIGNET_ADOPT_ERR_AGENT_NOT_FOUND: code = "not_found"; break;
          case SIGNET_ADOPT_ERR_PUBKEY_EXISTS:   code = "pubkey_exists"; break;
          default:                               code = "adopt_failed"; break;
        }
        message = g_strdup_printf("failed to adopt agent (%s)", code);
        signet_mgmt_publish_cas_audit(
            h, restoring ? "restore_existing" : "adopt_existing",
            req.agent_id, code, now);
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

    case SIGNET_MGMT_OP_CREATE_CREDENTIAL:
    case SIGNET_MGMT_OP_IMPORT_CREDENTIAL: {
      SignetStore *base_store = signet_key_store_get_store(h->keys);
      uint8_t *payload = NULL;
      size_t payload_len = 0;
      SignetSecretType secret_type;
      if (!base_store) {
        code = "no_store";
        message = g_strdup("persistent store not available");
        break;
      }
      if (!signet_secret_type_parse(req.secret_type, &secret_type) ||
          signet_mgmt_decode_secret_payload(req.payload_b64,
                                            &payload, &payload_len) != 0) {
        code = "invalid_credential";
        message = g_strdup("invalid credential type or payload");
        break;
      }
      SignetSecretMetadata metadata;
      memset(&metadata, 0, sizeof(metadata));
      const char *provenance =
          req.op == SIGNET_MGMT_OP_IMPORT_CREDENTIAL ? "imported" : "created";
      SignetSecretResult src = signet_store_create_secret(
          base_store, req.agent_id, secret_type, req.label,
          payload, payload_len, req.credential_policy_id,
          req.has_credential_expires_at ? req.credential_expires_at : 0,
          provenance, event_pubkey_hex, now, &metadata);
      sodium_free(payload);
      if (src == SIGNET_SECRET_OK) {
        audit_secret_id = g_strdup(metadata.id);
        result = signet_mgmt_secret_metadata_json(&metadata);
        if (result) {
          ok = true;
          code = req.op == SIGNET_MGMT_OP_IMPORT_CREDENTIAL
                   ? "credential_imported" : "credential_created";
          message = g_strdup(req.op == SIGNET_MGMT_OP_IMPORT_CREDENTIAL
                               ? "credential imported" : "credential created");
          signet_mgmt_publish_cas_audit(
              h, req.op == SIGNET_MGMT_OP_IMPORT_CREDENTIAL
                   ? "credential_import" : "credential_create",
              metadata.agent_id, "ok", now);
        } else {
          code = "internal_error";
          message = g_strdup("failed to build credential metadata");
        }
      } else if (src == SIGNET_SECRET_EXISTS) {
        code = "credential_exists";
        message = g_strdup("credential already exists; overwrite refused");
      } else if (src == SIGNET_SECRET_NOT_FOUND) {
        code = "agent_not_found";
        message = g_strdup("credential owner agent not found");
      } else {
        code = "invalid_credential";
        message = g_strdup("credential creation/import failed");
      }
      signet_secret_metadata_clear(&metadata);
      break;
    }

    case SIGNET_MGMT_OP_LIST_CREDENTIALS: {
      SignetStore *base_store = signet_key_store_get_store(h->keys);
      if (!base_store) {
        code = "no_store";
        message = g_strdup("persistent store not available");
        break;
      }
      SignetSecretMetadata *metadata = NULL;
      size_t count = 0;
      SignetSecretResult src = signet_store_list_secret_metadata(
          base_store, req.agent_id, now, &metadata, &count);
      if (src == SIGNET_SECRET_OK) {
        result = signet_mgmt_secret_metadata_list_json(metadata, count);
        if (result) {
          ok = true;
          code = "ok";
        } else {
          code = "internal_error";
          message = g_strdup("failed to build credential list");
        }
      } else {
        code = "credential_store_failed";
        message = g_strdup("failed to list credentials");
      }
      signet_secret_metadata_list_free(metadata, count);
      break;
    }

    case SIGNET_MGMT_OP_INSPECT_CREDENTIAL: {
      SignetStore *base_store = signet_key_store_get_store(h->keys);
      SignetSecretMetadata metadata;
      memset(&metadata, 0, sizeof(metadata));
      if (!base_store) {
        code = "no_store";
        message = g_strdup("persistent store not available");
        break;
      }
      SignetSecretResult src = signet_store_get_secret_metadata(
          base_store, req.credential_id, now, &metadata);
      if (src == SIGNET_SECRET_OK) {
        result = signet_mgmt_secret_metadata_json(&metadata);
        if (result) {
          ok = true;
          code = "ok";
        } else {
          code = "internal_error";
          message = g_strdup("failed to build credential metadata");
        }
      } else if (src == SIGNET_SECRET_NOT_FOUND) {
        code = "credential_not_found";
        message = g_strdup("credential not found");
      } else {
        code = "credential_store_failed";
        message = g_strdup("failed to inspect credential");
      }
      signet_secret_metadata_clear(&metadata);
      break;
    }

    case SIGNET_MGMT_OP_DELIVER_CREDENTIAL: {
      SignetStore *base_store = signet_key_store_get_store(h->keys);
      if (!base_store || !h->cap_policy || !req.agent_id || !req.agent_id[0]) {
        code = "credential_access_unavailable";
        message = g_strdup("credential delivery policy is unavailable");
        break;
      }

      SignetCredentialAccessContext access_ctx = {
        .store = base_store,
        .policy = h->cap_policy,
        .identity_policy = h->policy_store,
        .deny = h->deny,
        .logger = h->audit,
      };
      SignetCredentialAccessRequest access_req = {
        .agent_id = req.agent_id,
        .credential_id = req.credential_id,
        .capability = SIGNET_CAP_CREDENTIAL_DELIVER,
        .transport = "contextvm",
        .requester_pubkey = event_pubkey_hex,
        .lease_id = NULL,
        .one_use_delivery = true,
        .issue_lease = false,
        .lease_ttl_seconds = 60,
        .expected_version = req.expected_version,
      };
      SignetCredentialAccessGrant grant;
      memset(&grant, 0, sizeof(grant));
      SignetCredAccessStatus arc = signet_credential_access_acquire(
          &access_ctx, &access_req, now, &grant);
      if (arc != SIGNET_CRED_ACCESS_OK) {
        code = signet_cred_access_reason(arc);
        message = g_strdup(arc == SIGNET_CRED_ACCESS_VERSION_MISMATCH
                               ? "active credential version is not expected_version"
                               : "credential delivery denied");
        /* A delivery-grant requester learns nothing about credentials it is
         * not granted: another owner's credential and a missing one get the
         * same refusal. The audit keeps the real reason. */
        if (grant_path && (arc == SIGNET_CRED_ACCESS_NOT_OWNER ||
                           arc == SIGNET_CRED_ACCESS_NOT_FOUND)) {
          audit_code = code;
          code = "no_capability";
          g_free(message);
          message = g_strdup("credential is not delivered to this requester");
        }
        signet_credential_access_grant_clear(&grant);
        break;
      }

      char *payload_b64 = g_base64_encode(grant.record.payload,
                                          grant.record.payload_len);
      JsonBuilder *db = json_builder_new();
      if (payload_b64 && db) {
        json_builder_begin_object(db);
        json_builder_set_member_name(db, "credential_id");
        json_builder_add_string_value(db, req.credential_id);
        json_builder_set_member_name(db, "agent_id");
        json_builder_add_string_value(db, req.agent_id);
        json_builder_set_member_name(db, "version");
        json_builder_add_int_value(db, grant.record.active_version);
        json_builder_set_member_name(db, "payload_b64");
        json_builder_add_string_value(db, payload_b64);
        json_builder_set_member_name(db, "lease_id");
        json_builder_add_string_value(db, grant.lease_id ? grant.lease_id : "");
        json_builder_set_member_name(db, "lease_expires_at");
        json_builder_add_int_value(db, grant.lease_expires_at);
        json_builder_end_object(db);
        JsonNode *root = json_builder_get_root(db);
        JsonGenerator *gen = json_generator_new();
        if (root && gen) {
          json_generator_set_root(gen, root);
          result = json_generator_to_data(gen, NULL);
        }
        if (gen) g_object_unref(gen);
        if (root) json_node_free(root);
      }
      if (db) g_object_unref(db);
      if (payload_b64) {
        secure_wipe(payload_b64, strlen(payload_b64));
        g_free(payload_b64);
      }
      signet_credential_access_grant_clear(&grant);
      if (result) {
        ok = true;
        code = "credential_delivered";
        message = g_strdup("credential delivered through encrypted ContextVM");
      } else {
        code = "internal_error";
        message = g_strdup("failed to build encrypted credential delivery");
      }
      break;
    }

    case SIGNET_MGMT_OP_ROTATE_CREDENTIAL: {
      SignetStore *base_store = signet_key_store_get_store(h->keys);
      uint8_t *payload = NULL;
      size_t payload_len = 0;
      SignetSecretMetadata metadata;
      memset(&metadata, 0, sizeof(metadata));
      if (!base_store) {
        code = "no_store";
        message = g_strdup("persistent store not available");
        break;
      }
      if (signet_mgmt_decode_secret_payload(req.payload_b64,
                                            &payload, &payload_len) != 0) {
        code = "invalid_credential";
        message = g_strdup("invalid credential payload");
        break;
      }
      SignetSecretResult src = signet_store_rotate_secret_ex(
          base_store, req.credential_id, payload, payload_len,
          req.has_credential_expires_at, req.credential_expires_at,
          now, &metadata);
      sodium_free(payload);
      if (src == SIGNET_SECRET_OK) {
        result = signet_mgmt_secret_metadata_json(&metadata);
        if (result) {
          ok = true;
          code = "credential_rotated";
          message = g_strdup("credential rotated; prior version archived");
          signet_mgmt_publish_cas_audit(
              h, "credential_rotate", metadata.agent_id, "ok", now);
        } else {
          code = "internal_error";
          message = g_strdup("failed to build credential metadata");
        }
      } else if (src == SIGNET_SECRET_NOT_FOUND) {
        code = "credential_not_found";
        message = g_strdup("credential not found");
      } else if (src == SIGNET_SECRET_REVOKED) {
        code = "credential_revoked";
        message = g_strdup("revoked credential cannot be rotated");
      } else if (src == SIGNET_SECRET_EXPIRED) {
        code = "credential_expired";
        message = g_strdup("expired credential requires a replacement expiry");
      } else {
        code = "credential_store_failed";
        message = g_strdup("credential rotation failed");
      }
      signet_secret_metadata_clear(&metadata);
      break;
    }

    case SIGNET_MGMT_OP_REVOKE_CREDENTIAL: {
      SignetStore *base_store = signet_key_store_get_store(h->keys);
      SignetSecretMetadata metadata;
      memset(&metadata, 0, sizeof(metadata));
      if (!base_store) {
        code = "no_store";
        message = g_strdup("persistent store not available");
        break;
      }
      SignetSecretResult src = signet_store_revoke_secret(
          base_store, req.credential_id, now, &metadata);
      if (src == SIGNET_SECRET_OK) {
        result = signet_mgmt_secret_metadata_json(&metadata);
        if (result) {
          ok = true;
          code = "credential_revoked";
          message = g_strdup("credential revoked");
          signet_mgmt_publish_cas_audit(
              h, "credential_revoke", metadata.agent_id, "ok", now);
        } else {
          code = "internal_error";
          message = g_strdup("failed to build credential metadata");
        }
      } else if (src == SIGNET_SECRET_NOT_FOUND) {
        code = "credential_not_found";
        message = g_strdup("credential not found");
      } else {
        code = "credential_store_failed";
        message = g_strdup("credential revocation failed");
      }
      signet_secret_metadata_clear(&metadata);
      break;
    }

    case SIGNET_MGMT_OP_DELETE_CREDENTIAL: {
      SignetStore *base_store = signet_key_store_get_store(h->keys);
      if (!base_store) {
        code = "no_store";
        message = g_strdup("persistent store not available");
        break;
      }
      SignetSecretMetadata metadata;
      memset(&metadata, 0, sizeof(metadata));
      SignetSecretResult inspect_rc = signet_store_get_secret_metadata(
          base_store, req.credential_id, now, &metadata);
      SignetSecretResult src = inspect_rc == SIGNET_SECRET_OK
          ? signet_store_delete_revoked_secret(base_store, req.credential_id)
          : inspect_rc;
      if (src == SIGNET_SECRET_OK) {
        JsonBuilder *db = json_builder_new();
        if (db) {
          json_builder_begin_object(db);
          json_builder_set_member_name(db, "credential_id");
          json_builder_add_string_value(db, req.credential_id);
          json_builder_set_member_name(db, "agent_id");
          json_builder_add_string_value(db, metadata.agent_id ? metadata.agent_id : "");
          json_builder_set_member_name(db, "deleted");
          json_builder_add_boolean_value(db, TRUE);
          json_builder_end_object(db);
          JsonNode *root = json_builder_get_root(db);
          JsonGenerator *gen = json_generator_new();
          if (root && gen) {
            json_generator_set_root(gen, root);
            result = json_generator_to_data(gen, NULL);
          }
          if (gen) g_object_unref(gen);
          if (root) json_node_free(root);
          g_object_unref(db);
        }
        if (result) {
          ok = true;
          code = "credential_deleted";
          message = g_strdup("revoked credential deleted");
          signet_mgmt_publish_cas_audit(
              h, "credential_delete", metadata.agent_id, "ok", now);
        } else {
          code = "internal_error";
          message = g_strdup("failed to build credential deletion result");
        }
      } else if (src == SIGNET_SECRET_NOT_FOUND) {
        code = "credential_not_found";
        message = g_strdup("credential not found");
      } else if (src == SIGNET_SECRET_NOT_REVOKED) {
        code = "credential_not_revoked";
        message = g_strdup("credential must be revoked before deletion");
      } else {
        code = "credential_store_failed";
        message = g_strdup("credential deletion failed");
      }
      signet_secret_metadata_clear(&metadata);
      break;
    }

    case SIGNET_MGMT_OP_GRANT_PROVISIONER:
      if (signet_store_grant_provisioner(
              h->provisioner_store, req.provisioner_pubkey,
              event_pubkey_hex, now) == 0) {
        ok = true;
        /* Write-back, not last-writer-wins: config is the desired state, so a
         * grant that never reaches config would be revoked by the next
         * reload. */
        if (signet_mgmt_writeback_provisioners(h) == 0) {
          code = "provisioner_granted";
          message = g_strdup("provisioner grant persisted and active");
        } else {
          code = "provisioner_granted_config_writeback_failed";
          message = g_strdup("provisioner grant is active but could not be "
                             "written back to config; it will not survive a "
                             "reload");
        }
      } else {
        code = "policy_store_failed";
        message = g_strdup("failed to persist provisioner grant");
      }
      break;

    case SIGNET_MGMT_OP_REVOKE_PROVISIONER:
      if (signet_store_revoke_provisioner_ex(
              h->provisioner_store, req.provisioner_pubkey,
              event_pubkey_hex, now) == 0) {
        ok = true;
        /* The revocation is already in force and tombstoned; write-back keeps
         * the config file from listing authority that no longer exists. */
        if (signet_mgmt_writeback_provisioners(h) == 0) {
          code = "provisioner_revoked";
          message = g_strdup("provisioner revocation persisted and active");
        } else {
          code = "provisioner_revoked_config_writeback_failed";
          message = g_strdup("provisioner revocation is persisted and active; "
                             "config write-back failed but the revocation "
                             "tombstone still blocks it from being restored "
                             "by a reload");
        }
      } else {
        code = "policy_store_failed";
        message = g_strdup("failed to persist provisioner revocation");
      }
      break;

    default:
      code = "unknown_command";
      message = g_strdup("unknown management command");
      break;
  }

  /* 3b) Hash-chained mutation audit: every executed management mutation
   * (and every failed attempt at one) leaves a redacted, tamper-evident
   * entry keyed to the affected identity/credential. Read-only ops
   * (status/list/inspect) are not mutations and are skipped. */
  {
    const char *audit_op = signet_mgmt_op_audit_name(req.op);
    const char *auth = signet_mgmt_op_delivery_grant_eligible(req.op)
        ? (sender_is_provisioner ? "provisioner" : "delivery_grant")
        : NULL;
    if (audit_op) {
      signet_mgmt_chain_audit_ex(
          h, audit_op,
          req.op == SIGNET_MGMT_OP_DELIVER_CREDENTIAL
              ? event_pubkey_hex
              : ((req.agent_id && req.agent_id[0]) ? req.agent_id
                                                    : event_pubkey_hex),
          audit_secret_id ? audit_secret_id :
              (req.credential_id ? req.credential_id : req.provisioner_pubkey),
          ok ? "allow" : "deny", audit_code ? audit_code : code, auth,
          NULL, now);
    }
  }
  g_free(audit_secret_id);

  /* 4) Publish ack. */
  char *ack = signet_mgmt_build_ack(req.request_id, ok, code, message, result);
  if (ack) {
    signet_mgmt_publish_ack(h, event_pubkey_hex, ack, event_id_hex, now,
                            reply_transport, rpc_id);
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

int signet_mgmt_handler_handle_request(SignetMgmtHandler *h,
                                       const char *event_pubkey_hex,
                                       const char *content_json,
                                       SignetMgmtOp op,
                                       const char *event_id_hex,
                                       int64_t now) {
  return signet_mgmt_handler_handle_request_ex(
      h, event_pubkey_hex, content_json, op, event_id_hex, now,
      SIGNET_MGMT_REPLY_LEGACY_ACK, NULL, now);
}
