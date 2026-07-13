/* SPDX-License-Identifier: MIT
 *
 * nip46_server.c - NIP-46 request handling using libnostr/nips library calls.
 *
 * This module is an orchestration layer:
 * - Replay protection via SignetReplayCache
 * - NIP-44 v2 encrypt/decrypt for outer transport layer
 * - Policy gating via SignetPolicyEngine
 * - Event signing via libnostr NostrEvent + nostr_event_sign()
 * - NIP-46 message parse/build via nips/nip46 library
 * - Audit logging (JSONL) without secret material
 *
 * All crypto primitives delegate to monorepo libraries — no hand-rolled
 * BIP340, ECDH, AES, or event serialization in this file.
 */

#include "signet/nip46_server.h"
#include "signet/cascadia.h"
#include "signet/health_server.h"  /* g_signet_metrics */

#include "signet/relay_pool.h"
#include "signet/policy_engine.h"
#include "signet/key_store.h"
#include "signet/store.h"      /* persistent NIP-46 client bindings */
#include "signet/revocation.h" /* live deny list (suspension precedence) */
#include "signet/replay_cache.h"
#include "signet/audit_logger.h"
#include "signet/util.h"
#ifdef SIGNET_ENABLE_PASSKEYS
#include "signet/fido.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <json-glib/json-glib.h>

/* libnostr */
#include <nostr-event.h>
#include <nostr-keys.h>
#include <secure_buf.h>

/* NIP modules */
#include <nostr/nip04.h>
#include <nostr/nip44/nip44.h>
#include <nostr/nip46/nip46_msg.h>
#include <nostr/nip46/nip46_types.h>

#define SIGNET_NIP46_KIND_CIPHERTEXT NOSTR_EVENT_KIND_NIP46

/* ------------------------------ small helpers ----------------------------- */

static void signet_memzero(void *p, size_t n) {
  if (p && n) secure_wipe(p, n);
}

static char *signet_json_quote_string(const char *s) {
  if (!s) return NULL;

  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;

  json_builder_begin_array(b);
  json_builder_add_string_value(b, s);
  json_builder_end_array(b);

  JsonGenerator *g = json_generator_new();
  if (!g) {
    g_object_unref(b);
    return NULL;
  }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  json_generator_set_pretty(g, FALSE);
  char *array_json = json_generator_to_data(g, NULL);

  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);

  if (!array_json) return NULL;

  size_t len = strlen(array_json);
  if (len < 4) {
    g_free(array_json);
    return NULL;
  }

  char *quoted = g_strndup(array_json + 1, len - 2);
  g_free(array_json);
  return quoted;
}

static void signet_bytes32_to_hex(const uint8_t in[32], char out_hex[65]) {
  for (int i = 0; i < 32; i++) {
    sprintf(out_hex + (i * 2), "%02x", in[i]);
  }
  out_hex[64] = '\0';
}

/* ------------------------------ audit helper ------------------------------ */

static void signet_audit_nip46(SignetAuditLogger *audit,
                               int64_t now,
                               const char *identity,
                               const char *client_pubkey_hex,
                               const char *outer_event_id_hex,
                               const char *request_id,
                               const char *method,
                               int event_kind,
                               const char *decision,
                               const char *reason_code,
                               const char *status,
                               const char *code) {
  if (!audit) return;

  JsonBuilder *b = json_builder_new();
  if (!b) return;

  json_builder_begin_object(b);

  json_builder_set_member_name(b, "ts");
  json_builder_add_int_value(b, (gint64)now);

  json_builder_set_member_name(b, "component");
  json_builder_add_string_value(b, "nip46");

  if (identity) {
    json_builder_set_member_name(b, "identity");
    json_builder_add_string_value(b, identity);
  }
  if (client_pubkey_hex && client_pubkey_hex[0]) {
    json_builder_set_member_name(b, "client_pubkey");
    json_builder_add_string_value(b, client_pubkey_hex);
  }
  if (outer_event_id_hex && outer_event_id_hex[0]) {
    json_builder_set_member_name(b, "outer_event_id");
    json_builder_add_string_value(b, outer_event_id_hex);
  }
  if (request_id && request_id[0]) {
    json_builder_set_member_name(b, "request_id");
    json_builder_add_string_value(b, request_id);
  }
  if (method) {
    json_builder_set_member_name(b, "method");
    json_builder_add_string_value(b, method);
  }
  if (event_kind >= 0) {
    json_builder_set_member_name(b, "event_kind");
    json_builder_add_int_value(b, (gint64)event_kind);
  }
  if (decision) {
    json_builder_set_member_name(b, "decision");
    json_builder_add_string_value(b, decision);
  }
  if (reason_code) {
    json_builder_set_member_name(b, "reason_code");
    json_builder_add_string_value(b, reason_code);
  }
  if (status) {
    json_builder_set_member_name(b, "status");
    json_builder_add_string_value(b, status);
  }
  if (code) {
    json_builder_set_member_name(b, "code");
    json_builder_add_string_value(b, code);
  }

  json_builder_end_object(b);

  JsonGenerator *g = json_generator_new();
  if (!g) { g_object_unref(b); return; }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  json_generator_set_pretty(g, FALSE);
  char *json = json_generator_to_data(g, NULL);

  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);

  if (json) {
    (void)signet_audit_log_json(audit, SIGNET_AUDIT_EVENT_SIGN_REQUEST, json);
    g_free(json);
  }
}

static void signet_nip46_publish_cas_audit(SignetRelayPool *relays,
                                           const char *signer_sk_hex,
                                           const char *agent_id,
                                           int64_t now) {
  if (!relays || !signer_sk_hex) return;
  NostrEvent *evt = nostr_event_new();
  if (!evt) return;
  nostr_event_set_kind(evt, CAS_AUDIT);
  nostr_event_set_created_at(evt, now);
  char *content = g_strdup_printf("{\"domain\":\"signet\",\"type\":\"sign\",\"agent\":\"%s\",\"status\":\"ok\"}", agent_id ? agent_id : "");
  nostr_event_set_content(evt, content ? content : "{}");
  g_free(content);
  NostrTags *tags = nostr_tags_new(0);
  if (tags) {
    NostrTag *domain = nostr_tag_new("domain", "signet", NULL);
    NostrTag *type = nostr_tag_new("type", "sign", NULL);
    if (domain) nostr_tags_append(tags, domain);
    if (type) nostr_tags_append(tags, type);
    if (agent_id) { NostrTag *agent = nostr_tag_new("agent", agent_id, NULL); if (agent) nostr_tags_append(tags, agent); }
    nostr_event_set_tags(evt, tags);
  }
  if (nostr_event_sign(evt, signer_sk_hex) == 0) {
    char *json = nostr_event_serialize_compact(evt);
    if (json) { (void)signet_relay_pool_publish_event_json(relays, json); free(json); }
  }
  nostr_event_free(evt);
}

/* -------------------- extract event kind from JSON ----------------------- */

static bool signet_json_event_extract_kind(const char *event_json, int *out_kind) {
  if (out_kind) *out_kind = -1;
  if (!event_json || !out_kind) return false;

  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, event_json, -1, NULL)) return false;

  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return false;

  JsonObject *o = json_node_get_object(root);
  if (!o || !json_object_has_member(o, "kind")) return false;

  *out_kind = (int)json_object_get_int_member(o, "kind");
  return true;
}

/* -------------------- sign event using libnostr NostrEvent --------------- */

static char *signet_sign_event_json_with_seckey(const char *seckey_hex,
                                                const char *event_json,
                                                char **out_err) {
  if (out_err) *out_err = NULL;
  if (!seckey_hex || !event_json) {
    if (out_err) *out_err = g_strdup("internal error");
    return NULL;
  }

  /* Parse the unsigned event JSON */
  NostrEvent *evt = nostr_event_new();
  if (!evt) {
    if (out_err) *out_err = g_strdup("OOM");
    return NULL;
  }

  /* Deserialize from the JSON provided by the client */
  if (!nostr_event_deserialize_compact(evt, event_json, NULL)) {
    if (out_err) *out_err = g_strdup("invalid event JSON");
    nostr_event_free(evt);
    return NULL;
  }

  /* Sign the event — this computes id, sets pubkey from sk, and produces sig */
  int sign_rc = nostr_event_sign(evt, seckey_hex);
  if (sign_rc != 0) {
    if (out_err) *out_err = g_strdup("event signing failed");
    nostr_event_free(evt);
    return NULL;
  }

  /* Serialize the signed event back to JSON */
  char *signed_json = nostr_event_serialize_compact(evt);
  nostr_event_free(evt);

  if (!signed_json) {
    if (out_err) *out_err = g_strdup("failed to serialize signed event");
    return NULL;
  }

  return signed_json; /* caller frees with free() */
}

/* ------------- build + sign outer response event using NostrEvent --------- */

static char *signet_build_outer_response_event_json(const char *remote_signer_seckey_hex,
                                                    const char *client_pubkey_hex,
                                                    const char *request_outer_event_id_hex,
                                                    int64_t now,
                                                    const char *encrypted_content,
                                                    char **out_err) {
  if (out_err) *out_err = NULL;
  if (!remote_signer_seckey_hex || !client_pubkey_hex || !encrypted_content) {
    if (out_err) *out_err = g_strdup("internal error");
    return NULL;
  }

  NostrEvent *evt = nostr_event_new();
  if (!evt) {
    if (out_err) *out_err = g_strdup("OOM");
    return NULL;
  }

  nostr_event_set_kind(evt, SIGNET_NIP46_KIND_CIPHERTEXT);
  nostr_event_set_created_at(evt, now);
  nostr_event_set_content(evt, encrypted_content);

  /* Add tags: ["p", client_pubkey] and optionally ["e", request_event_id] */
  NostrTags *tags = nostr_tags_new(0);
  if (tags) {
    NostrTag *p_tag = nostr_tag_new("p", client_pubkey_hex, NULL);
    if (p_tag) {
      nostr_tags_append(tags, p_tag);
    }

    if (request_outer_event_id_hex && request_outer_event_id_hex[0]) {
      NostrTag *e_tag = nostr_tag_new("e", request_outer_event_id_hex, NULL);
      if (e_tag) {
        nostr_tags_append(tags, e_tag);
      }
    }
    nostr_event_set_tags(evt, tags);
  }

  /* Sign — this computes id, derives pubkey, and produces sig */
  int rc = nostr_event_sign(evt, remote_signer_seckey_hex);
  if (rc != 0) {
    if (out_err) *out_err = g_strdup("failed to sign response event");
    nostr_event_free(evt);
    return NULL;
  }

  char *json = nostr_event_serialize_compact(evt);
  nostr_event_free(evt);

  if (!json) {
    if (out_err) *out_err = g_strdup("failed to serialize response event");
    return NULL;
  }

  return json; /* caller frees with free() */
}

/* ------------------------------ server object ----------------------------- */

struct SignetNip46Server {
  SignetRelayPool *relays;
  SignetPolicyEngine *policy;
  SignetKeyStore *keys;
  SignetReplayCache *replay;
  SignetAuditLogger *audit;
  struct SignetFidoService *fido;

  char *identity;
  GHashTable *sessions_by_client_pubkey; /* client ephemeral pubkey -> agent_id */
  SignetDenyList *deny;                  /* live deny list (owned by daemon) */

  GMutex mu;
};

void signet_nip46_server_set_deny_list(SignetNip46Server *s,
                                       struct SignetDenyList *deny) {
  if (!s) return;
  s->deny = deny;
}

SignetNip46Server *signet_nip46_server_new(SignetRelayPool *relays,
                                           SignetPolicyEngine *policy,
                                           SignetKeyStore *keys,
                                           SignetReplayCache *replay,
                                           SignetAuditLogger *audit,
                                           const SignetNip46ServerConfig *cfg) {
  if (!cfg || !cfg->identity) return NULL;

  SignetNip46Server *s = (SignetNip46Server *)calloc(1, sizeof(*s));
  if (!s) return NULL;

  g_mutex_init(&s->mu);

  s->relays = relays;
  s->policy = policy;
  s->keys = keys;
  s->replay = replay;
  s->audit = audit;
  s->fido = cfg->fido;

  s->identity = g_strdup(cfg->identity);
  s->sessions_by_client_pubkey = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  if (!s->identity || !s->sessions_by_client_pubkey) {
    if (s->sessions_by_client_pubkey) g_hash_table_destroy(s->sessions_by_client_pubkey);
    g_free(s->identity);
    g_mutex_clear(&s->mu);
    free(s);
    return NULL;
  }

  return s;
}

void signet_nip46_server_free(SignetNip46Server *s) {
  if (!s) return;
  if (s->sessions_by_client_pubkey) {
    g_hash_table_destroy(s->sessions_by_client_pubkey);
  }
  g_mutex_clear(&s->mu);
  g_free(s->identity);
  free(s);
}

/* ----------------------------- handle_event ------------------------------- */

bool signet_nip46_server_handle_event(SignetNip46Server *s,
                                      const char *remote_signer_pubkey_hex,
                                      const char *remote_signer_secret_key_hex,
                                      const char *client_pubkey_hex,
                                      const char *ciphertext,
                                      int64_t created_at,
                                      const char *event_id_hex,
                                      int64_t now) {
  if (!s) return false;
  if (!remote_signer_pubkey_hex || !remote_signer_secret_key_hex ||
      !client_pubkey_hex || !ciphertext) return false;

  /* 1) Replay check (by outer event id). The replay cache has its own lock;
   * do not hold the NIP-46 server session mutex across crypto, policy, or I/O. */
  SignetReplayResult rr = SIGNET_REPLAY_OK;
  if (s->replay && event_id_hex && event_id_hex[0]) {
    rr = signet_replay_check_and_mark(s->replay, event_id_hex, created_at, now);
  }

  /* 2) Decrypt request content (NIP-44 v2) */
  char *plain = NULL;
  char *dec_err = NULL;
  bool dec_ok = false;
  {
    uint8_t sk[32], pk[32];
    if (signet_hex_to_bytes32(remote_signer_secret_key_hex, sk) &&
        signet_hex_to_bytes32(client_pubkey_hex, pk)) {
      uint8_t *pt = NULL;
      size_t pt_len = 0;
      int rc = nostr_nip44_decrypt_v2(sk, pk, ciphertext, &pt, &pt_len);
      signet_memzero(sk, 32);
      if (rc == 0 && pt && pt_len > 0) {
        /* NIP-44 returns raw bytes; NUL-terminate for JSON parsing. */
        plain = (char *)malloc(pt_len + 1);
        if (plain) {
          memcpy(plain, pt, pt_len);
          plain[pt_len] = '\0';
          dec_ok = true;
        }
        free(pt);
      }
    } else {
      dec_err = g_strdup("invalid key hex");
    }
  }

  /* 3) Parse NIP-46 request using library */
  NostrNip46Request req;
  memset(&req, 0, sizeof(req));
  bool parse_ok = false;
  if (dec_ok) {
    parse_ok = (nostr_nip46_request_parse(plain, &req) == 0);
  }

  const char *method = parse_ok ? req.method : NULL;
  const char *req_id = parse_ok ? req.id : NULL;

  /* Determine event_kind for policy checks (only meaningful for sign_event) */
  int event_kind = -1;
  if (parse_ok && method && strcmp(method, "sign_event") == 0 &&
      req.params && req.n_params >= 1) {
    (void)signet_json_event_extract_kind(req.params[0], &event_kind);
  }

  char *session_agent_id = NULL;
  const char *policy_identity = s->identity;
  const char *pre_code = NULL;
  char *pre_err = NULL;

  if (parse_ok && method) {
    if (strcmp(method, "connect") == 0) {
      const char *requested_signer = (req.params && req.n_params >= 1) ? req.params[0] : NULL;
      const char *provided_secret = (req.params && req.n_params >= 2) ? req.params[1] : NULL;

      if (!requested_signer || !requested_signer[0]) {
        pre_code = "invalid_params";
        pre_err = g_strdup("connect requires [remote_signer_pubkey, connect_secret?]");
      } else if (g_ascii_strcasecmp(requested_signer, remote_signer_pubkey_hex) != 0) {
        pre_code = "wrong_signer";
        pre_err = g_strdup("connect target does not match bunker pubkey");
      } else {
        /* PAIRING path: a provided one-time secret authorizes binding (or
         * re-binding) this client key to its agent. Consumption and the
         * durable binding commit in ONE store transaction — a crash or
         * binding failure can never burn the credential without recording
         * the pairing. */
        bool secret_provided = (provided_secret && provided_secret[0]);
        SignetStore *st = signet_key_store_get_store(s->keys);
        if (secret_provided && st) {
          int rc = signet_store_consume_connect_secret_and_bind(
              st, provided_secret, client_pubkey_hex, now, &session_agent_id);
          if (rc != 0) { g_free(session_agent_id); session_agent_id = NULL; }
        } else if (secret_provided) {
          /* Cache-only mode: no persistent secrets/bindings exist. */
          int rc = signet_key_store_consume_connect_secret(s->keys, provided_secret, now, &session_agent_id);
          if (rc != 0) { g_free(session_agent_id); session_agent_id = NULL; }
        }

        /* RECONNECT path: falls back to the PERSISTENT binding created at
         * pairing time. The sender proved possession of the bound client key
         * by NIP-44-encrypting this request, so no new trust is granted.
         * A non-empty secret that failed above is accepted ONLY when it is
         * the client's OWN former pairing secret (hash-pinned at pairing) —
         * the stale value a restarted client re-sends. An arbitrary wrong
         * secret stays auth_failed so misconfiguration/probing is surfaced
         * rather than silently ignored. */
        if (!session_agent_id) {
          char *bound = NULL;
          char *bound_hash = NULL;
          if (st && signet_store_lookup_client_binding(st, client_pubkey_hex, now,
                                                       &bound, &bound_hash) == 0 && bound) {
            bool stale_ok = true;
            if (secret_provided) {
              char *provided_hash =
                  g_compute_checksum_for_string(G_CHECKSUM_SHA256, provided_secret, -1);
              stale_ok = (bound_hash && provided_hash &&
                          g_strcmp0(bound_hash, provided_hash) == 0);
              g_free(provided_hash);
            }
            if (stale_ok) {
              session_agent_id = bound;
              bound = NULL;
            }
          }
          g_free(bound);
          g_free(bound_hash);

          if (!session_agent_id) {
            pre_code = "auth_failed";
            pre_err = secret_provided
                          ? g_strdup("connect_secret mismatch")
                          : g_strdup("connect requires a connect_secret or an existing client binding");
          }
        }

        if (session_agent_id) policy_identity = session_agent_id;
      }
    } else {
      /* Resolve the requesting client to its agent. The persistent binding
       * table is authoritative when a store is present (it survives daemon
       * restarts and honors revocation immediately); the in-memory session
       * table only serves cache-only deployments. */
      SignetStore *st = signet_key_store_get_store(s->keys);
      if (st) {
        char *bound = NULL;
        int brc = signet_store_lookup_client_binding(st, client_pubkey_hex, now, &bound, NULL);
        if (brc == 0 && bound) {
          session_agent_id = bound;
        } else {
          g_free(bound);
          pre_code = "not_connected";
          pre_err = g_strdup("client has no active NIP-46 session");
        }
      } else {
        bool bound_found = false;
        if (s->sessions_by_client_pubkey) {
          g_mutex_lock(&s->mu);
          const char *bound_agent = (const char *)g_hash_table_lookup(
              s->sessions_by_client_pubkey, client_pubkey_hex);
          if (bound_agent && bound_agent[0]) {
            bound_found = true;
            session_agent_id = g_strdup(bound_agent);
          }
          g_mutex_unlock(&s->mu);
        }

        if (!bound_found) {
          pre_code = "not_connected";
          pre_err = g_strdup("client has no active NIP-46 session");
        } else if (!session_agent_id) {
          pre_code = "oom";
          pre_err = g_strdup("out of memory");
        }
      }

      if (session_agent_id) policy_identity = session_agent_id;
    }

    /* Suspension precedence: a deny-listed agent identity is refused BEFORE
     * policy evaluation, on every path (pairing, reconnect, requests). The
     * policy engine does not consult the deny list, so without this check a
     * suspended-but-not-revoked agent with a persistent binding could keep
     * using its custody key. */
    if (!pre_err && session_agent_id && s->deny) {
      char agent_pk[65] = {0};
      if (signet_key_store_get_agent_pubkey(s->keys, session_agent_id,
                                            agent_pk, sizeof(agent_pk)) &&
          signet_deny_list_contains(s->deny, agent_pk)) {
        pre_code = "deny_listed";
        pre_err = g_strdup("agent identity is deny-listed");
      }
    }
  }

  /* 4) Policy check */
  SignetPolicyResult pres;
  memset(&pres, 0, sizeof(pres));
  pres.decision = SIGNET_POLICY_DECISION_DENY;
  pres.reason_code = "policy_engine_missing";

  bool policy_ok = false;
  if (!pre_err && s->policy && parse_ok && method) {
    policy_ok = signet_policy_engine_eval(s->policy,
                                          policy_identity,
                                          client_pubkey_hex,
                                          method,
                                          event_kind,
                                          now,
                                          &pres);
    if (!policy_ok) {
      pres.decision = SIGNET_POLICY_DECISION_DENY;
      pres.reason_code = "policy_eval_error";
    }
  } else if (!pre_err && parse_ok && method) {
    policy_ok = true;
    pres.decision = SIGNET_POLICY_DECISION_DENY;
    pres.reason_code = "policy_engine_missing";
  }

  /* 5) Decide early errors (replay, decrypt, parse, session, deny) */
  const char *decision = "deny";
  const char *status = "error";
  const char *code = "internal_error";

  char *result = NULL;
  bool result_is_json = false;
  char *err_str = NULL;
  bool allow = false;

  if (rr == SIGNET_REPLAY_DUPLICATE) {
    code = "replay_duplicate";
    err_str = g_strdup("replay rejected (duplicate)");
  } else if (rr == SIGNET_REPLAY_TOO_OLD) {
    code = "replay_too_old";
    err_str = g_strdup("replay rejected (too old)");
  } else if (rr == SIGNET_REPLAY_TOO_FAR_IN_FUTURE) {
    code = "replay_in_future";
    err_str = g_strdup("replay rejected (in future)");
  } else if (rr == SIGNET_REPLAY_INVALID) {
    code = "replay_invalid";
    err_str = g_strdup("replay rejected (malformed event id/timestamp)");
  } else if (!dec_ok) {
    code = "decrypt_failed";
    err_str = g_strdup("decrypt failed");
  } else if (!parse_ok) {
    code = "invalid_request";
    err_str = g_strdup("invalid request");
  } else if (pre_err) {
    code = pre_code ? pre_code : "session_error";
    err_str = pre_err;
    pre_err = NULL;
  } else if (!policy_ok) {
    code = "policy_error";
    err_str = g_strdup("policy evaluation error");
  } else if (pres.decision != SIGNET_POLICY_DECISION_ALLOW) {
    code = "policy_denied";
    err_str = g_strdup(pres.reason_code ? pres.reason_code : "denied");
  } else {
    allow = true;
  }

  if (allow) {
    decision = "allow";

    /* 6) Execute method */
    if (strcmp(method, "connect") == 0) {
      char *session_key = g_strdup(client_pubkey_hex);
      char *session_value = g_strdup(session_agent_id);
      if (!session_key || !session_value) {
        g_free(session_key);
        g_free(session_value);
        err_str = g_strdup("out of memory");
        status = "error";
        code = "oom";
      } else {
        g_mutex_lock(&s->mu);
        g_hash_table_replace(s->sessions_by_client_pubkey, session_key, session_value);
        g_mutex_unlock(&s->mu);

        /* The persistent pairing was already committed ATOMICALLY with the
         * secret consumption (or resolved from an existing binding); only
         * the cache-only RAM session needed recording here. */

        result = g_strdup("ack");
        result_is_json = false;
        status = "ok";
        code = "ok";
      }

    } else if (strcmp(method, "ping") == 0) {
      result = g_strdup("pong");
      result_is_json = false;
      status = "ok";
      code = "ok";

    } else if (strcmp(method, "get_public_key") == 0) {
      char agent_pubkey_hex[65];
      if (!signet_key_store_get_agent_pubkey(s->keys, session_agent_id,
                                             agent_pubkey_hex, sizeof(agent_pubkey_hex))) {
        err_str = g_strdup("failed to load agent pubkey");
        status = "error";
        code = "invalid_key";
      } else {
        result = g_strdup(agent_pubkey_hex);
        result_is_json = false;
        status = "ok";
        code = "ok";
      }

    } else if (strcmp(method, "sign_event") == 0 ||
               strcmp(method, "nip04_encrypt") == 0 ||
               strcmp(method, "nip04_decrypt") == 0 ||
               strcmp(method, "nip44_encrypt") == 0 ||
               strcmp(method, "nip44_decrypt") == 0) {
      SignetLoadedKey agent_key;
      memset(&agent_key, 0, sizeof(agent_key));
      if (!signet_key_store_load_agent_key(s->keys, session_agent_id, &agent_key) ||
          !agent_key.secret_key || agent_key.secret_key_len != 32) {
        err_str = g_strdup("failed to load agent key");
        status = "error";
        code = "invalid_key";
      } else {
        char agent_sk_hex[65];
        signet_bytes32_to_hex(agent_key.secret_key, agent_sk_hex);

        if (strcmp(method, "sign_event") == 0) {
          if (!req.params || req.n_params < 1) {
            err_str = g_strdup("sign_event requires event JSON param");
            status = "error";
            code = "invalid_params";
          } else {
            char *serr = NULL;
            char *signed_evt = signet_sign_event_json_with_seckey(
                agent_sk_hex, req.params[0], &serr);
            if (!signed_evt) {
              err_str = serr ? serr : g_strdup("sign_event failed");
              status = "error";
              code = "sign_failed";
            } else {
              result = signed_evt;
              result_is_json = false;
              status = "ok";
              code = "ok";
              signet_nip46_publish_cas_audit(s->relays, remote_signer_secret_key_hex, session_agent_id, now);
              g_free(serr);
            }
          }
        } else if (strcmp(method, "nip04_encrypt") == 0) {
          if (!req.params || req.n_params < 2) {
            err_str = g_strdup("nip04_encrypt requires [pubkey, plaintext]");
            status = "error";
            code = "invalid_params";
          } else {
            char *ct = NULL;
            char *eerr = NULL;
            int rc = nostr_nip04_encrypt(req.params[1], req.params[0],
                                         agent_sk_hex,
                                         &ct, &eerr);
            if (rc != 0) {
              err_str = eerr ? eerr : g_strdup("encrypt failed");
              status = "error";
              code = "encrypt_failed";
            } else {
              result = ct;
              result_is_json = false;
              status = "ok";
              code = "ok";
              free(eerr);
            }
          }
        } else if (strcmp(method, "nip04_decrypt") == 0) {
          if (!req.params || req.n_params < 2) {
            err_str = g_strdup("nip04_decrypt requires [pubkey, ciphertext]");
            status = "error";
            code = "invalid_params";
          } else {
            char *pt = NULL;
            char *derr = NULL;
            int rc = nostr_nip04_decrypt(req.params[1], req.params[0],
                                         agent_sk_hex,
                                         &pt, &derr);
            if (rc != 0) {
              err_str = derr ? derr : g_strdup("decrypt failed");
              status = "error";
              code = "decrypt_failed";
            } else {
              result = pt;
              result_is_json = false;
              status = "ok";
              code = "ok";
              free(derr);
            }
          }
        } else if (strcmp(method, "nip44_encrypt") == 0) {
          /* NIP-44 v2 encrypt: params = [peer_pubkey_hex, plaintext]. */
          if (!req.params || req.n_params < 2) {
            err_str = g_strdup("nip44_encrypt requires [pubkey, plaintext]");
            status = "error";
            code = "invalid_params";
          } else {
            uint8_t peer_pk[32];
            if (!signet_hex_to_bytes32(req.params[0], peer_pk)) {
              err_str = g_strdup("invalid peer pubkey");
              status = "error";
              code = "invalid_params";
            } else {
              char *ct = NULL;
              int rc = nostr_nip44_encrypt_v2(agent_key.secret_key, peer_pk,
                                              (const uint8_t *)req.params[1],
                                              strlen(req.params[1]), &ct);
              signet_memzero(peer_pk, sizeof(peer_pk));
              if (rc != 0 || !ct) {
                err_str = g_strdup("encrypt failed");
                status = "error";
                code = "encrypt_failed";
              } else {
                result = g_strdup(ct);
                result_is_json = false;
                free(ct);
                status = "ok";
                code = "ok";
              }
            }
          }
        } else { /* nip44_decrypt: params = [peer_pubkey_hex, ciphertext] */
          if (!req.params || req.n_params < 2) {
            err_str = g_strdup("nip44_decrypt requires [pubkey, ciphertext]");
            status = "error";
            code = "invalid_params";
          } else {
            uint8_t peer_pk[32];
            if (!signet_hex_to_bytes32(req.params[0], peer_pk)) {
              err_str = g_strdup("invalid peer pubkey");
              status = "error";
              code = "invalid_params";
            } else {
              uint8_t *pt = NULL;
              size_t pt_len = 0;
              int rc = nostr_nip44_decrypt_v2(agent_key.secret_key, peer_pk,
                                              req.params[1], &pt, &pt_len);
              signet_memzero(peer_pk, sizeof(peer_pk));
              if (rc != 0 || !pt) {
                err_str = g_strdup("decrypt failed");
                status = "error";
                code = "decrypt_failed";
              } else {
                result = g_strndup((const char *)pt, pt_len);
                result_is_json = false;
                signet_memzero(pt, pt_len);
                free(pt);
                status = "ok";
                code = "ok";
              }
            }
          }
        }

        signet_memzero(agent_sk_hex, sizeof(agent_sk_hex));
        signet_loaded_key_clear(&agent_key);
      }

    } else if (strcmp(method, "webauthn_get_info") == 0 ||
               strcmp(method, "webauthn_make_credential") == 0 ||
               strcmp(method, "webauthn_get_assertion") == 0 ||
               strcmp(method, "webauthn_export") == 0 ||
               strcmp(method, "webauthn_import") == 0) {
#ifdef SIGNET_ENABLE_PASSKEYS
      char *fido_json = NULL;
      SignetFidoError ferr;
      SignetFidoStatus fst = SIGNET_FIDO_ERR_INTERNAL;
      if (strcmp(method, "webauthn_get_info") == 0) {
        fst = signet_fido_get_info_json(s->fido, session_agent_id, &fido_json, &ferr);
      } else {
        if (!req.params || req.n_params < 1) {
          err_str = g_strdup("webauthn method requires JSON payload param");
          status = "error";
          code = "invalid_params";
        } else if (strcmp(method, "webauthn_make_credential") == 0) {
          fst = signet_fido_make_credential_json(s->fido, session_agent_id,
                                                 req.params[0], &fido_json, &ferr);
        } else if (strcmp(method, "webauthn_get_assertion") == 0) {
          fst = signet_fido_get_assertion_json(s->fido, session_agent_id,
                                               req.params[0], &fido_json, &ferr);
        } else if (strcmp(method, "webauthn_export") == 0) {
          fst = signet_fido_export_credential_json(s->fido, session_agent_id,
                                                   req.params[0], &fido_json, &ferr);
        } else {
          fst = signet_fido_import_credential_json(s->fido, session_agent_id,
                                                   req.params[0], &fido_json, &ferr);
        }
      }
      if (!err_str) {
        if (fst == SIGNET_FIDO_OK) {
          result = fido_json;
          fido_json = NULL;
          result_is_json = true;
          status = "ok";
          code = "ok";
        } else {
          err_str = g_strdup(ferr.reason ? ferr.reason : signet_fido_status_string(fst));
          status = "error";
          code = signet_fido_status_string(fst);
        }
      }
      g_free(fido_json);
#else
      err_str = g_strdup("passkeys support not built");
      status = "error";
      code = "not_configured";
#endif

    } else if (strcmp(method, "get_relays") == 0) {
      /* Build a JSON object mapping relay URLs to empty read/write hints. */
      size_t n_urls = 0;
      const char *const *urls = signet_relay_pool_get_urls(s->relays, &n_urls);
      GString *rb = g_string_new("{");
      for (size_t ri = 0; ri < n_urls; ri++) {
        if (ri > 0) g_string_append_c(rb, ',');
        g_string_append_printf(rb, "\"%s\":{\"read\":true,\"write\":true}",
                               urls[ri] ? urls[ri] : "");
      }
      g_string_append_c(rb, '}');
      result = g_string_free(rb, FALSE);
      result_is_json = true;
      status = "ok";
      code = "ok";

    } else {
      err_str = g_strdup("unsupported method");
      status = "error";
      code = "unsupported_method";
    }
  }

  /* Increment metrics counters based on outcome. */
  if (allow) {
    g_atomic_int_inc(&g_signet_metrics.auth_ok);
    if (method && strcmp(method, "sign_event") == 0 && result)
      g_atomic_int_inc(&g_signet_metrics.sign_total);
  } else if (rr != SIGNET_REPLAY_OK) {
    g_atomic_int_inc(&g_signet_metrics.auth_error);
  } else {
    g_atomic_int_inc(&g_signet_metrics.auth_denied);
  }

  /* 7) Audit (never include plaintext/keys) */
  signet_audit_nip46(s->audit, now, policy_identity, client_pubkey_hex,
                     event_id_hex, req_id, method ? method : "unknown",
                     event_kind, decision,
                     pres.reason_code ? pres.reason_code : "n/a",
                     status, code);

  /* 8) Build NIP-46 response JSON using library */
  char *resp_json = NULL;
  if (err_str) {
    resp_json = nostr_nip46_response_build_err(req_id ? req_id : "", err_str);
  } else {
    g_autofree char *result_json = NULL;
    if (result_is_json) {
      result_json = g_strdup(result ? result : "\"\"");
    } else {
      result_json = signet_json_quote_string(result ? result : "");
    }
    resp_json = nostr_nip46_response_build_ok(req_id ? req_id : "",
                                               result_json ? result_json : "\"\"");
  }

  /* 9) Encrypt response (NIP-44 v2) */
  char *enc_resp = NULL;
  char *enc_err = NULL;
  bool enc_ok = false;

  if (resp_json) {
    uint8_t sk[32], pk[32];
    if (signet_hex_to_bytes32(remote_signer_secret_key_hex, sk) &&
        signet_hex_to_bytes32(client_pubkey_hex, pk)) {
      int rc = nostr_nip44_encrypt_v2(sk, pk,
                                      (const uint8_t *)resp_json, strlen(resp_json),
                                      &enc_resp);
      signet_memzero(sk, 32);
      enc_ok = (rc == 0 && enc_resp != NULL);
    }
  }

  /* 10) Build outer response event (signed) and publish */
  bool published = false;
  if (enc_ok && enc_resp) {
    char *outer_err = NULL;
    char *outer_evt = signet_build_outer_response_event_json(
        remote_signer_secret_key_hex, client_pubkey_hex,
        event_id_hex, now, enc_resp, &outer_err);
    if (outer_evt) {
      if (s->relays) {
        /* NPA-02: Use ack-aware publish so relay rejections are logged.
         * The callback logs warnings; published tracks enqueue success. */
        published = (signet_relay_pool_publish_event_json_ack(
                       s->relays, outer_evt, NULL, NULL) == 0);
      }
      free(outer_evt);
    }
    g_free(outer_err);
  }

  /* Cleanup — wipe sensitive material */
  if (plain) { signet_memzero(plain, strlen(plain)); free(plain); }
  if (resp_json) { signet_memzero(resp_json, strlen(resp_json)); free(resp_json); }
  if (enc_resp) { free(enc_resp); }
  free(enc_err);
  if (result) { signet_memzero(result, strlen(result)); g_free(result); }
  g_free(err_str);
  g_free(pre_err);
  g_free(session_agent_id);
  free(dec_err);

  nostr_nip46_request_free(&req);

  return published;
}
