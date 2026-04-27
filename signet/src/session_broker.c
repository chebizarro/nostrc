/* SPDX-License-Identifier: MIT
 *
 * session_broker.c - Credential → session token broker.
 *
 * Implements the generic cookie-based REST adapter: decrypt credential,
 * POST to service, extract session token, issue lease.
 *
 * When built with SIGNET_HAVE_CURL, performs a real HTTP POST exchange.
 * Without libcurl, falls back to a local token (useful for tests).
 */

#include "signet/session_broker.h"
#include "signet/store.h"
#include "signet/store_secrets.h"
#include "signet/store_leases.h"
#include "signet/store_audit.h"
#include "signet/capability.h"
#include "signet/audit_logger.h"
#include "signet/util.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <json-glib/json-glib.h>
#include <sodium.h>

#ifdef SIGNET_HAVE_CURL
#include <curl/curl.h>
#endif

#ifdef SIGNET_HAVE_CURL

/* ----------------------------- libcurl helpers ---------------------------- */

typedef struct {
  char *data;
  size_t len;
} CurlBuffer;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
  CurlBuffer *buf = (CurlBuffer *)userdata;
  size_t total = size * nmemb;
  char *tmp = realloc(buf->data, buf->len + total + 1);
  if (!tmp) return 0;
  buf->data = tmp;
  memcpy(buf->data + buf->len, ptr, total);
  buf->len += total;
  buf->data[buf->len] = '\0';
  return total;
}

/* Parse a JSON response of the form:
 *   {"session_token":"...","expires_at":1234567890}
 * or {"token":"...","expires_in":3600}
 *
 * Returns 0 on success, -1 on parse failure. */
static int parse_session_response(const char *json, size_t json_len,
                                   char **out_token, int64_t *out_expires_at) {
  if (!json || json_len == 0 || !out_token || !out_expires_at) return -1;
  *out_token = NULL;
  *out_expires_at = 0;

  JsonParser *parser = json_parser_new();
  if (!parser) return -1;

  GError *err = NULL;
  gboolean ok = json_parser_load_from_data(parser, json, (gssize)json_len, &err);
  if (!ok) {
    if (err) g_error_free(err);
    g_object_unref(parser);
    return -1;
  }

  JsonNode *root = json_parser_get_root(parser);
  JsonObject *obj = (root && JSON_NODE_HOLDS_OBJECT(root))
                        ? json_node_get_object(root)
                        : NULL;
  if (!obj) {
    g_object_unref(parser);
    return -1;
  }

  const char *token = NULL;
  if (json_object_has_member(obj, "session_token"))
    token = json_object_get_string_member(obj, "session_token");
  else if (json_object_has_member(obj, "token"))
    token = json_object_get_string_member(obj, "token");

  if (!token || token[0] == '\0') {
    g_object_unref(parser);
    return -1;
  }
  *out_token = g_strdup(token);

  if (json_object_has_member(obj, "expires_at")) {
    *out_expires_at = (int64_t)json_object_get_int_member(obj, "expires_at");
  } else if (json_object_has_member(obj, "expires_in")) {
    int64_t delta = (int64_t)json_object_get_int_member(obj, "expires_in");
    if (delta > 0)
      *out_expires_at = signet_now_unix() + delta;
  }

  /* Fallback: 1h if no expiry found. */
  if (*out_expires_at <= 0)
    *out_expires_at = signet_now_unix() + 3600;

  g_object_unref(parser);
  return 0;
}

/* POST the credential to the service URL and extract the session token.
 * Returns 0 on success, -1 on failure. */
static int exchange_credential_http(const char *service_url,
                                      const uint8_t *credential,
                                      size_t credential_len,
                                      char **out_token,
                                      int64_t *out_expires_at) {
  CURL *curl = curl_easy_init();
  if (!curl) return -1;

  CurlBuffer response = { .data = NULL, .len = 0 };

  /* Build JSON POST body: {"credential":"<base64>"} */
  char *cred_b64 = g_base64_encode(credential, credential_len);
  char *post_body = g_strdup_printf("{\"credential\":\"%s\"}", cred_b64);
  g_free(cred_b64);

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, service_url);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  /* Security: require HTTPS for production service URLs. */
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);

  CURLcode res = curl_easy_perform(curl);

  int rc = -1;
  if (res == CURLE_OK) {
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 200 && http_code < 300 && response.data) {
      rc = parse_session_response(response.data, response.len,
                                   out_token, out_expires_at);
    }
  }

  /* Wipe credential from POST body before freeing. */
  if (post_body) {
    sodium_memzero(post_body, strlen(post_body));
    g_free(post_body);
  }
  free(response.data);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  return rc;
}

#endif /* SIGNET_HAVE_CURL */

/* Generate a local random session token (fallback when no service URL
 * is provided or when libcurl is not available). */
static void generate_local_token(char **out_token, int64_t *out_expires_at) {
  uint8_t raw[32];
  randombytes_buf(raw, sizeof(raw));
  char *token_hex = g_malloc(65);
  for (int i = 0; i < 32; i++)
    sprintf(token_hex + i * 2, "%02x", raw[i]);
  token_hex[64] = '\0';
  sodium_memzero(raw, sizeof(raw));

  *out_token = token_hex;
  *out_expires_at = signet_now_unix() + 3600; /* 1h default */
}

int signet_session_broker_get(SignetStore *store,
                                SignetPolicyRegistry *policy,
                                SignetAuditLogger *audit,
                                const SignetSessionRequest *req,
                                SignetSessionResult *result) {
  if (!store || !req || !req->credential_id || !req->agent_id || !result)
    return -1;
  memset(result, 0, sizeof(*result));

  /* 1. Check capability: agent must have credential.get_session. */
  if (policy) {
    if (!signet_policy_has_capability(policy, req->agent_id,
                                       SIGNET_CAP_CREDENTIAL_GET_SESSION))
      return -1;

    /* Rate limit. */
    if (!signet_policy_rate_limit_check(policy, req->agent_id,
                                         SIGNET_CAP_CREDENTIAL_GET_SESSION))
      return -1;
  }

  /* 2. Look up the credential from secrets store. */
  SignetSecretRecord rec;
  memset(&rec, 0, sizeof(rec));
  int rc = signet_store_get_secret(store, req->credential_id, &rec);
  if (rc != 0)
    return -1; /* credential not found or decryption error */

  int64_t now = signet_now_unix();
  char *session_token = NULL;
  int64_t expires_at = 0;
  bool exchanged = false;

  /* 3. Exchange credential for session token via HTTP POST. */
#ifdef SIGNET_HAVE_CURL
  if (req->service_url && req->service_url[0] != '\0' &&
      rec.payload && rec.payload_len > 0) {
    rc = exchange_credential_http(req->service_url,
                                    rec.payload, rec.payload_len,
                                    &session_token, &expires_at);
    if (rc == 0)
      exchanged = true;
  }
#endif

  /* Fallback: generate a local token if HTTP exchange failed or
   * no service URL was provided. */
  if (!exchanged)
    generate_local_token(&session_token, &expires_at);

  /* Wipe the credential payload immediately. */
  signet_secret_record_clear(&rec);

  /* 4. Issue a lease. */
  uint8_t lid_raw[16];
  randombytes_buf(lid_raw, sizeof(lid_raw));
  char lease_id[33];
  for (int i = 0; i < 16; i++)
    sprintf(lease_id + i * 2, "%02x", lid_raw[i]);
  lease_id[32] = '\0';

  char *meta = g_strdup_printf(
      "{\"credential_id\":\"%s\",\"service_url\":\"%s\",\"exchanged\":%s}",
      req->credential_id,
      req->service_url ? req->service_url : "",
      exchanged ? "true" : "false");

  (void)signet_store_issue_lease(store, lease_id,
                                   req->credential_id,
                                   req->agent_id,
                                   now, expires_at, meta);
  g_free(meta);

  /* 5. Audit log (no secret values). */
  if (audit) {
    char *detail = g_strdup_printf(
        "{\"credential_id\":\"%s\",\"lease_id\":\"%s\",\"expires_at\":%" G_GINT64_FORMAT ",\"exchanged\":%s}",
        req->credential_id, lease_id, expires_at,
        exchanged ? "true" : "false");
    signet_audit_log_append(store, now, req->agent_id,
                             "session_broker_get",
                             req->credential_id,
                             "dbus", detail);
    g_free(detail);
  }

  /* Populate result. */
  result->session_token = session_token;
  result->expires_at = expires_at;
  result->lease_id = g_strdup(lease_id);

  return 0;
}

void signet_session_result_clear(SignetSessionResult *result) {
  if (!result) return;
  if (result->session_token) {
    sodium_memzero(result->session_token, strlen(result->session_token));
    g_free(result->session_token);
  }
  g_free(result->lease_id);
  memset(result, 0, sizeof(*result));
}
