/* SPDX-License-Identifier: MIT */
#include "session_broker_internal.h"
#include "credential_access_internal.h"
#include "secret_json.h"
#include "signet/audit_logger.h"
#include "signet/capability.h"
#include "signet/store.h"
#include "signet/store_audit.h"
#include "signet/store_leases.h"
#include "signet/util.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <sodium.h>
#include <string.h>

#ifdef SIGNET_HAVE_CURL
#include <curl/curl.h>

/* Constrain the entire HTTP authority to numeric loopback and an optional
 * decimal port. Proxies are disabled below. */
static bool secure_endpoint(const char *url) {
  for (const unsigned char *p = (const unsigned char *)url; *p; p++)
    if (*p <= ' ' || *p >= 127 || *p == '\\') return false;
  if (strchr(url, '#')) return false;
  if (g_str_has_prefix(url, "https://")) {
    const char *authority = url + strlen("https://");
    size_t len = strcspn(authority, "/?");
    return len && !memchr(authority, '@', len);
  }
  static const char loopback[] = "http://127.0.0.1";
  if (!g_str_has_prefix(url, loopback)) return false;
  const char *suffix = url + sizeof(loopback) - 1;
  if (*suffix == ':') {
    if (!g_ascii_isdigit(*++suffix)) return false;
    while (g_ascii_isdigit(*suffix)) suffix++;
  }
  return *suffix == '\0' || *suffix == '/' || *suffix == '?';
}

#define SESSION_RESPONSE_LIMIT (64 * 1024)

typedef struct {
  char *data;
  size_t len;
} CurlBuffer;

static size_t write_response(char *ptr, size_t size, size_t count, void *data) {
  CurlBuffer *buf = data;
  if (size && count > (SESSION_RESPONSE_LIMIT - buf->len) / size) return 0;
  size_t n = size * count;
  memcpy(buf->data + buf->len, ptr, n);
  buf->len += n;
  return n;
}

static bool read_expiry(JsonObject *obj, const char *field, int64_t *value) {
  JsonNode *node = json_object_get_member(obj, field);
  if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_INT64)
    return false;
  *value = json_node_get_int(node);
  return true;
}

static bool parse_response(const CurlBuffer *buf, SignetSessionResult *result) {
  if (!buf->len || memchr(buf->data, '\0', buf->len)) return false;
  /* JSON-GLib exposes strings as NUL-terminated values. Reject a JSON NUL
   * escape instead of returning a truncated token (escaped backslashes are OK). */
  for (size_t i = 0; i + 1 < buf->len; i++) {
    if (buf->data[i] != '\\') continue;
    if (buf->len - i >= 6 && memcmp(buf->data + i, "\\u0000", 6) == 0)
      return false;
    i++;
  }
  g_autoptr(SignetSecretJsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, buf->data, buf->len, NULL)) return false;
  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return false;
  JsonObject *obj = json_node_get_object(root);
  JsonNode *node = json_object_get_member(obj,
      json_object_has_member(obj, "session_token") ? "session_token" : "token");
  if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING)
    return false;
  const char *token = json_node_get_string(node);
  if (!token || !*token || !g_utf8_validate(token, -1, NULL)) return false;

  int64_t now = signet_now_unix();
  int64_t expires_at = now + 3600;
  if (json_object_has_member(obj, "expires_at")) {
    if (!read_expiry(obj, "expires_at", &expires_at) || expires_at <= now)
      return false;
  } else if (json_object_has_member(obj, "expires_in")) {
    int64_t ttl;
    if (!read_expiry(obj, "expires_in", &ttl) || ttl <= 0 || ttl > INT64_MAX - now)
      return false;
    expires_at = now + ttl;
  }
  result->session_token = g_strdup(token);
  result->expires_at = expires_at;
  return true;
}

static bool exchange(const SignetSecretRecord *rec, SignetSessionResult *result) {
  if (!secure_endpoint(rec->service_url) || !rec->payload_len) return false;
  CURL *curl = curl_easy_init();
  if (!curl) return false;
  bool https = g_str_has_prefix(rec->service_url, "https://");
  /* With NOSIGNAL a synchronous resolver cannot be interrupted on timeout. */
  if (https && !(curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_ASYNCHDNS)) {
    curl_easy_cleanup(curl);
    return false;
  }
  CurlBuffer response = { .data = g_malloc(SESSION_RESPONSE_LIMIT) };
  char *b64 = g_base64_encode(rec->payload, rec->payload_len);
  char *body = g_strdup_printf("{\"credential\":\"%s\"}", b64);
  signet_secret_gfree(b64);

  struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
  struct curl_slist *both = headers ? curl_slist_append(headers, "Accept: application/json") : NULL;
  if (both) headers = both;
  long status = 0;
  bool ok = both &&
      curl_easy_setopt(curl, CURLOPT_URL, rec->service_url) == CURLE_OK &&
#if LIBCURL_VERSION_NUM >= 0x075500
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR,
                       https ? "https" : "http") == CURLE_OK &&
#else
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                       https ? CURLPROTO_HTTPS : CURLPROTO_HTTP) == CURLE_OK &&
#endif
      curl_easy_setopt(curl, CURLOPT_PROXY, "") == CURLE_OK &&
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L) == CURLE_OK &&
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L) == CURLE_OK &&
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) == CURLE_OK &&
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response) == CURLE_OK &&
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response) == CURLE_OK &&
      curl_easy_perform(curl) == CURLE_OK &&
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK &&
      status >= 200 && status < 300 && parse_response(&response, result);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  signet_secret_bytes_free(response.data, response.len);
  signet_secret_gfree(body);
  return ok;
}
#endif /* SIGNET_HAVE_CURL: without libcurl, fail closed. */

SignetSessionStatus signet_session_broker_get_for_transport(
    const SignetCredentialAccessContext *ctx, const SignetSessionRequest *req,
    const char *transport, SignetSessionResult *result,
    SignetCredAccessStatus *access_status) {
  memset(result, 0, sizeof(*result));
  SignetCredentialAccessRequest access = {
    .agent_id = req->agent_id,
    .credential_id = req->credential_id,
    .capability = SIGNET_CAP_CREDENTIAL_GET_SESSION,
    .transport = transport,
  };
  SignetCredentialAccessGrant grant;
  *access_status = signet_credential_access_acquire_session(
      ctx, &access, req->service_url, signet_now_unix(), &grant);
  if (*access_status != SIGNET_CRED_ACCESS_OK) return SIGNET_SESSION_ACCESS_FAILED;

  g_autofree char *credential_id = g_strdup(grant.record.id);
  bool exchanged = false;
#ifdef SIGNET_HAVE_CURL
  exchanged = exchange(&grant.record, result);
#endif
  signet_credential_access_grant_clear(&grant);
  int64_t now = signet_now_unix();
  if (!exchanged || result->expires_at <= now) {
    signet_session_result_clear(result);
    (void)signet_audit_log_append(ctx->store, now, req->agent_id, "session_issue",
        credential_id, transport, "{\"decision\":\"error\",\"reason\":\"exchange_failed\"}");
    return SIGNET_SESSION_EXCHANGE_FAILED;
  }

  uint8_t raw[16];
  randombytes_buf(raw, sizeof(raw));
  char lease_id[sizeof(raw) * 2 + 1];
  sodium_bin2hex(lease_id, sizeof(lease_id), raw, sizeof(raw));
  uint8_t digest[crypto_hash_sha256_BYTES];
  char hash[sizeof(digest) * 2 + 1];
  crypto_hash_sha256(digest, (const unsigned char *)result->session_token,
                     strlen(result->session_token));
  sodium_bin2hex(hash, sizeof(hash), digest, sizeof(digest));
  g_autofree char *meta = g_strdup_printf("{\"session_token_hash\":\"%s\"}", hash);
  if (signet_store_issue_lease(ctx->store, lease_id, credential_id, req->agent_id,
                               now, result->expires_at, meta) != 0) {
    (void)signet_audit_log_append(ctx->store, now, req->agent_id, "session_issue",
        credential_id, transport, "{\"decision\":\"error\",\"reason\":\"lease_persist_failed\"}");
    signet_session_result_clear(result);
    return SIGNET_SESSION_STORE_FAILED;
  }
  g_autofree char *detail = g_strdup_printf(
      "{\"decision\":\"allow\",\"reason\":\"ok\",\"lease_id\":\"%s\"}", lease_id);
  if (signet_audit_log_append(ctx->store, now, req->agent_id, "session_issue",
                               credential_id, transport, detail) != 0) {
    (void)signet_store_revoke_lease(ctx->store, lease_id, now);
    signet_session_result_clear(result);
    return SIGNET_SESSION_STORE_FAILED;
  }
  result->lease_id = g_strdup(lease_id);
  return SIGNET_SESSION_OK;
}

int signet_session_broker_get(SignetStore *store, SignetPolicyRegistry *policy,
                              struct SignetDenyList *deny, SignetAuditLogger *audit,
                              const SignetSessionRequest *req, SignetSessionResult *result) {
  if (!store || !req || !req->credential_id || !req->agent_id || !result) return -1;
  SignetCredentialAccessContext ctx = {
    .store = store, .policy = policy, .deny = deny, .logger = audit,
  };
  SignetCredAccessStatus access_status;
  return signet_session_broker_get_for_transport(&ctx, req, "session_broker", result,
      &access_status) == SIGNET_SESSION_OK ? 0 : -1;
}

void signet_session_result_clear(SignetSessionResult *result) {
  if (!result) return;
  signet_secret_gfree(result->session_token);
  g_free(result->lease_id);
  memset(result, 0, sizeof(*result));
}
