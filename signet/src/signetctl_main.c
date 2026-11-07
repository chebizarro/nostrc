/* SPDX-License-Identifier: MIT
 *
 * signetctl_main.c - Nostr-native management CLI for Signet.
 *
 * Publishes signed management events to relays and waits for ack responses.
 * Uses the provisioner's file-backed nsec (SIGNET_PROVISIONER_NSEC_FILE) to sign.
 */

#include "secret_input.h"
#include "signet/signet_config.h"
#include "signet/mgmt_protocol.h"
#include "signet/relay_pool.h"
#include "signet/store.h"
#include "signet/store_leases.h"
#include "signet/store_audit.h"
#include "signet/store_secrets.h"
#include "signet/cli_secret_output.h"
#include "signet/util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <glib.h>
#include <json-glib/json-glib.h>

/* libnostr */
#include <sodium.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/nip19/nip19.h>
#include <nostr/nip17/nip17.h>   /* NIP-59/NIP-17 gift-wrap for ContextVM intents */

#include <secure_buf.h>
#include <glib-unix.h>
#include "relay_pool_private.h"
#include <signal.h>

#define SIGNETCTL_VERSION "0.8.1"
#define SIGNETCTL_TIMEOUT_SEC 10


/* ----------------------------- usage ------------------------------------- */

static void signetctl_usage(FILE *out) {
  fprintf(out,
    "signetctl %s - Nostr-native Signet management CLI\n"
    "\n"
    "Usage: signetctl [-c <config>] <command> [args]\n"
    "\n"
    "Commands:\n"
    "  provision <agent_id> [--deliver <bootstrap_pubkey>] [--ttl <sec>]\n"
    "                           Provision and optionally gift-wrap bunker URI\n"
    "  revoke <agent_id>        Revoke an agent identity\n"
    "  rotate <agent_id>        Rotate an agent identity key\n"
    "  adopt-existing <agent_id> --sec -\n"
    "                 [--expected-pubkey <hex>] [--deliver <bootstrap_pubkey>] [--ttl <sec>]\n"
    "                           Adopt an existing (BYO) keypair as an agent\n"
    "                           (--sec - reads the secret from stdin)\n"
    "  restore-existing <agent_id> (--sec -|--sec-from-store <db>)\n"
    "                 [--expected-pubkey <hex>]\n"
    "                           Restore an existing agent identity without\n"
    "                           revoking its persistent client bindings\n"
    "  reissue-connect <agent_id> [--out <path>] [--show-secret]\n"
    "                           Mint a fresh one-time connect_secret for an\n"
    "                           existing agent (explicit re-pairing). --out writes\n"
    "                           the secret atomically to <path> (0600);\n"
    "                           --show-secret prints it to stdout instead\n"
    "  list-clients <agent_id>  List the agent's persistent NIP-46 client\n"
    "                           bindings (via management plane)\n"
    "  revoke-client <client_pubkey>\n"
    "                           Soft-revoke a NIP-46 client binding; the\n"
    "                           client must re-pair with a fresh secret\n"
    "  set-policy <agent_id> <policy-json>\n"
    "                           Set an agent policy\n"
    "  grant-provisioner <pubkey>\n"
    "                           Persist and activate a provisioner grant\n"
    "  revoke-provisioner <pubkey>\n"
    "                           Persist and activate a provisioner revocation\n"
    "  writer-acquire <agent_id> <owner_pubkey>\n"
    "                           Assign the fenced identity's sole NIP-46 writer\n"
    "                           client (provisioner only; single-use client key)\n"
    "  status                   Query daemon health status\n"
    "  list                     List managed agents\n"
    "\n"
    "  list-agents [--verify]   List agents with details (local store);\n"
    "                           --verify decrypts each key and checks it derives\n"
    "                           the stored pubkey (STATUS: ok/DECRYPT_FAIL/MISMATCH)\n"
    "  list-sessions            List active sessions (local store)\n"
    "  list-leases              List active credential leases (local store)\n"
    "  verify-audit             Verify hash-chained audit log integrity\n"
    "\n"
    "  create-credential <agent_id> --type <api_token|credential> --label <label>\n"
    "                    [--policy-id <id>] [--expires-at <unix>] (--file <path>|--stdin)\n"
    "  import-credential <agent_id> --type <api_token|credential> --label <label>\n"
    "                    [--policy-id <id>] [--expires-at <unix>] (--file <path>|--stdin)\n"
    "  list-credentials [agent_id]       List payload-free credential metadata\n"
    "  inspect-credential <id>           Inspect payload-free metadata\n"
    "  deliver-credential <agent_id> <id> --out <path>\n"
    "                           Deliver through policy/lease/audit controls;\n"
    "                           writes atomically as 0600, never prints payload\n"
    "  rotate-credential <id> [--expires-at <unix>] (--file <path>|--stdin)\n"
    "  revoke-credential <id>            Soft-revoke a credential\n"
    "  delete-credential <id> --confirm Delete a previously revoked credential\n"
    "                           All credential operations use encrypted ContextVM;\n"
    "                           payload files must be regular owner-only files.\n"
    "  migrate-db               Migrate a legacy plaintext SQLite DB to SQLCipher\n"
    "  backup-db <path>         Write an encrypted point-in-time backup of the\n"
    "                           entire store (current + archived secrets, audit)\n"
    "                           keyed by SIGNET_BACKUP_KEY (never via argv)\n"
    "  restore-db <path> --confirm\n"
    "                           Restore an encrypted backup over the configured\n"
    "                           DB (offline; stop signetd first). Previous DB is\n"
    "                           kept at <db>.pre-restore\n"
    "\n"
    "Options:\n"
    "  -c <path>    Configuration file path\n"
    "  -h, --help   Show this help\n"
    "\n"
    "Environment:\n"
    "  SIGNET_PROVISIONER_NSEC_FILE  File containing provisioner nsec (required)\\n"
    "  SIGNET_RELAYS            Comma-separated relay URLs (or set in config)\n"
    "  SIGNET_DB_KEY_FILE       Protected master-key file (or legacy SIGNET_DB_KEY)\n"
    "  SIGNET_BACKUP_KEY_FILE   Protected backup-key file (or legacy SIGNET_BACKUP_KEY)\n"
    "\n"
    "Examples:\n"
    "  signetctl provision my-agent\n"
    "  signetctl adopt-existing my-agent --sec - --expected-pubkey <hex>\n"
    "  signetctl reissue-connect my-agent --out /run/agent/connect-secret\n"
    "  signetctl revoke my-agent\n"
    "  signetctl status\n"
    "  signetctl list\n",
    SIGNETCTL_VERSION);
}

/* -------------------- build Cascadia ContextVM intent -------------------- */

/* Map an operation to its Cascadia ContextVM JSON-RPC method name. */
static const char *signetctl_contextvm_method(SignetMgmtOp op) {
  switch (op) {
    case SIGNET_MGMT_OP_PROVISION_AGENT: return "agent/provision";
    case SIGNET_MGMT_OP_REVOKE_AGENT:    return "agent/revoke";
    case SIGNET_MGMT_OP_SET_POLICY:      return "agent/set-policy";
    case SIGNET_MGMT_OP_GET_STATUS:      return "agent/get-status";
    case SIGNET_MGMT_OP_LIST_AGENTS:     return "agent/list";
    case SIGNET_MGMT_OP_ROTATE_KEY:      return "agent/rotate-key";
    case SIGNET_MGMT_OP_ADOPT_EXISTING:  return "agent/adopt-existing";
    case SIGNET_MGMT_OP_RESTORE_EXISTING:return "agent/restore-existing";
    case SIGNET_MGMT_OP_REISSUE_CONNECT: return "agent/reissue-connect";
    case SIGNET_MGMT_OP_LIST_CLIENTS:    return "agent/list-clients";
    case SIGNET_MGMT_OP_REVOKE_CLIENT:   return "agent/revoke-client";
    case SIGNET_MGMT_OP_CREATE_CREDENTIAL: return "credential/create";
    case SIGNET_MGMT_OP_IMPORT_CREDENTIAL: return "credential/import";
    case SIGNET_MGMT_OP_LIST_CREDENTIALS: return "credential/list";
    case SIGNET_MGMT_OP_INSPECT_CREDENTIAL: return "credential/inspect";
    case SIGNET_MGMT_OP_DELIVER_CREDENTIAL: return "credential/deliver";
    case SIGNET_MGMT_OP_ROTATE_CREDENTIAL: return "credential/rotate";
    case SIGNET_MGMT_OP_REVOKE_CREDENTIAL: return "credential/revoke";
    case SIGNET_MGMT_OP_DELETE_CREDENTIAL: return "credential/delete";
    case SIGNET_MGMT_OP_GRANT_PROVISIONER: return "config/grant-provisioner";
    case SIGNET_MGMT_OP_REVOKE_PROVISIONER: return "config/revoke-provisioner";
    case SIGNET_MGMT_OP_WRITER_ACQUIRE: return "agent/writer-acquire";
    default:                          return NULL;
  }
}

static void signetctl_wipe_json(JsonNode *node) {
  if (!node) return;
  if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING) {
    const char *s = json_node_get_string(node);
    if (s) secure_wipe((void *)s, strlen(s));
  } else if (JSON_NODE_HOLDS_OBJECT(node)) {
    JsonObject *object = json_node_get_object(node);
    GList *members = json_object_get_members(object);
    for (GList *it = members; it; it = it->next)
      signetctl_wipe_json(json_object_get_member(object, it->data));
    g_list_free(members);
  } else if (JSON_NODE_HOLDS_ARRAY(node)) {
    JsonArray *array = json_node_get_array(node);
    for (guint i = 0; i < json_array_get_length(array); i++)
      signetctl_wipe_json(json_array_get_element(array, i));
  }
}

typedef JsonParser SignetSecretJsonParser;
static void signetctl_secret_parser_free(SignetSecretJsonParser *parser) {
  signetctl_wipe_json(json_parser_get_root(parser));
  g_object_unref(parser);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SignetSecretJsonParser, signetctl_secret_parser_free)

/* Build a Cascadia ContextVM JSON-RPC 2.0 intent:
 *   {"jsonrpc":"2.0","id":"<request_id>","method":"agent/...","params":{...}}
 * This is the kind-25910 CAS_INTENT payload the daemon parses. */
static char *signetctl_build_intent(SignetMgmtOp op, const char *agent_id, const char *request_id,
                                    const char *policy_json, const char *bootstrap_pubkey,
                                    int delivery_ttl, const char *agent_nsec,
                                    const char *expected_pubkey, const char *client_pubkey,
                                    const char *provisioner_pubkey,
                                    const char *credential_id, const char *secret_type,
                                    const char *label, const char *payload_b64,
                                    const char *credential_policy_id,
                                    bool has_expires_at, int64_t expires_at,
                                    const char *writer_pubkey) {
  const char *method = signetctl_contextvm_method(op);
  if (!method) return NULL;

  g_autoptr(SignetSecretJsonParser) policy_parser = NULL;
  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "jsonrpc"); json_builder_add_string_value(b, "2.0");
  json_builder_set_member_name(b, "id"); json_builder_add_string_value(b, request_id);
  json_builder_set_member_name(b, "method"); json_builder_add_string_value(b, method);

  json_builder_set_member_name(b, "params");
  json_builder_begin_object(b);
  if (agent_id) {
    json_builder_set_member_name(b, "agent_id");
    json_builder_add_string_value(b, agent_id);
  }
  if (agent_nsec) {
    json_builder_set_member_name(b, "agent_nsec");
    json_builder_add_string_value(b, agent_nsec);
  }
  if (expected_pubkey) {
    json_builder_set_member_name(b, "expected_pubkey");
    json_builder_add_string_value(b, expected_pubkey);
  }
  if (client_pubkey) {
    json_builder_set_member_name(b, "client_pubkey");
    json_builder_add_string_value(b, client_pubkey);
  }
  if (provisioner_pubkey) {
    json_builder_set_member_name(b, "provisioner_pubkey");
    json_builder_add_string_value(b, provisioner_pubkey);
  }
  if (writer_pubkey) {
    json_builder_set_member_name(b, "writer_pubkey");
    json_builder_add_string_value(b, writer_pubkey);
  }
  if (credential_id) {
    json_builder_set_member_name(b, "credential_id");
    json_builder_add_string_value(b, credential_id);
  }
  if (secret_type) {
    json_builder_set_member_name(b, "secret_type");
    json_builder_add_string_value(b, secret_type);
  }
  if (label) {
    json_builder_set_member_name(b, "label");
    json_builder_add_string_value(b, label);
  }
  if (payload_b64) {
    json_builder_set_member_name(b, "payload_b64");
    json_builder_add_string_value(b, payload_b64);
  }
  if (credential_policy_id) {
    json_builder_set_member_name(b, "policy_id");
    json_builder_add_string_value(b, credential_policy_id);
  }
  if (has_expires_at) {
    json_builder_set_member_name(b, "expires_at");
    json_builder_add_int_value(b, expires_at);
  }
  if (bootstrap_pubkey) {
    json_builder_set_member_name(b, "deliver");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_set_member_name(b, "bootstrap_pubkey");
    json_builder_add_string_value(b, bootstrap_pubkey);
    json_builder_set_member_name(b, "delivery_ttl");
    json_builder_add_int_value(b, delivery_ttl > 0 ? delivery_ttl : 600);
  }
  if (policy_json) {
    policy_parser = json_parser_new();
    if (json_parser_load_from_data(policy_parser, policy_json, -1, NULL)) {
      JsonNode *proot = json_parser_get_root(policy_parser);
      if (proot && JSON_NODE_HOLDS_OBJECT(proot)) {
        json_builder_set_member_name(b, "policy");
        json_builder_add_value(b, json_node_copy(proot));
      }
    }
  }
  json_builder_end_object(b); /* params */
  json_builder_end_object(b); /* root */

  JsonGenerator *gen = json_generator_new();
  if (!gen) { g_object_unref(b); return NULL; }
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);
  char *out = json_generator_to_data(gen, NULL);
  signetctl_wipe_json(root);
  json_node_free(root);
  g_object_unref(gen);
  g_object_unref(b);
  return out;
}

/* Gift-wrap (NIP-59/NIP-17, kind 1059) the intent to the bunker pubkey, signed
 * with the provisioner key. Returns the serialized gift-wrap event JSON. */
static char *signetctl_build_gift_wrapped_intent(const char *intent_json,
                                                 const char *bunker_pubkey_hex,
                                                 const char *provisioner_sk_hex) {
  if (!intent_json || !bunker_pubkey_hex || !provisioner_sk_hex) return NULL;
  NostrEvent *gw = nostr_nip17_wrap_dm(provisioner_sk_hex, bunker_pubkey_hex, intent_json);
  if (!gw) return NULL;
  char *json = nostr_event_serialize_compact(gw);
  nostr_event_free(gw);
  return json;
}

/* Print only agent_id, pubkey, bunker_uri from an adopt-existing JSON-RPC
 * reply. A bunker URI is a bearer secret, not public metadata. */
static void signetctl_print_adopt_result(const char *reply_json) {
  g_autoptr(SignetSecretJsonParser) p = json_parser_new();
  if (!reply_json || !json_parser_load_from_data(p, reply_json, -1, NULL)) {
    fprintf(stderr, "signetctl: invalid adopt/restore reply\n");
    return;
  }
  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return;
  JsonObject *o = json_node_get_object(root);
  JsonObject *res = o;
  if (json_object_has_member(o, "result")) {
    JsonNode *rn = json_object_get_member(o, "result");
    if (rn && JSON_NODE_HOLDS_OBJECT(rn)) res = json_node_get_object(rn);
  }
  const char *aid = json_object_has_member(res, "agent_id") ? json_object_get_string_member(res, "agent_id") : NULL;
  const char *pk  = json_object_has_member(res, "pubkey") ? json_object_get_string_member(res, "pubkey") : NULL;
  const char *uri = json_object_has_member(res, "bunker_uri") ? json_object_get_string_member(res, "bunker_uri") : NULL;
  if (aid) printf("agent_id: %s\n", aid);
  if (pk)  printf("pubkey: %s\n", pk);
  if (uri) printf("bunker_uri: [redacted; use reissue-connect --out <file>]\n");
}

/* Atomically write a secret to path with mode 0600: write to a same-directory
 * temp file (O_EXCL), fsync, then rename over the destination. Returns 0 on
 * success, -1 on error. Never logs the secret. */
static int signetctl_write_secret_file(const char *path, const char *secret) {
  if (!path || !path[0] || !secret) return -1;
  char *tmp = g_strdup_printf("%s.tmp.%ld", path, (long)getpid());
  int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    fprintf(stderr, "signetctl: failed to create '%s': %s\n", tmp, g_strerror(errno));
    g_free(tmp);
    return -1;
  }
  size_t len = strlen(secret);
  size_t off = 0;
  bool ok = true;
  while (off < len) {
    ssize_t wr = write(fd, secret + off, len - off);
    if (wr < 0) {
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    off += (size_t)wr;
  }
  if (ok) ok = (fsync(fd) == 0);
  close(fd);
  if (ok && rename(tmp, path) != 0) {
    fprintf(stderr, "signetctl: failed to rename '%s' -> '%s': %s\n", tmp, path, g_strerror(errno));
    ok = false;
  }
  if (ok) {
    /* Make the rename durable: fsync the parent directory. */
    char *dir = g_path_get_dirname(path);
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) { (void)fsync(dfd); close(dfd); }
    g_free(dir);
  }
  if (!ok) unlink(tmp);
  g_free(tmp);
  return ok ? 0 : -1;
}

/* Handle an agent/reissue-connect JSON-RPC reply. Prints non-secret fields;
 * the fresh connect_secret is written to out_path (atomic, 0600) when given,
 * or printed only with --show-secret. Returns process exit code. */
static int signetctl_handle_reissue_result(const char *reply_json,
                                           const char *out_path,
                                           bool show_secret) {
  g_autoptr(SignetSecretJsonParser) p = json_parser_new();
  if (!reply_json || !json_parser_load_from_data(p, reply_json, -1, NULL)) {
    fprintf(stderr, "signetctl: unparseable reissue-connect reply\n");
    return 1;
  }
  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    fprintf(stderr, "signetctl: malformed reissue-connect reply\n");
    return 1;
  }
  JsonObject *o = json_node_get_object(root);
  JsonObject *res = o;
  if (json_object_has_member(o, "result")) {
    JsonNode *rn = json_object_get_member(o, "result");
    if (rn && JSON_NODE_HOLDS_OBJECT(rn)) res = json_node_get_object(rn);
  }
  const char *aid = json_object_has_member(res, "agent_id") ? json_object_get_string_member(res, "agent_id") : NULL;
  const char *upk = json_object_has_member(res, "user_pubkey") ? json_object_get_string_member(res, "user_pubkey") : NULL;
  const char *bpk = json_object_has_member(res, "bunker_pubkey") ? json_object_get_string_member(res, "bunker_pubkey") : NULL;
  const char *sec = json_object_has_member(res, "connect_secret") ? json_object_get_string_member(res, "connect_secret") : NULL;
  const char *url = json_object_has_member(res, "bunker_uri") ? json_object_get_string_member(res, "bunker_uri") : NULL;

  if (!sec || !sec[0]) {
    fprintf(stderr, "signetctl: reply carried no connect_secret\n");
    return 1;
  }

  if (aid) printf("agent_id: %s\n", aid);
  if (upk) printf("user_pubkey: %s\n", upk);
  if (bpk) printf("bunker_pubkey: %s\n", bpk);

  if (out_path && out_path[0]) {
    if (signetctl_write_secret_file(out_path, sec) != 0) return 1;
    printf("connect_secret: written to %s\n", out_path);
  }
  if (show_secret) {
    printf("connect_secret: %s\n", sec);
    if (url) printf("bunker_uri: %s\n", url);
  } else if (!out_path) {
    printf("connect_secret: (withheld; pass --out <path> or --show-secret)\n");
    return 1;
  }
  return 0;
}

/* ---------------------- ack response handling ----------------------------- */

typedef enum { CONNECTING, REPLY_SUBSCRIBING, REQUEST_PENDING, COMPLETE } SignetctlRelayState;

typedef struct {
  SignetctlRelayState state;
  GMainLoop *loop;
  SignetRelayPool *pool;
  const char *event_json;
  const char *failure;
  char *selected_relay;
  GHashTable *closed_relays;
  size_t n_relays;
  bool received;
  bool is_error;                     /* true if the JSON-RPC reply carried an error */
  char *response_json;
  GMutex mu;
  GCond cond;
  /* Correlation / security fields. */
  char expected_request_id[17];      /* JSON-RPC id we sent */
  char expected_sender_hex[65];      /* bunker pubkey (reply's real sender) */
  char provisioner_sk_hex[65];       /* our SK for gift-wrap decrypt */
} SignetctlAckCtx;

static void signetctl_wipe_free_reply(char *value) {
  if (value) secure_wipe(value, strlen(value));
  free(value);
}

static void signetctl_on_event(const SignetRelayEventView *ev, void *user_data) {
  SignetctlAckCtx *ctx = (SignetctlAckCtx *)user_data;
  if (!ctx || !ev) return;

  /* Replies are Cascadia ContextVM JSON-RPC responses delivered as NIP-59
   * gift-wraps (kind 1059). Ignore anything else. */
  if (ev->kind != NIP59_GIFT_WRAP || !ev->event_json) return;

  /* Unwrap the gift-wrap with our provisioner key. Only replies addressed to us
   * decrypt successfully; `sender` is the real (seal) author = the bunker. */
  char *inner = NULL;
  char *sender = NULL;
  {
    NostrEvent *gw = nostr_event_new();
    if (gw && nostr_event_deserialize_compact(gw, ev->event_json, NULL)) {
      (void)nostr_nip17_decrypt_dm(gw, ctx->provisioner_sk_hex, &inner, &sender);
    }
    if (gw) nostr_event_free(gw);
  }
  if (!inner) { signetctl_wipe_free_reply(inner); free(sender); return; }

  /* The reply must come from our bunker. */
  if (ctx->expected_sender_hex[0] &&
      (!sender || g_ascii_strcasecmp(sender, ctx->expected_sender_hex) != 0)) {
    signetctl_wipe_free_reply(inner); free(sender);
    return;
  }

  /* Correlate by JSON-RPC id == our request id, and note error replies. An
   * error reply from our bunker with a null/absent id (e.g. unauthorized or
   * unknown-method) is still ours only when we did not have a request id to
   * correlate. A non-matching or missing id must not be allowed to poison a
   * later command from relay history. */
  bool is_error = false;
  bool id_ok = (ctx->expected_request_id[0] == '\0');
  {
    g_autoptr(SignetSecretJsonParser) p = json_parser_new();
    if (json_parser_load_from_data(p, inner, -1, NULL)) {
      JsonNode *root = json_parser_get_root(p);
      if (root && JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject *o = json_node_get_object(root);
        if (json_object_has_member(o, "error")) is_error = true;
        if (json_object_has_member(o, "id")) {
          JsonNode *idn = json_object_get_member(o, "id");
          const char *rid = (JSON_NODE_HOLDS_VALUE(idn) &&
                             json_node_get_value_type(idn) == G_TYPE_STRING)
                              ? json_node_get_string(idn) : NULL;
          if (ctx->expected_request_id[0] && rid &&
              strcmp(rid, ctx->expected_request_id) == 0)
            id_ok = true;
        }
      }
    }
  }
  if (is_error && ctx->expected_request_id[0] && !id_ok) {
    signetctl_wipe_free_reply(inner); free(sender);
    return;
  }
  if (!id_ok && !is_error) { signetctl_wipe_free_reply(inner); free(sender); return; }

  g_mutex_lock(&ctx->mu);
  if (!ctx->received && ctx->state == REQUEST_PENDING) {
    ctx->state = COMPLETE;
    ctx->received = true;
    ctx->is_error = is_error;
    ctx->response_json = g_strdup(inner);
    g_cond_signal(&ctx->cond);
    g_main_loop_quit(ctx->loop);
  }
  g_mutex_unlock(&ctx->mu);
  signetctl_wipe_free_reply(inner);
  free(sender);
}

/* Relay callbacks and deadlines drive one operation; never re-sign/re-execute
 * a mutation after an ambiguous transport result. */
static void signetctl_fail(SignetctlAckCtx *ctx, const char *failure) {
  g_mutex_lock(&ctx->mu);
  if (ctx->state != COMPLETE) {
    ctx->state = COMPLETE;
    ctx->failure = failure;
    g_main_loop_quit(ctx->loop);
  }
  g_mutex_unlock(&ctx->mu);
}
static gboolean signetctl_deadline(gpointer data) {
  signetctl_fail(data, "operation deadline; mutation outcome is unknown, do not blindly retry");
  return G_SOURCE_CONTINUE;
}
static gboolean signetctl_cancel(gpointer data) {
  signetctl_fail(data, "cancelled; mutation outcome may be unknown");
  return G_SOURCE_CONTINUE;
}
static void signetctl_publish_ok(const char *id, bool accepted, const char *reason, void *data) {
  (void)id;
  if (!accepted) signetctl_fail(data, !g_strcmp0(reason, "transport-aborted:")
    ? "transport aborted; mutation outcome is unknown" : "relay rejected request or did not acknowledge it");
}
static void signetctl_subscription(const char *url, const char *state,
                                    const char *reason, void *data) {
  SignetctlAckCtx *ctx = data;
  g_mutex_lock(&ctx->mu);
  SignetctlRelayState current = ctx->state;
  g_mutex_unlock(&ctx->mu);
  if (current == COMPLETE) return;
  if (!strcmp(state, "CONNECTED")) g_hash_table_remove(ctx->closed_relays, url);
  if (!strcmp(state, "CONNECTED") && current == CONNECTING) {
    g_mutex_lock(&ctx->mu);
    ctx->state = REPLY_SUBSCRIBING;
    g_mutex_unlock(&ctx->mu);
    int kinds[] = { NIP59_GIFT_WRAP };
    char *pubkey = nostr_key_get_public(ctx->provisioner_sk_hex);
    int rc = pubkey ? signet_relay_pool_subscribe_scoped(ctx->pool, kinds, 1, pubkey,
                           (int64_t)time(NULL) - 49 * 3600) : -1;
    free(pubkey);
    if (rc) signetctl_fail(ctx, "reply subscription failed");
  } else if (!strcmp(state, "EOSE") && current == REPLY_SUBSCRIBING &&
             signet_relay_pool_is_subscribed(ctx->pool)) {
    g_mutex_lock(&ctx->mu);
    ctx->selected_relay = g_strdup(url);
    ctx->state = REQUEST_PENDING; /* before send: a fast reply cannot be missed */
    g_mutex_unlock(&ctx->mu);
    /* Exactly one caught-up relay: another relay's rejection cannot race and
     * poison a viable path. A transport failure is ambiguous, never retried. */
    if (signet_relay_pool_publish_selected_ack(ctx->pool, ctx->event_json, url,
                                               signetctl_publish_ok, ctx))
      signetctl_fail(ctx, "request could not be enqueued");
  } else if (!strcmp(state, "CLOSED")) {
    if (!reason || !g_str_has_prefix(reason, "auth-required:")) {
      g_hash_table_add(ctx->closed_relays, g_strdup(url));
      if ((ctx->selected_relay && !strcmp(url, ctx->selected_relay)) ||
          (!ctx->selected_relay && g_hash_table_size(ctx->closed_relays) >= ctx->n_relays))
        signetctl_fail(ctx, "relay closed reply subscription");
    }
  } else if (!strcmp(state, "DISCONNECTED") && current != CONNECTING &&
             !signet_relay_pool_is_connected(ctx->pool)) {
    signetctl_fail(ctx, "relay connection failed; mutation outcome may be unknown");
  }
}

/* ---------------------- resolve provisioner key --------------------------- */

static bool signetctl_resolve_provisioner_key(char *out_sk_hex, size_t sz) {
  if (!out_sk_hex || sz < 65) return false;
  const char *legacy = g_getenv("SIGNET_PROVISIONER_NSEC");
  if (legacy && legacy[0]) {
    fprintf(stderr, "signetctl: SIGNET_PROVISIONER_NSEC is no longer accepted; "
                    "mount the secret and set SIGNET_PROVISIONER_NSEC_FILE\n");
    return false;
  }
  const char *path = g_getenv("SIGNET_PROVISIONER_NSEC_FILE");
  if (!path || !path[0]) return false;

  char *contents = NULL;
  if (signet_secret_input_file(path, &contents) != 0) {
    fprintf(stderr, "signetctl: cannot read protected SIGNET_PROVISIONER_NSEC_FILE\n");
    return false;
  }
  const char *nsec = contents;
  bool ok = false;

  uint8_t raw[32] = {0};
  if (g_ascii_strncasecmp(nsec, "nsec1", 5) == 0)
    ok = nostr_nip19_decode_nsec(nsec, raw) == 0;
  else
    ok = signet_hex_to_bytes32(nsec, raw);
  if (ok) {
    for (int i = 0; i < 32; i++) sprintf(out_sk_hex + i * 2, "%02x", raw[i]);
    out_sk_hex[64] = '\0';
  }
  sodium_memzero(raw, sizeof(raw));

  sodium_free(contents);
  return ok;
}

/* Caller owns the locked result. No secret survives a failed load. */
static char *signetctl_load_agent_secret(const char *store_path, const char *agent_id) {
  char *secret = sodium_malloc(256);
  if (!secret) return NULL;
  memset(secret, 0, 256);
  if (store_path) {
    char *key = NULL;
    if (signet_secret_input_resolve("SIGNET_DB_KEY", "SIGNET_DB_KEY_FILE", &key) != 0) {
      sodium_free(secret);
      return NULL;
    }
    SignetStoreConfig scfg = {.db_path = store_path, .master_key = key, .read_only = true};
    SignetStore *source = signet_store_open(&scfg);
    sodium_free(key);
    SignetAgentRecord rec = {0};
    bool ok = source && signet_store_get_agent(source, agent_id, &rec) == 0 &&
              rec.secret_key && rec.secret_key_len == 32;
    if (ok) {
      for (int i = 0; i < 32; i++) sprintf(secret + i * 2, "%02x", rec.secret_key[i]);
    }
    signet_agent_record_clear(&rec);
    if (source) signet_store_close(source);
    if (!ok) { sodium_free(secret); return NULL; }
  } else {
    size_t n = 0;
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
      if (c == 0 || n == 255) { sodium_free(secret); return NULL; }
      secret[n++] = (char)c;
    }
    if (ferror(stdin) || !n) { sodium_free(secret); return NULL; }
    if (n && secret[n - 1] == '\r') --n;
    secret[n] = '\0';
  }
  uint8_t raw[32] = {0};
  bool ok = signet_hex_to_bytes32(secret, raw) ||
            (g_str_has_prefix(secret, "nsec1") && nostr_nip19_decode_nsec(secret, raw) == 0);
  sodium_memzero(raw, sizeof(raw));
  if (!ok) { sodium_free(secret); return NULL; }
  return secret;
}

#define SIGNETCTL_MAX_CREDENTIAL_PAYLOAD (1024u * 1024u)

static bool signetctl_parse_i64(const char *s, int64_t *out) {
  if (!s || !s[0] || !out) return false;
  char *end = NULL;
  errno = 0;
  gint64 value = g_ascii_strtoll(s, &end, 10);
  if (errno == ERANGE || !end || *end != '\0') return false;
  *out = (int64_t)value;
  return true;
}

static int signetctl_read_credential_payload(const char *input_file,
                                             bool from_stdin,
                                             uint8_t **out_payload,
                                             size_t *out_len) {
  if (!out_payload || !out_len || (!!input_file == from_stdin)) return -1;
  *out_payload = NULL;
  *out_len = 0;

  int fd = STDIN_FILENO;
  if (input_file) {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(input_file, flags);
    if (fd < 0) {
      fprintf(stderr, "signetctl: cannot open protected input file '%s': %s\n",
              input_file, g_strerror(errno));
      return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0077) != 0) {
      fprintf(stderr, "signetctl: input file must be a regular, owner-only "
                      "(0600 or stricter), non-symlink file owned by this user\n");
      close(fd);
      return -1;
    }
    if (st.st_size <= 0 ||
        (uint64_t)st.st_size > SIGNETCTL_MAX_CREDENTIAL_PAYLOAD) {
      fprintf(stderr, "signetctl: credential input must be 1..%u bytes\n",
              SIGNETCTL_MAX_CREDENTIAL_PAYLOAD);
      close(fd);
      return -1;
    }
  }

  uint8_t *payload = sodium_malloc(SIGNETCTL_MAX_CREDENTIAL_PAYLOAD + 1u);
  if (!payload) {
    if (input_file) close(fd);
    return -1;
  }
  size_t total = 0;
  for (;;) {
    ssize_t n = read(fd, payload + total,
                     SIGNETCTL_MAX_CREDENTIAL_PAYLOAD + 1u - total);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      fprintf(stderr, "signetctl: failed to read credential input: %s\n",
              g_strerror(errno));
      sodium_free(payload);
      if (input_file) close(fd);
      return -1;
    }
    if (n == 0) break;
    total += (size_t)n;
    if (total > SIGNETCTL_MAX_CREDENTIAL_PAYLOAD) {
      fprintf(stderr, "signetctl: credential input exceeds %u bytes\n",
              SIGNETCTL_MAX_CREDENTIAL_PAYLOAD);
      sodium_free(payload);
      if (input_file) close(fd);
      return -1;
    }
  }
  if (input_file) close(fd);
  if (total == 0) {
    fprintf(stderr, "signetctl: credential input is empty\n");
    sodium_free(payload);
    return -1;
  }

  *out_payload = payload;
  *out_len = total;
  return 0;
}

static void signetctl_wipe_free_string(char *value) {
  if (!value) return;
  secure_wipe(value, strlen(value));
  g_free(value);
}

/* ------------------------------ main ------------------------------------- */

int main(int argc, char **argv) {
  if (sodium_init() < 0) {
    fprintf(stderr, "signetctl: libsodium initialization failed\n");
    return 1;
  }
  const char *config_path = NULL;
  int argi = 1;

  while (argi < argc) {
    if (strcmp(argv[argi], "-c") == 0 && (argi + 1) < argc) {
      config_path = argv[argi + 1];
      argi += 2;
    } else if (strcmp(argv[argi], "-h") == 0 || strcmp(argv[argi], "--help") == 0) {
      signetctl_usage(stdout);
      return 0;
    } else {
      break;
    }
  }

  if (argi >= argc) {
    signetctl_usage(stderr);
    return 2;
  }

  const char *cmd = argv[argi++];

  /* Determine ContextVM operation and agent_id. */
  SignetMgmtOp op = SIGNET_MGMT_OP_UNKNOWN;
  const char *agent_id = NULL;
  const char *deliver_bootstrap_pubkey = NULL;
  int delivery_ttl = 600;
  const char *adopt_sec = NULL;
  const char *restore_store_path = NULL;
  const char *adopt_expected_pubkey = NULL;
  const char *reissue_out_path = NULL;
  bool reissue_show_secret = false;
  const char *revoke_client_pubkey = NULL;
  const char *provisioner_pubkey = NULL;
  const char *writer_pubkey = NULL;
  const char *credential_id = NULL;
  const char *credential_type = NULL;
  const char *credential_label = NULL;
  const char *credential_policy_id = NULL;
  const char *credential_input_file = NULL;
  const char *credential_output_file = NULL;
  bool credential_from_stdin = false;
  bool has_credential_expires_at = false;
  int64_t credential_expires_at = 0;
  bool credential_delete_confirmed = false;
  char *credential_payload_b64 = NULL;

  if (strcmp(cmd, "provision") == 0) {
    op = SIGNET_MGMT_OP_PROVISION_AGENT;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: provision requires <agent_id>\n");
      return 2;
    }
    agent_id = argv[argi++];
    while (argi < argc) {
      if (strcmp(argv[argi], "--deliver") == 0 && (argi + 1) < argc) {
        deliver_bootstrap_pubkey = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--ttl") == 0 && (argi + 1) < argc) {
        delivery_ttl = atoi(argv[argi + 1]);
        argi += 2;
      } else {
        fprintf(stderr, "signetctl: unknown provision option '%s'\n", argv[argi]);
        return 2;
      }
    }
  } else if (strcmp(cmd, "revoke") == 0) {
    op = SIGNET_MGMT_OP_REVOKE_AGENT;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: revoke requires <agent_id>\n");
      return 2;
    }
    agent_id = argv[argi++];
  } else if (strcmp(cmd, "rotate") == 0) {
    op = SIGNET_MGMT_OP_ROTATE_KEY;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: rotate requires <agent_id>\n");
      return 2;
    }
    agent_id = argv[argi++];
  } else if (strcmp(cmd, "reissue-connect") == 0) {
    op = SIGNET_MGMT_OP_REISSUE_CONNECT;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: reissue-connect requires <agent_id>\n");
      return 2;
    }
    agent_id = argv[argi++];
    while (argi < argc) {
      if (strcmp(argv[argi], "--out") == 0 && (argi + 1) < argc) {
        reissue_out_path = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--show-secret") == 0) {
        reissue_show_secret = true;
        argi += 1;
      } else {
        fprintf(stderr, "signetctl: unknown reissue-connect option '%s'\n", argv[argi]);
        return 2;
      }
    }
  } else if (strcmp(cmd, "list-clients") == 0) {
    op = SIGNET_MGMT_OP_LIST_CLIENTS;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: list-clients requires <agent_id>\n");
      return 2;
    }
    agent_id = argv[argi++];
  } else if (strcmp(cmd, "revoke-client") == 0) {
    op = SIGNET_MGMT_OP_REVOKE_CLIENT;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: revoke-client requires <client_pubkey>\n");
      return 2;
    }
    revoke_client_pubkey = argv[argi++];
  } else if (strcmp(cmd, "grant-provisioner") == 0 ||
             strcmp(cmd, "revoke-provisioner") == 0) {
    op = strcmp(cmd, "grant-provisioner") == 0
           ? SIGNET_MGMT_OP_GRANT_PROVISIONER
           : SIGNET_MGMT_OP_REVOKE_PROVISIONER;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: %s requires <pubkey>\n", cmd);
      return 2;
    }
    provisioner_pubkey = argv[argi++];
    if (argi != argc) {
      fprintf(stderr, "signetctl: unexpected %s argument\n", cmd);
      return 2;
    }
  } else if (strcmp(cmd, "writer-acquire") == 0) {
    op = SIGNET_MGMT_OP_WRITER_ACQUIRE;
    if (argc - argi != 2) {
      fprintf(stderr, "signetctl: writer-acquire requires <agent_id> <owner_pubkey>\n");
      return 2;
    }
    agent_id = argv[argi++];
    writer_pubkey = argv[argi++];
    if (!signet_hex_to_bytes32(writer_pubkey, (uint8_t[32]){0})) {
      fprintf(stderr, "signetctl: invalid writer pubkey\n");
      return 2;
    }
  } else if (strcmp(cmd, "adopt-existing") == 0 ||
             strcmp(cmd, "restore-existing") == 0) {
    bool restoring = strcmp(cmd, "restore-existing") == 0;
    op = restoring ? SIGNET_MGMT_OP_RESTORE_EXISTING
                   : SIGNET_MGMT_OP_ADOPT_EXISTING;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: %s requires <agent_id>\n", cmd);
      return 2;
    }
    agent_id = argv[argi++];
    while (argi < argc) {
      if (strcmp(argv[argi], "--sec") == 0 && (argi + 1) < argc) {
        if (adopt_sec || restore_store_path || strcmp(argv[argi + 1], "-") != 0) {
          fprintf(stderr, "signetctl: choose exactly one secret source; --sec accepts only -\n");
          return 2;
        }
        adopt_sec = argv[argi + 1];
        argi += 2;
      } else if (restoring && strcmp(argv[argi], "--sec-from-store") == 0 &&
                 (argi + 1) < argc) {
        if (restore_store_path || adopt_sec || !argv[argi + 1][0]) {
          fprintf(stderr, "signetctl: choose exactly one secret source\n");
          return 2;
        }
        restore_store_path = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--expected-pubkey") == 0 && (argi + 1) < argc) {
        adopt_expected_pubkey = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--deliver") == 0 && (argi + 1) < argc) {
        deliver_bootstrap_pubkey = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--ttl") == 0 && (argi + 1) < argc) {
        delivery_ttl = atoi(argv[argi + 1]);
        argi += 2;
      } else {
        fprintf(stderr, "signetctl: unknown %s option '%s'\n", cmd, argv[argi]);
        return 2;
      }
    }
    if ((!adopt_sec || !adopt_sec[0]) && !restore_store_path) {
      fprintf(stderr, "signetctl: %s requires a secret source\n", cmd);
      return 2;
    }
    if (adopt_sec && restore_store_path) {
      fprintf(stderr, "signetctl: choose exactly one secret source\n");
      return 2;
    }
    if (adopt_sec && strcmp(adopt_sec, "-") != 0) {
      fprintf(stderr, "signetctl: secret input is accepted only from stdin\n");
      return 2;
    }
  } else if (strcmp(cmd, "set-policy") == 0) {
    op = SIGNET_MGMT_OP_SET_POLICY;
    if ((argi + 1) >= argc) {
      fprintf(stderr, "signetctl: set-policy requires <agent_id> <policy-json>\n");
      return 2;
    }
    agent_id = argv[argi++];
  } else if (strcmp(cmd, "status") == 0) {
    op = SIGNET_MGMT_OP_GET_STATUS;
  } else if (strcmp(cmd, "list") == 0) {
    op = SIGNET_MGMT_OP_LIST_AGENTS;
  } else if (strcmp(cmd, "create-credential") == 0 ||
             strcmp(cmd, "import-credential") == 0) {
    op = strcmp(cmd, "create-credential") == 0
           ? SIGNET_MGMT_OP_CREATE_CREDENTIAL
           : SIGNET_MGMT_OP_IMPORT_CREDENTIAL;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: %s requires <agent_id>\n", cmd);
      return 2;
    }
    agent_id = argv[argi++];
    while (argi < argc) {
      if (strcmp(argv[argi], "--type") == 0 && argi + 1 < argc) {
        credential_type = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--label") == 0 && argi + 1 < argc) {
        credential_label = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--policy-id") == 0 && argi + 1 < argc) {
        credential_policy_id = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--expires-at") == 0 && argi + 1 < argc) {
        if (!signetctl_parse_i64(argv[argi + 1], &credential_expires_at) ||
            credential_expires_at < 0) {
          fprintf(stderr, "signetctl: invalid --expires-at value\n");
          return 2;
        }
        has_credential_expires_at = true;
        argi += 2;
      } else if (strcmp(argv[argi], "--file") == 0 && argi + 1 < argc) {
        if (credential_input_file) {
          fprintf(stderr, "signetctl: --file may be specified only once\n");
          return 2;
        }
        credential_input_file = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--stdin") == 0) {
        if (credential_from_stdin) {
          fprintf(stderr, "signetctl: --stdin may be specified only once\n");
          return 2;
        }
        credential_from_stdin = true;
        argi++;
      } else {
        fprintf(stderr, "signetctl: unknown %s option '%s'\n", cmd, argv[argi]);
        return 2;
      }
    }
    SignetSecretType parsed_type;
    if (!credential_type ||
        !signet_secret_type_parse(credential_type, &parsed_type) ||
        (parsed_type != SIGNET_SECRET_API_TOKEN &&
         parsed_type != SIGNET_SECRET_CREDENTIAL) ||
        !credential_label || !credential_label[0] ||
        (!!credential_input_file == credential_from_stdin)) {
      fprintf(stderr, "signetctl: %s requires --type <api_token|credential>, "
                      "--label <label>, and exactly one of --file/--stdin\n", cmd);
      return 2;
    }
  } else if (strcmp(cmd, "list-credentials") == 0) {
    op = SIGNET_MGMT_OP_LIST_CREDENTIALS;
    if (argi < argc) agent_id = argv[argi++];
    if (argi != argc) {
      fprintf(stderr, "signetctl: list-credentials accepts at most [agent_id]\n");
      return 2;
    }
  } else if (strcmp(cmd, "inspect-credential") == 0) {
    op = SIGNET_MGMT_OP_INSPECT_CREDENTIAL;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: inspect-credential requires <id>\n");
      return 2;
    }
    credential_id = argv[argi++];
    if (argi != argc) {
      fprintf(stderr, "signetctl: unexpected inspect-credential argument\n");
      return 2;
    }
  } else if (strcmp(cmd, "deliver-credential") == 0) {
    op = SIGNET_MGMT_OP_DELIVER_CREDENTIAL;
    if (argi + 1 >= argc) {
      fprintf(stderr, "signetctl: deliver-credential requires <agent_id> <id> --out <path>\n");
      return 2;
    }
    agent_id = argv[argi++];
    credential_id = argv[argi++];
    if (argi + 1 < argc && strcmp(argv[argi], "--out") == 0) {
      credential_output_file = argv[argi + 1];
      argi += 2;
    }
    if (!credential_output_file || argi != argc) {
      fprintf(stderr, "signetctl: deliver-credential requires exactly one --out <path>\n");
      return 2;
    }
  } else if (strcmp(cmd, "rotate-credential") == 0) {
    op = SIGNET_MGMT_OP_ROTATE_CREDENTIAL;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: rotate-credential requires <id>\n");
      return 2;
    }
    credential_id = argv[argi++];
    while (argi < argc) {
      if (strcmp(argv[argi], "--file") == 0 && argi + 1 < argc) {
        if (credential_input_file) {
          fprintf(stderr, "signetctl: --file may be specified only once\n");
          return 2;
        }
        credential_input_file = argv[argi + 1];
        argi += 2;
      } else if (strcmp(argv[argi], "--stdin") == 0) {
        if (credential_from_stdin) {
          fprintf(stderr, "signetctl: --stdin may be specified only once\n");
          return 2;
        }
        credential_from_stdin = true;
        argi++;
      } else if (strcmp(argv[argi], "--expires-at") == 0 && argi + 1 < argc) {
        if (!signetctl_parse_i64(argv[argi + 1], &credential_expires_at) ||
            credential_expires_at < 0) {
          fprintf(stderr, "signetctl: invalid --expires-at value\n");
          return 2;
        }
        has_credential_expires_at = true;
        argi += 2;
      } else {
        fprintf(stderr, "signetctl: unknown rotate-credential option '%s'\n",
                argv[argi]);
        return 2;
      }
    }
    if (!!credential_input_file == credential_from_stdin) {
      fprintf(stderr, "signetctl: rotate-credential requires exactly one of "
                      "--file <path> or --stdin\n");
      return 2;
    }
  } else if (strcmp(cmd, "revoke-credential") == 0) {
    op = SIGNET_MGMT_OP_REVOKE_CREDENTIAL;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: revoke-credential requires <id>\n");
      return 2;
    }
    credential_id = argv[argi++];
    if (argi != argc) {
      fprintf(stderr, "signetctl: unexpected revoke-credential argument\n");
      return 2;
    }
  } else if (strcmp(cmd, "delete-credential") == 0) {
    op = SIGNET_MGMT_OP_DELETE_CREDENTIAL;
    if (argi >= argc) {
      fprintf(stderr, "signetctl: delete-credential requires <id> --confirm\n");
      return 2;
    }
    credential_id = argv[argi++];
    if (argi < argc && strcmp(argv[argi], "--confirm") == 0) {
      credential_delete_confirmed = true;
      argi++;
    }
    if (!credential_delete_confirmed || argi != argc) {
      fprintf(stderr, "signetctl: delete-credential requires explicit --confirm\n");
      return 2;
    }
  } else if (strcmp(cmd, "migrate-db") == 0) {
    /* Migrate a legacy plaintext SQLite database to SQLCipher in place. */
    SignetConfig lcfg;
    if (signet_config_load(config_path, &lcfg) != 0) {
      fprintf(stderr, "signetctl: failed to load config\n");
      return 1;
    }
    char *db_key = NULL;
    int db_key_rc = signet_secret_input_resolve("SIGNET_DB_KEY", "SIGNET_DB_KEY_FILE", &db_key);
    int ret = 1;
    if (db_key_rc < 0) {
      fprintf(stderr, "signetctl: invalid database key input\n");
    } else if (!signet_store_sqlcipher_available()) {
      fprintf(stderr, "signetctl: this build is not linked against SQLCipher; "
              "cannot migrate. Rebuild with -Dsignet_use_sqlcipher=true.\n");
    } else if (!db_key || !db_key[0]) {
      fprintf(stderr, "signetctl: SIGNET_DB_KEY is required to migrate.\n");
    } else if (!signet_store_file_is_plaintext_sqlite(lcfg.db_path)) {
      fprintf(stdout, "signetctl: '%s' is not a legacy plaintext SQLite database; "
              "nothing to migrate.\n", lcfg.db_path);
      ret = 0;
    } else if (signet_store_migrate_plaintext_to_sqlcipher(lcfg.db_path, db_key) == 0) {
      fprintf(stdout, "signetctl: migrated '%s' to SQLCipher.\n"
              "  A plaintext backup remains at '%s.plaintext-backup' and still "
              "contains cleartext secrets;\n  securely delete it once you have "
              "verified the migration.\n", lcfg.db_path, lcfg.db_path);
      ret = 0;
    } else {
      fprintf(stderr, "signetctl: migration of '%s' failed (original left intact).\n",
              lcfg.db_path);
    }
    sodium_free(db_key);
    signet_config_clear(&lcfg);
    return ret;
  } else if (strcmp(cmd, "backup-db") == 0 || strcmp(cmd, "restore-db") == 0) {
    /* Encrypted backup/restore of the local store. Keys come from the
     * protected files or legacy environment values — never argv. */
    bool is_backup = strcmp(cmd, "backup-db") == 0;
    const char *file_path = NULL;
    bool confirmed = false;
    for (int ai = argi; ai < argc; ai++) {
      if (strcmp(argv[ai], "--confirm") == 0) confirmed = true;
      else if (!file_path) file_path = argv[ai];
      else { fprintf(stderr, "signetctl: unexpected argument '%s'\n", argv[ai]); return 2; }
    }
    if (!file_path || !file_path[0]) {
      fprintf(stderr, "signetctl: %s requires a backup file path\n", cmd);
      return 2;
    }
    if (!is_backup && !confirmed) {
      fprintf(stderr, "signetctl: restore-db REPLACES the configured database "
              "and requires explicit --confirm (stop signetd first).\n");
      return 2;
    }
    SignetConfig lcfg;
    if (signet_config_load(config_path, &lcfg) != 0) {
      fprintf(stderr, "signetctl: failed to load config\n");
      return 1;
    }
    char *db_key = NULL;
    int db_key_rc = signet_secret_input_resolve("SIGNET_DB_KEY", "SIGNET_DB_KEY_FILE", &db_key);
    char *backup_key = NULL;
    int backup_key_rc = signet_secret_input_resolve("SIGNET_BACKUP_KEY", "SIGNET_BACKUP_KEY_FILE", &backup_key);
    int ret = 1;
    if (db_key_rc != 0 || backup_key_rc < 0) {
      fprintf(stderr, "signetctl: invalid or missing database/backup key input\n");
    } else if (!backup_key || !backup_key[0]) {
      fprintf(stderr, "signetctl: SIGNET_BACKUP_KEY is required (an independent "
              "key protecting the backup file; >= 32 bytes of key material).\n");
    } else if (is_backup) {
      SignetStoreConfig scfg = { .db_path = lcfg.db_path, .master_key = db_key };
      SignetStore *store = signet_store_open(&scfg);
      if (!store) {
        fprintf(stderr, "signetctl: failed to open store at %s\n", lcfg.db_path);
      } else {
        if (signet_store_backup(store, file_path, backup_key) == 0) {
          fprintf(stdout, "signetctl: encrypted backup written to '%s'.\n"
                  "  It contains current AND archived secrets (envelope-encrypted;\n"
                  "  payloads additionally require the master key on restore).\n",
                  file_path);
          ret = 0;
        } else {
          fprintf(stderr, "signetctl: backup to '%s' failed "
                  "(see the [signet] log line above).\n", file_path);
        }
        signet_store_close(store);
      }
    } else {
      if (signet_store_restore_backup(file_path, backup_key,
                                      lcfg.db_path, db_key) == 0) {
        fprintf(stdout, "signetctl: restored '%s' from backup '%s'.\n"
                "  The previous database (if any) was kept at '%s.pre-restore'.\n",
                lcfg.db_path, file_path, lcfg.db_path);
        ret = 0;
      } else {
        fprintf(stderr, "signetctl: restore of '%s' from '%s' failed; the "
                "original database is untouched.\n", lcfg.db_path, file_path);
      }
    }
    sodium_free(backup_key);
    sodium_free(db_key);
    signet_config_clear(&lcfg);
    return ret;
  } else if (strcmp(cmd, "list-agents") == 0 ||
             strcmp(cmd, "list-sessions") == 0 ||
             strcmp(cmd, "list-leases") == 0 ||
             strcmp(cmd, "verify-audit") == 0) {
    /* Local store introspection — handled separately below. */
    op = (SignetMgmtOp)-1;
  } else {
    fprintf(stderr, "signetctl: unknown command '%s'\n", cmd);
    return 2;
  }

  /* --- Local store introspection commands --- */
  if (op == (SignetMgmtOp)-1) {
    /* Optional flags for introspection commands. */
    bool verify_agents = false;
    for (int ai = argi; ai < argc; ai++) {
      if (strcmp(argv[ai], "--verify") == 0) verify_agents = true;
    }
    SignetConfig lcfg;
    if (signet_config_load(config_path, &lcfg) != 0) {
      fprintf(stderr, "signetctl: failed to load config\n");
      return 1;
    }
    char *db_key = NULL;
    int db_key_rc = signet_secret_input_resolve("SIGNET_DB_KEY", "SIGNET_DB_KEY_FILE", &db_key);
    if (db_key_rc < 0) {
      signet_config_clear(&lcfg);
      return 1;
    }
    /* Local introspection never mutates the daemon-owned store. */
    bool ro = true;
    SignetStoreConfig scfg = {
      .db_path = lcfg.db_path,
      .master_key = db_key ? db_key : "",
      .read_only = ro,
    };
    SignetStore *store = signet_store_open(&scfg);
    sodium_free(db_key);
    if (!store) {
      fprintf(stderr, "signetctl: failed to open store at %s "
              "(see the [signet] log line above for the underlying error)\n",
              lcfg.db_path);
      signet_config_clear(&lcfg);
      return 1;
    }

    int local_rc = 0;
    if (strcmp(cmd, "list-agents") == 0) {
      char **ids = NULL;
      size_t count = 0;
      if (signet_store_list_agents(store, &ids, &count) == 0) {
        if (verify_agents)
          printf("%-24s %-11s %-64s %-13s %s\n",
                 "AGENT_ID", "PROVENANCE", "PUBKEY", "STATUS", "CREATED_AT");
        else
          printf("%-24s %-11s %-64s %s\n",
                 "AGENT_ID", "PROVENANCE", "PUBKEY", "CREATED_AT");
        size_t failed = 0;
        for (size_t i = 0; i < count; i++) {
          /* Render from non-secret metadata so a row still shows even when its
           * custody key cannot be decrypted (wrong/rotated DEK, corrupt blob). */
          SignetAgentMeta meta;
          memset(&meta, 0, sizeof(meta));
          if (signet_store_get_agent_meta(store, ids[i], &meta) != 0) {
            printf("%-24s (error)\n", ids[i]);
            failed++;
            continue;
          }
          if (verify_agents) {
            /* --verify: attempt to decrypt the custody key and confirm it
             * derives the stored pubkey. */
            const char *status = "ok";
            SignetAgentRecord rec;
            memset(&rec, 0, sizeof(rec));
            int grc = signet_store_get_agent(store, ids[i], &rec);
            if (grc != 0 || !rec.secret_key || rec.secret_key_len != 32) {
              status = "DECRYPT_FAIL";
              failed++;
              signet_agent_record_clear(&rec);
            } else {
              char skx[65];
              for (size_t b = 0; b < rec.secret_key_len && b < 32; b++)
                snprintf(skx + b * 2, 3, "%02x", rec.secret_key[b]);
              skx[64] = '\0';
              char *dpk = nostr_key_get_public(skx);
              sodium_memzero(skx, sizeof(skx));
              if (!dpk) {
                status = "BAD_KEY";
                failed++;
              } else if (meta.pubkey && g_ascii_strcasecmp(dpk, meta.pubkey) != 0) {
                status = "MISMATCH";
                failed++;
              }
              free(dpk);
              signet_agent_record_clear(&rec);
            }
            printf("%-24s %-11s %-64s %-13s %" G_GINT64_FORMAT "\n",
                   ids[i],
                   meta.provenance ? meta.provenance : "provisioned",
                   meta.pubkey ? meta.pubkey : "(unknown)",
                   status, meta.created_at);
          } else {
            printf("%-24s %-11s %-64s %" G_GINT64_FORMAT "\n",
                   ids[i],
                   meta.provenance ? meta.provenance : "provisioned",
                   meta.pubkey ? meta.pubkey : "(unknown)",
                   meta.created_at);
          }
          signet_agent_meta_clear(&meta);
        }
        if (verify_agents)
          printf("\nTotal: %zu agents, %zu failed verification\n", count, failed);
        else
          printf("\nTotal: %zu agents\n", count);
        if (failed) local_rc = 1;
        signet_store_free_agent_ids(ids, count);
      } else {
        local_rc = 1;
        fprintf(stderr, "signetctl: failed to list agents\n");
      }
    } else if (strcmp(cmd, "list-leases") == 0) {
      int64_t now = (int64_t)time(NULL);
      /* List all active leases (pass NULL agent_id for all). */
      SignetLeaseRecord *leases = NULL;
      size_t count = 0;
      if (signet_store_list_active_leases(store, NULL, now, &leases, &count) == 0) {
        printf("%-16s %-16s %-16s %-12s %-12s\n",
               "LEASE_ID", "AGENT_ID", "SECRET_ID", "ISSUED_AT", "EXPIRES_AT");
        for (size_t i = 0; i < count; i++) {
          printf("%-16s %-16s %-16s %-12" G_GINT64_FORMAT " %-12" G_GINT64_FORMAT "\n",
                 leases[i].lease_id ? leases[i].lease_id : "",
                 leases[i].agent_id ? leases[i].agent_id : "",
                 leases[i].secret_id ? leases[i].secret_id : "",
                 leases[i].issued_at,
                 leases[i].expires_at);
        }
        printf("\nTotal: %zu active leases\n", count);
        signet_lease_list_free(leases, count);
      } else {
        local_rc = 1;
        fprintf(stderr, "signetctl: failed to list leases\n");
      }
    } else if (strcmp(cmd, "list-sessions") == 0) {
      /* Sessions are tracked as leases with secret_id='session'. */
      int64_t now = (int64_t)time(NULL);
      SignetLeaseRecord *leases = NULL;
      size_t count = 0;
      if (signet_store_list_active_leases(store, NULL, now, &leases, &count) == 0) {
        printf("%-16s %-16s %-12s %-12s\n",
               "SESSION_ID", "AGENT_ID", "ISSUED_AT", "EXPIRES_AT");
        size_t session_count = 0;
        for (size_t i = 0; i < count; i++) {
          if (leases[i].secret_id && strcmp(leases[i].secret_id, "session") == 0) {
            printf("%-16s %-16s %-12" G_GINT64_FORMAT " %-12" G_GINT64_FORMAT "\n",
                   leases[i].lease_id ? leases[i].lease_id : "",
                   leases[i].agent_id ? leases[i].agent_id : "",
                   leases[i].issued_at,
                   leases[i].expires_at);
            session_count++;
          }
        }
        printf("\nTotal: %zu active sessions\n", session_count);
        signet_lease_list_free(leases, count);
      } else {
        local_rc = 1;
        fprintf(stderr, "signetctl: failed to list sessions\n");
      }
    } else if (strcmp(cmd, "verify-audit") == 0) {
      int64_t broken_id = 0;
      int vrc = signet_audit_verify_chain(store, 1, 0, &broken_id);
      if (vrc == 0) {
        int64_t total = signet_audit_log_count(store);
        printf("Audit log integrity: OK\n");
        printf("Total entries: %" G_GINT64_FORMAT "\n", total);
      } else if (vrc == 1) {
        local_rc = 1;
        fprintf(stderr, "Audit log integrity: BROKEN at entry %" G_GINT64_FORMAT "\n", broken_id);
      } else {
        local_rc = 1;
        fprintf(stderr, "signetctl: failed to verify audit chain\n");
      }
    }

    signet_store_close(store);
    signet_config_clear(&lcfg);
    return local_rc;
  }

  /* Validate config before reading any command secrets. */
  SignetConfig cfg;
  if (signet_config_load(config_path, &cfg) != 0) {
    fprintf(stderr, "signetctl: failed to load config\n");
    return 1;
  }
  if (cfg.n_relays == 0 || !cfg.remote_signer_pubkey_hex[0]) {
    fprintf(stderr, "signetctl: relay list and bunker pubkey are required\n");
    signet_config_clear(&cfg);
    return 1;
  }

  if (op == SIGNET_MGMT_OP_CREATE_CREDENTIAL ||
      op == SIGNET_MGMT_OP_IMPORT_CREDENTIAL ||
      op == SIGNET_MGMT_OP_ROTATE_CREDENTIAL) {
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    if (signetctl_read_credential_payload(credential_input_file,
                                          credential_from_stdin,
                                          &payload, &payload_len) != 0) {
      signet_config_clear(&cfg);
      return 1;
    }
    credential_payload_b64 = g_base64_encode(payload, payload_len);
    sodium_free(payload);
    if (!credential_payload_b64) {
      fprintf(stderr, "signetctl: failed to encode credential payload\n");
      signet_config_clear(&cfg);
      return 1;
    }
  }

  /* Resolve provisioner secret key. */
  char provisioner_sk_hex[65];
  if (!signetctl_resolve_provisioner_key(provisioner_sk_hex, sizeof(provisioner_sk_hex))) {
    fprintf(stderr, "signetctl: SIGNET_PROVISIONER_NSEC_FILE not set or invalid\n");
    secure_wipe(provisioner_sk_hex, sizeof(provisioner_sk_hex));
    signet_config_clear(&cfg);
    signetctl_wipe_free_string(credential_payload_b64);
    return 1;
  }

  char *agent_secret = NULL;
  if (adopt_sec || restore_store_path) {
    agent_secret = signetctl_load_agent_secret(restore_store_path, agent_id);
    if (!agent_secret) {
      fprintf(stderr, "signetctl: cannot load valid 32-byte agent secret\n");
      signet_config_clear(&cfg);
      secure_wipe(provisioner_sk_hex, sizeof(provisioner_sk_hex));
      signetctl_wipe_free_string(credential_payload_b64);
      return 1;
    }
    adopt_sec = agent_secret;
  }

  /* Generate a request ID for correlation. */
  char request_id[17];
  uint64_t rid = (uint64_t)g_get_real_time();
  snprintf(request_id, sizeof(request_id), "%016" G_GINT64_MODIFIER "x", rid);

  const char *policy_json = NULL;
  if (op == SIGNET_MGMT_OP_SET_POLICY) {
    policy_json = argv[argi++];
  }

  /* Build the Cascadia ContextVM intent (JSON-RPC 2.0, kind-25910 payload). */
  char *intent = signetctl_build_intent(op, agent_id, request_id, policy_json,
                                        deliver_bootstrap_pubkey, delivery_ttl,
                                        adopt_sec, adopt_expected_pubkey,
                                        revoke_client_pubkey, provisioner_pubkey,
                                        credential_id,
                                        credential_type, credential_label,
                                        credential_payload_b64,
                                        credential_policy_id,
                                        has_credential_expires_at,
                                        credential_expires_at,
                                        writer_pubkey);
  sodium_free(agent_secret);
  signetctl_wipe_free_string(credential_payload_b64);
  credential_payload_b64 = NULL;
  if (!intent) {
    fprintf(stderr, "signetctl: failed to build intent\n");
    signet_config_clear(&cfg);
    secure_wipe(provisioner_sk_hex, 64);
    return 1;
  }

  /* Gift-wrap (NIP-59/NIP-17, kind 1059) the intent to the bunker pubkey. */
  const char *bunker_pk = cfg.remote_signer_pubkey_hex[0]
                            ? cfg.remote_signer_pubkey_hex
                            : NULL;
  if (!bunker_pk) {
    fprintf(stderr, "signetctl: bunker pubkey not configured (set SIGNET_BUNKER_NSEC_FILE / [nostr] bunker_pubkey)\n");
    sodium_memzero(intent, strlen(intent));
    g_free(intent);
    signet_config_clear(&cfg);
    secure_wipe(provisioner_sk_hex, 64);
    return 1;
  }
  char *event_json = signetctl_build_gift_wrapped_intent(intent, bunker_pk, provisioner_sk_hex);
  sodium_memzero(intent, strlen(intent));
  g_free(intent);

  if (!event_json) {
    fprintf(stderr, "signetctl: failed to build gift-wrapped intent\n");
    signet_config_clear(&cfg);
    secure_wipe(provisioner_sk_hex, 64);
    return 1;
  }

  /* Set up relay pool to publish and listen for ack. */
  SignetctlAckCtx ack_ctx;
  memset(&ack_ctx, 0, sizeof(ack_ctx));
  g_mutex_init(&ack_ctx.mu);
  g_cond_init(&ack_ctx.cond);

  /* Populate correlation fields for ack validation.
   * Must copy SK before wiping it — needed for NIP-44 decrypt of acks. */
  memcpy(ack_ctx.expected_request_id, request_id, sizeof(ack_ctx.expected_request_id));
  if (cfg.remote_signer_pubkey_hex[0])
    memcpy(ack_ctx.expected_sender_hex, cfg.remote_signer_pubkey_hex, 65);
  memcpy(ack_ctx.provisioner_sk_hex, provisioner_sk_hex, 65);
  secure_wipe(provisioner_sk_hex, 64);

  /* signetctl signs AUTH challenges using the provisioner's own key,
   * allowing it to connect to relays that require NIP-42 auth. */
  SignetRelayPoolConfig rp_cfg = {
    .relays = (const char *const *)cfg.relays,
    .n_relays = cfg.n_relays,
    .on_event = signetctl_on_event,
    .user_data = &ack_ctx,
    .auth_sk_hex = ack_ctx.provisioner_sk_hex[0] ? ack_ctx.provisioner_sk_hex : NULL,
    .auth_relay_tag_url = g_getenv("SIGNET_AUTH_RELAY_URL"),
  };
  SignetRelayPool *rp = signet_relay_pool_new(&rp_cfg);

  int exit_code = 1;
  guint deadline_source = 0, interrupt_source = 0, terminate_source = 0;
  ack_ctx.loop = g_main_loop_new(NULL, FALSE);

  if (!rp || signet_relay_pool_start(rp) != 0) {
    fprintf(stderr, "signetctl: failed to start relay pool\n");
    goto cleanup;
  }

  ack_ctx.pool = rp;
  ack_ctx.event_json = event_json;
  ack_ctx.n_relays = cfg.n_relays;
  ack_ctx.closed_relays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  signet_relay_pool_set_subscription_callback(rp, signetctl_subscription, &ack_ctx);
  deadline_source = g_timeout_add_seconds(SIGNETCTL_TIMEOUT_SEC, signetctl_deadline, &ack_ctx);
  interrupt_source = g_unix_signal_add(SIGINT, signetctl_cancel, &ack_ctx);
  terminate_source = g_unix_signal_add(SIGTERM, signetctl_cancel, &ack_ctx);
  g_main_loop_run(ack_ctx.loop);

  if (ack_ctx.received && ack_ctx.response_json) {
    if (ack_ctx.is_error) {
      fprintf(stderr, "signetctl: management request refused\n");
      exit_code = 1;
    } else {
      if (op == SIGNET_MGMT_OP_PROVISION_AGENT ||
          op == SIGNET_MGMT_OP_ADOPT_EXISTING ||
          op == SIGNET_MGMT_OP_RESTORE_EXISTING) {
        signetctl_print_adopt_result(ack_ctx.response_json);
        exit_code = 0;
      } else if (op == SIGNET_MGMT_OP_REISSUE_CONNECT) {
        exit_code = signetctl_handle_reissue_result(ack_ctx.response_json,
                                                    reissue_out_path,
                                                    reissue_show_secret);
      } else if (op == SIGNET_MGMT_OP_DELIVER_CREDENTIAL) {
        exit_code = signet_cli_handle_delivery_result(ack_ctx.response_json,
                                                      credential_output_file);
      } else {
        printf("Reply received:\n%s\n", ack_ctx.response_json);
        exit_code = 0;
      }
    }
  } else {
    fprintf(stderr, "signetctl: %s\n", ack_ctx.failure ? ack_ctx.failure : "operation failed");
  }

cleanup:
  if (deadline_source) g_source_remove(deadline_source);
  if (interrupt_source) g_source_remove(interrupt_source);
  if (terminate_source) g_source_remove(terminate_source);
  if (rp) {
    signet_relay_pool_stop(rp);
    signet_relay_pool_free(rp);
  }
  signetctl_wipe_free_string(event_json);
  g_free(ack_ctx.selected_relay);
  if (ack_ctx.closed_relays) g_hash_table_destroy(ack_ctx.closed_relays);
  g_main_loop_unref(ack_ctx.loop);
  /* The reply may embed a fresh connect_secret (reissue-connect) or bunker
   * URI (provision/adopt) — wipe before free. */
  if (ack_ctx.response_json) secure_wipe(ack_ctx.response_json, strlen(ack_ctx.response_json));
  g_free(ack_ctx.response_json);
  secure_wipe(ack_ctx.provisioner_sk_hex, sizeof(ack_ctx.provisioner_sk_hex));
  g_mutex_clear(&ack_ctx.mu);
  g_cond_clear(&ack_ctx.cond);
  signet_config_clear(&cfg);
  return exit_code;
}
