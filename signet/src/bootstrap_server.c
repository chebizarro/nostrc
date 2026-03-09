/* SPDX-License-Identifier: MIT
 *
 * bootstrap_server.c - HTTP endpoints for Signet v2 bootstrap and re-auth.
 *
 * POST /bootstrap  - Verify bootstrap token, provision agent, return URI
 * GET  /challenge  - Issue challenge for agent re-authentication
 * POST /auth       - Verify signed auth event, return session token
 *
 * Uses libmicrohttpd following the same pattern as health_server.c.
 */

#include "signet/bootstrap_server.h"
#include "signet/key_store.h"
#include "signet/store.h"
#include "signet/store_tokens.h"
#include "signet/nostr_auth.h"
#include "signet/audit_logger.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
/* glib.h is found via pkg-config glib-2.0 --cflags */
#include <json-glib/json-glib.h>
#include <microhttpd.h>
#include <sodium.h>

/* Max POST body size (8 KiB — bootstrap tokens and auth events are small). */
#define MAX_POST_SIZE 8192

struct SignetBootstrapServer {
  char *listen;
  struct MHD_Daemon *mhd;
  SignetKeyStore *keys;
  SignetStore *store;
  SignetChallengeStore *challenges;
  SignetAuditLogger *audit;
  const SignetFleetRegistry *fleet;
  char **relay_urls;
  size_t n_relay_urls;
};

/* Per-connection context for accumulating POST body. */
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} PostCtx;

static int64_t signet_now_unix(void) {
  return (int64_t)time(NULL);
}

/* ----------------------------- JSON helpers ------------------------------- */

static char *json_error(const char *message) {
  return g_strdup_printf("{\"error\":\"%s\"}", message);
}

static enum MHD_Result
send_json(struct MHD_Connection *conn, unsigned int status, char *json) {
  struct MHD_Response *resp = MHD_create_response_from_buffer(
      strlen(json), json, MHD_RESPMEM_MUST_FREE);
  MHD_add_response_header(resp, "Content-Type", "application/json");
  MHD_add_response_header(resp, "Cache-Control", "no-store");
  enum MHD_Result ret = MHD_queue_response(conn, status, resp);
  MHD_destroy_response(resp);
  return ret;
}

/* ----------------------------- POST /bootstrap --------------------------- */

/* Request JSON:
 *   {"token": "<raw_token>", "agent_id": "<id>", "bootstrap_pubkey": "<hex>"}
 * Response JSON (200):
 *   {"uri": "nostrconnect://...", "pubkey": "<hex>"}
 * Errors: 400, 403, 500
 */
static enum MHD_Result
handle_bootstrap(SignetBootstrapServer *bs, struct MHD_Connection *conn,
                 const char *body, size_t body_len) {
  (void)body_len;

  /* Parse JSON body. */
  JsonParser *parser = json_parser_new();
  if (!parser)
    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                     json_error("internal error"));

  GError *err = NULL;
  if (!json_parser_load_from_data(parser, body, -1, &err)) {
    g_object_unref(parser);
    if (err) g_error_free(err);
    return send_json(conn, MHD_HTTP_BAD_REQUEST,
                     json_error("invalid JSON"));
  }

  JsonNode *root = json_parser_get_root(parser);
  JsonObject *obj = (root && JSON_NODE_HOLDS_OBJECT(root))
                        ? json_node_get_object(root)
                        : NULL;
  if (!obj) {
    g_object_unref(parser);
    return send_json(conn, MHD_HTTP_BAD_REQUEST,
                     json_error("expected JSON object"));
  }

  const char *token = json_object_has_member(obj, "token")
                          ? json_object_get_string_member(obj, "token")
                          : NULL;
  const char *agent_id = json_object_has_member(obj, "agent_id")
                             ? json_object_get_string_member(obj, "agent_id")
                             : NULL;
  const char *bootstrap_pubkey = json_object_has_member(obj, "bootstrap_pubkey")
                                     ? json_object_get_string_member(obj, "bootstrap_pubkey")
                                     : NULL;

  if (!token || !token[0] || !agent_id || !agent_id[0] ||
      !bootstrap_pubkey || !bootstrap_pubkey[0]) {
    g_object_unref(parser);
    return send_json(conn, MHD_HTTP_BAD_REQUEST,
                     json_error("missing token, agent_id, or bootstrap_pubkey"));
  }

  /* Hash the raw token for store lookup. */
  uint8_t hash_raw[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(hash_raw, (const uint8_t *)token, strlen(token));
  char token_hash[crypto_hash_sha256_BYTES * 2 + 1];
  for (size_t i = 0; i < crypto_hash_sha256_BYTES; i++)
    sprintf(token_hash + i * 2, "%02x", hash_raw[i]);
  token_hash[sizeof(token_hash) - 1] = '\0';
  sodium_memzero(hash_raw, sizeof(hash_raw));

  /* Verify the bootstrap token. */
  int64_t now = signet_now_unix();
  SignetTokenResult tr = signet_store_verify_bootstrap_token(
      bs->store, token_hash, agent_id, bootstrap_pubkey, now);

  if (tr != SIGNET_TOKEN_OK) {
    g_object_unref(parser);
    const char *msg = "token verification failed";
    unsigned int status = MHD_HTTP_FORBIDDEN;
    switch (tr) {
    case SIGNET_TOKEN_NOT_FOUND:     msg = "unknown token"; break;
    case SIGNET_TOKEN_EXPIRED:       msg = "token expired"; break;
    case SIGNET_TOKEN_ALREADY_USED:  msg = "token already used"; break;
    case SIGNET_TOKEN_MAX_ATTEMPTS:  msg = "max attempts exceeded"; break;
    case SIGNET_TOKEN_PUBKEY_MISMATCH: msg = "pubkey mismatch"; break;
    case SIGNET_TOKEN_AGENT_MISMATCH:  msg = "agent_id mismatch"; break;
    default: status = MHD_HTTP_INTERNAL_SERVER_ERROR; msg = "internal error"; break;
    }
    return send_json(conn, status, json_error(msg));
  }

  /* Token is valid — provision the agent keypair. */
  char pubkey_hex[65];
  char *bunker_uri = NULL;
  int rc = signet_key_store_provision_agent(
      bs->keys, agent_id,
      (const char *const *)bs->relay_urls, bs->n_relay_urls,
      pubkey_hex, sizeof(pubkey_hex), &bunker_uri);

  if (rc != 0) {
    g_object_unref(parser);
    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                     json_error("keypair provisioning failed"));
  }

  /* Mark token consumed only after successful provisioning. */
  (void)signet_store_consume_bootstrap_token(bs->store, token_hash, now);

  /* Build success response. */
  char *resp_json = g_strdup_printf(
      "{\"uri\":\"%s\",\"pubkey\":\"%s\"}",
      bunker_uri ? bunker_uri : "", pubkey_hex);

  g_free(bunker_uri);
  g_object_unref(parser);

  return send_json(conn, MHD_HTTP_OK, resp_json);
}

/* ----------------------------- GET /challenge ----------------------------- */

/* Query params: ?agent_id=<id>
 * Response JSON (200): {"challenge": "<hex>"}
 */
static enum MHD_Result
handle_challenge(SignetBootstrapServer *bs, struct MHD_Connection *conn) {
  const char *agent_id = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "agent_id");

  if (!agent_id || !agent_id[0])
    return send_json(conn, MHD_HTTP_BAD_REQUEST,
                     json_error("missing agent_id query parameter"));

  int64_t now = signet_now_unix();
  char *challenge = signet_challenge_issue(bs->challenges, agent_id, now);
  if (!challenge)
    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                     json_error("failed to issue challenge"));

  char *resp_json = g_strdup_printf("{\"challenge\":\"%s\"}", challenge);
  g_free(challenge);

  return send_json(conn, MHD_HTTP_OK, resp_json);
}

/* ----------------------------- POST /auth -------------------------------- */

/* Request JSON: the signed Nostr auth event (kind SIGNET_AUTH_KIND).
 * Response JSON (200): {"session_token": "<hex>", "expires_at": <unix_ts>}
 * Errors: 400, 401, 403
 */
static enum MHD_Result
handle_auth(SignetBootstrapServer *bs, struct MHD_Connection *conn,
            const char *body, size_t body_len) {
  (void)body_len;

  if (!body || !body[0])
    return send_json(conn, MHD_HTTP_BAD_REQUEST,
                     json_error("empty body"));

  int64_t now = signet_now_unix();
  char *agent_id = NULL;
  char *pubkey_hex = NULL;

  SignetAuthResult ar = signet_auth_verify(
      bs->challenges, bs->fleet, body, now, &agent_id, &pubkey_hex);

  if (ar != SIGNET_AUTH_OK) {
    g_free(agent_id);
    g_free(pubkey_hex);
    unsigned int status = (ar == SIGNET_AUTH_ERR_DENIED ||
                           ar == SIGNET_AUTH_ERR_NOT_IN_FLEET)
                              ? MHD_HTTP_FORBIDDEN
                              : MHD_HTTP_UNAUTHORIZED;
    return send_json(conn, status,
                     json_error(signet_auth_result_string(ar)));
  }

  /* Generate session token: 32 random bytes → 64-char hex. */
  uint8_t session_raw[32];
  randombytes_buf(session_raw, sizeof(session_raw));
  char session_hex[65];
  for (int i = 0; i < 32; i++)
    sprintf(session_hex + i * 2, "%02x", session_raw[i]);
  session_hex[64] = '\0';
  sodium_memzero(session_raw, sizeof(session_raw));

  int64_t expires_at = now + SIGNET_SESSION_TTL_S;

  char *resp_json = g_strdup_printf(
      "{\"session_token\":\"%s\",\"agent_id\":\"%s\",\"pubkey\":\"%s\","
      "\"expires_at\":%" PRId64 "}",
      session_hex, agent_id ? agent_id : "", pubkey_hex ? pubkey_hex : "",
      expires_at);

  g_free(agent_id);
  g_free(pubkey_hex);

  return send_json(conn, MHD_HTTP_OK, resp_json);
}

/* ----------------------------- MHD handler ------------------------------- */

static enum MHD_Result
signet_bootstrap_handler(void *cls,
                         struct MHD_Connection *connection,
                         const char *url,
                         const char *method,
                         const char *version,
                         const char *upload_data,
                         size_t *upload_data_size,
                         void **con_cls) {
  (void)version;
  SignetBootstrapServer *bs = (SignetBootstrapServer *)cls;

  /* First call for a new connection: allocate POST context. */
  if (*con_cls == NULL) {
    PostCtx *pc = g_new0(PostCtx, 1);
    *con_cls = pc;
    return MHD_YES;
  }

  PostCtx *pc = (PostCtx *)*con_cls;

  /* Accumulate POST body. */
  if (*upload_data_size > 0) {
    size_t needed = pc->len + *upload_data_size;
    if (needed > MAX_POST_SIZE) {
      *upload_data_size = 0;
      return send_json(connection, MHD_HTTP_CONTENT_TOO_LARGE,
                       json_error("request too large"));
    }
    if (needed > pc->cap) {
      pc->cap = needed + 256;
      pc->buf = g_realloc(pc->buf, pc->cap + 1);
    }
    memcpy(pc->buf + pc->len, upload_data, *upload_data_size);
    pc->len += *upload_data_size;
    pc->buf[pc->len] = '\0';
    *upload_data_size = 0;
    return MHD_YES;
  }

  /* POST body fully received — dispatch. */

  /* GET /challenge */
  if (strcmp(method, "GET") == 0 && strcmp(url, "/challenge") == 0) {
    return handle_challenge(bs, connection);
  }

  /* POST /bootstrap */
  if (strcmp(method, "POST") == 0 && strcmp(url, "/bootstrap") == 0) {
    return handle_bootstrap(bs, connection, pc->buf, pc->len);
  }

  /* POST /auth */
  if (strcmp(method, "POST") == 0 && strcmp(url, "/auth") == 0) {
    return handle_auth(bs, connection, pc->buf, pc->len);
  }

  return send_json(connection, MHD_HTTP_NOT_FOUND,
                   json_error("not found"));
}

static void signet_post_ctx_free(void *cls,
                                 struct MHD_Connection *connection,
                                 void **con_cls,
                                 enum MHD_RequestTerminationCode toe) {
  (void)cls;
  (void)connection;
  (void)toe;
  PostCtx *pc = (PostCtx *)*con_cls;
  if (pc) {
    g_free(pc->buf);
    g_free(pc);
  }
  *con_cls = NULL;
}

/* ------------------------------ public API -------------------------------- */

SignetBootstrapServer *signet_bootstrap_server_new(const SignetBootstrapServerConfig *cfg) {
  if (!cfg || !cfg->listen || !cfg->keys || !cfg->store || !cfg->challenges)
    return NULL;

  SignetBootstrapServer *bs = (SignetBootstrapServer *)calloc(1, sizeof(*bs));
  if (!bs) return NULL;

  bs->listen = g_strdup(cfg->listen);
  bs->keys = cfg->keys;
  bs->store = cfg->store;
  bs->challenges = cfg->challenges;
  bs->audit = cfg->audit;
  bs->fleet = cfg->fleet;

  /* Copy relay URLs. */
  if (cfg->relay_urls && cfg->n_relay_urls > 0) {
    bs->relay_urls = g_new0(char *, cfg->n_relay_urls + 1);
    for (size_t i = 0; i < cfg->n_relay_urls; i++)
      bs->relay_urls[i] = g_strdup(cfg->relay_urls[i]);
    bs->n_relay_urls = cfg->n_relay_urls;
  }

  return bs;
}

void signet_bootstrap_server_free(SignetBootstrapServer *bs) {
  if (!bs) return;
  signet_bootstrap_server_stop(bs);
  g_free(bs->listen);
  if (bs->relay_urls) {
    for (size_t i = 0; i < bs->n_relay_urls; i++)
      g_free(bs->relay_urls[i]);
    g_free(bs->relay_urls);
  }
  free(bs);
}

int signet_bootstrap_server_start(SignetBootstrapServer *bs) {
  if (!bs || !bs->listen) return -1;
  if (bs->mhd) return 0; /* already started */

  const char *colon = strrchr(bs->listen, ':');
  if (!colon || colon[1] == '\0') return -1;

  unsigned int port = (unsigned int)atoi(colon + 1);
  if (port == 0 && strcmp(colon + 1, "0") != 0) return -1;

  bs->mhd = MHD_start_daemon(
      MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
      (uint16_t)port,
      NULL, NULL,
      signet_bootstrap_handler, bs,
      MHD_OPTION_NOTIFY_COMPLETED, signet_post_ctx_free, bs,
      MHD_OPTION_END);

  return bs->mhd ? 0 : -1;
}

void signet_bootstrap_server_stop(SignetBootstrapServer *bs) {
  if (!bs || !bs->mhd) return;
  MHD_stop_daemon(bs->mhd);
  bs->mhd = NULL;
}
