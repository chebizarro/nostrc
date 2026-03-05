/* SPDX-License-Identifier: MIT
 *
 * vault_client.c - Vault KV v2 client (libcurl).
 */

#include "signet/vault_client.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <pthread.h>

#include "curl/curl.h"
#include "json-glib/json-glib.h"

#include "secure_buf.h"

struct SignetVaultClient {
  char *base_url;
  char *token;
  char *ca_bundle_path;
  char *namespace_name;
  uint32_t timeout_ms;
};

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;

static void signet_curl_global_init_once(void) {
  (void)curl_global_init(CURL_GLOBAL_DEFAULT);
}

static char *signet_strdup0(const char *s) {
  if (!s) return NULL;
  if (s[0] == '\0') return NULL;
  return strdup(s);
}

static void signet_trim_trailing_slashes(char *s) {
  if (!s) return;
  size_t n = strlen(s);
  while (n > 0 && s[n - 1] == '/') {
    s[n - 1] = '\0';
    n--;
  }
}

static const char *signet_nonempty_or_default(const char *s, const char *def) {
  if (s && s[0] != '\0') return s;
  return def;
}

static void signet_wipe_and_free(char **ps) {
  if (!ps || !*ps) return;
  secure_wipe(*ps, strlen(*ps));
  free(*ps);
  *ps = NULL;
}

void signet_vault_response_clear(SignetVaultResponse *r) {
  if (!r) return;

  if (r->body) {
    /* Body can contain secrets for KV reads; wipe before freeing. */
    secure_wipe(r->body, strlen(r->body));
    free(r->body);
  }
  if (r->error_msg) {
    /* Should not contain secrets, but wipe defensively. */
    secure_wipe(r->error_msg, strlen(r->error_msg));
    free(r->error_msg);
  }

  r->body = NULL;
  r->error_msg = NULL;
  r->http_status = 0;
}

static void signet_vault_set_error(SignetVaultResponse *out, const char *msg) {
  if (!out) return;
  if (out->error_msg) return;
  if (!msg) msg = "vault error";
  out->error_msg = strdup(msg);
}

static bool signet_is_unreserved(unsigned char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
  switch (c) {
    case '-': case '_': case '.': case '~':
      return true;
    default:
      return false;
  }
}

static char *signet_url_escape_component(const char *s, bool preserve_slash) {
  if (!s) return NULL;

  /* First pass: compute length */
  size_t in_len = strlen(s);
  size_t out_len = 0;
  for (size_t i = 0; i < in_len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (preserve_slash && c == '/') {
      out_len += 1;
    } else if (signet_is_unreserved(c)) {
      out_len += 1;
    } else {
      out_len += 3;
    }
  }

  char *out = (char *)malloc(out_len + 1);
  if (!out) return NULL;

  static const char *hex = "0123456789ABCDEF";
  size_t j = 0;
  for (size_t i = 0; i < in_len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (preserve_slash && c == '/') {
      out[j++] = '/';
    } else if (signet_is_unreserved(c)) {
      out[j++] = (char)c;
    } else {
      out[j++] = '%';
      out[j++] = hex[(c >> 4) & 0xF];
      out[j++] = hex[c & 0xF];
    }
  }
  out[j] = '\0';
  return out;
}

static char *signet_strip_leading_slashes_dup(const char *s) {
  if (!s) return NULL;
  while (*s == '/') s++;
  if (*s == '\0') return NULL;
  return strdup(s);
}

static char *signet_vault_build_kv2_url(SignetVaultClient *c,
                                        const char *mount,
                                        const char *path) {
  if (!c || !c->base_url) return NULL;

  char *mount2 = signet_strip_leading_slashes_dup(mount);
  char *path2 = signet_strip_leading_slashes_dup(path);
  if (!mount2 || !path2) {
    free(mount2);
    free(path2);
    return NULL;
  }

  char *mount_esc = signet_url_escape_component(mount2, false);
  char *path_esc = signet_url_escape_component(path2, true);
  free(mount2);
  free(path2);

  if (!mount_esc || !path_esc) {
    free(mount_esc);
    free(path_esc);
    return NULL;
  }

  /* base_url is stored without trailing slash */
  size_t need = strlen(c->base_url) + strlen("/v1/") + strlen(mount_esc) + strlen("/data/") +
                strlen(path_esc) + 1;

  char *url = (char *)malloc(need);
  if (!url) {
    free(mount_esc);
    free(path_esc);
    return NULL;
  }

  (void)snprintf(url, need, "%s/v1/%s/data/%s", c->base_url, mount_esc, path_esc);
  free(mount_esc);
  free(path_esc);
  return url;
}

typedef struct {
  char *ptr;
  size_t len;
  size_t cap;
  size_t limit;
} SignetMemBuf;

static void signet_membuf_free(SignetMemBuf *b) {
  if (!b) return;
  if (b->ptr) {
    secure_wipe(b->ptr, b->len);
    free(b->ptr);
  }
  b->ptr = NULL;
  b->len = 0;
  b->cap = 0;
  b->limit = 0;
}

static int signet_membuf_ensure(SignetMemBuf *b, size_t add) {
  if (!b) return -1;
  if (add > (SIZE_MAX - b->len - 1)) return -1;

  size_t want = b->len + add + 1;
  if (want <= b->cap) return 0;

  size_t new_cap = b->cap ? b->cap : 4096;
  while (new_cap < want) {
    size_t next = new_cap * 2;
    if (next < new_cap) return -1;
    new_cap = next;
  }

  char *np = (char *)realloc(b->ptr, new_cap);
  if (!np) return -1;

  b->ptr = np;
  b->cap = new_cap;
  return 0;
}

static size_t signet_curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  SignetMemBuf *b = (SignetMemBuf *)userdata;
  size_t n = size * nmemb;
  if (!b || !ptr) return 0;

  if (b->limit > 0 && (b->len + n) > b->limit) {
    return 0; /* abort transfer */
  }

  if (signet_membuf_ensure(b, n) != 0) return 0;

  memcpy(b->ptr + b->len, ptr, n);
  b->len += n;
  b->ptr[b->len] = '\0';
  return n;
}

static bool signet_json_validate_if_present(SignetVaultResponse *out) {
  if (!out) return false;
  if (!out->body || out->body[0] == '\0') return true;

  g_autoptr(JsonParser) p = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(p, out->body, -1, &err)) {
    signet_vault_set_error(out, "vault returned non-JSON response");
    if (err) g_error_free(err);
    return false;
  }
  return true;
}

static char *signet_vault_extract_error_string(const char *body) {
  if (!body || body[0] == '\0') return NULL;

  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, body, -1, NULL)) return NULL;

  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;

  JsonObject *o = json_node_get_object(root);
  if (!o) return NULL;

  if (!json_object_has_member(o, "errors")) return NULL;

  JsonArray *arr = json_object_get_array_member(o, "errors");
  if (!arr || json_array_get_length(arr) < 1) return NULL;

  const char *s = json_array_get_string_element(arr, 0);
  if (!s || s[0] == '\0') return NULL;

  return strdup(s);
}

static bool signet_vault_http_request(SignetVaultClient *c,
                                      const char *method,
                                      const char *url,
                                      const char *body,
                                      size_t body_len,
                                      SignetVaultResponse *out) {
  if (!c || !method || !url || !out) return false;

  signet_vault_response_clear(out);

  if (!c->token || c->token[0] == '\0') {
    signet_vault_set_error(out, "missing Vault token (set config token or VAULT_TOKEN)");
    return false;
  }

  pthread_once(&g_curl_once, signet_curl_global_init_once);

  CURL *h = curl_easy_init();
  if (!h) {
    signet_vault_set_error(out, "curl_easy_init failed");
    return false;
  }

  char errbuf[CURL_ERROR_SIZE];
  errbuf[0] = '\0';

  SignetMemBuf buf;
  memset(&buf, 0, sizeof(buf));
  buf.limit = 4 * 1024 * 1024; /* 4MiB cap */

  struct curl_slist *headers = NULL;

  /* Token header (sensitive, never log). */
  size_t tok_need = strlen("X-Vault-Token: ") + strlen(c->token) + 1;
  char *tok_hdr = (char *)malloc(tok_need);
  if (!tok_hdr) {
    curl_easy_cleanup(h);
    signet_vault_set_error(out, "OOM building headers");
    return false;
  }
  (void)snprintf(tok_hdr, tok_need, "X-Vault-Token: %s", c->token);
  headers = curl_slist_append(headers, tok_hdr);
  secure_wipe(tok_hdr, strlen(tok_hdr));
  free(tok_hdr);

  if (c->namespace_name && c->namespace_name[0] != '\0') {
    size_t ns_need = strlen("X-Vault-Namespace: ") + strlen(c->namespace_name) + 1;
    char *ns_hdr = (char *)malloc(ns_need);
    if (!ns_hdr) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(h);
      signet_vault_set_error(out, "OOM building headers");
      return false;
    }
    (void)snprintf(ns_hdr, ns_need, "X-Vault-Namespace: %s", c->namespace_name);
    headers = curl_slist_append(headers, ns_hdr);
    secure_wipe(ns_hdr, strlen(ns_hdr));
    free(ns_hdr);
  }

  headers = curl_slist_append(headers, "Accept: application/json");
  if (strcmp(method, "POST") == 0) {
    headers = curl_slist_append(headers, "Content-Type: application/json");
  }

  (void)curl_easy_setopt(h, CURLOPT_URL, url);
  (void)curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
  (void)curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
  (void)curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, signet_curl_write_cb);
  (void)curl_easy_setopt(h, CURLOPT_WRITEDATA, &buf);
  (void)curl_easy_setopt(h, CURLOPT_USERAGENT, "signet/0.1");
  (void)curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);

  long timeout_ms = (long)(c->timeout_ms ? c->timeout_ms : 10000);
  (void)curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeout_ms);

  (void)curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
  (void)curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
  if (c->ca_bundle_path && c->ca_bundle_path[0] != '\0') {
    (void)curl_easy_setopt(h, CURLOPT_CAINFO, c->ca_bundle_path);
  }

  (void)curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");

  if (strcmp(method, "GET") == 0) {
    /* default */
  } else if (strcmp(method, "POST") == 0) {
    (void)curl_easy_setopt(h, CURLOPT_POST, 1L);
    (void)curl_easy_setopt(h, CURLOPT_POSTFIELDS, body ? body : "");
    (void)curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
  } else if (strcmp(method, "DELETE") == 0) {
    (void)curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");
  } else {
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    signet_vault_set_error(out, "unsupported HTTP method");
    return false;
  }

  CURLcode rc = curl_easy_perform(h);

  long http = 0;
  (void)curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &http);
  out->http_status = http;

  if (buf.ptr) {
    out->body = buf.ptr;
    buf.ptr = NULL;
    buf.len = 0;
    buf.cap = 0;
  }

  signet_membuf_free(&buf);
  curl_slist_free_all(headers);
  curl_easy_cleanup(h);

  if (rc != CURLE_OK) {
    if (errbuf[0] != '\0') {
      out->error_msg = strdup(errbuf);
    } else {
      out->error_msg = strdup(curl_easy_strerror(rc));
    }
    return false;
  }

  if (http < 200 || http >= 300) {
    char *vault_err = signet_vault_extract_error_string(out->body);
    if (vault_err) {
      out->error_msg = vault_err;
    } else {
      char tmp[128];
      (void)snprintf(tmp, sizeof(tmp), "vault HTTP error %ld", http);
      out->error_msg = strdup(tmp);
    }
    return false;
  }

  /* Validate JSON when a body is present (Vault typically returns JSON). */
  if (!signet_json_validate_if_present(out)) return false;

  return true;
}

SignetVaultClient *signet_vault_client_new(const SignetVaultClientConfig *cfg) {
  if (!cfg) return NULL;
  if (!cfg->base_url || cfg->base_url[0] == '\0') return NULL;

  const char *env_tok = getenv("VAULT_TOKEN");
  const char *token = (env_tok && env_tok[0] != '\0') ? env_tok : cfg->token;

  SignetVaultClient *c = (SignetVaultClient *)calloc(1, sizeof(*c));
  if (!c) return NULL;

  c->base_url = strdup(cfg->base_url);
  c->token = signet_strdup0(token);
  c->ca_bundle_path = signet_strdup0(cfg->ca_bundle_path);
  c->namespace_name = signet_strdup0(cfg->namespace_name);
  c->timeout_ms = cfg->timeout_ms;

  if (!c->base_url) {
    signet_vault_client_free(c);
    return NULL;
  }

  signet_trim_trailing_slashes(c->base_url);
  return c;
}

void signet_vault_client_free(SignetVaultClient *c) {
  if (!c) return;

  if (c->base_url) free(c->base_url);
  signet_wipe_and_free(&c->token);
  if (c->ca_bundle_path) free(c->ca_bundle_path);
  if (c->namespace_name) free(c->namespace_name);

  c->base_url = NULL;
  c->ca_bundle_path = NULL;
  c->namespace_name = NULL;
  c->timeout_ms = 0;

  free(c);
}

bool signet_vault_kv2_read(SignetVaultClient *c,
                           const char *mount,
                           const char *path,
                           SignetVaultResponse *out) {
  if (!c || !mount || mount[0] == '\0' || !path || path[0] == '\0' || !out) return false;

  char *url = signet_vault_build_kv2_url(c, mount, path);
  if (!url) {
    signet_vault_response_clear(out);
    signet_vault_set_error(out, "failed to build Vault KV v2 URL");
    return false;
  }

  bool ok = signet_vault_http_request(c, "GET", url, NULL, 0, out);
  free(url);
  return ok;
}

bool signet_vault_kv2_write(SignetVaultClient *c,
                            const char *mount,
                            const char *path,
                            const char *data_json_object,
                            SignetVaultResponse *out) {
  if (!c || !mount || mount[0] == '\0' || !path || path[0] == '\0' || !out) return false;
  if (!data_json_object || data_json_object[0] == '\0') {
    signet_vault_response_clear(out);
    signet_vault_set_error(out, "missing data_json_object");
    return false;
  }

  /* Validate that data_json_object is a JSON object, and build {"data": <object>}. */
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, data_json_object, -1, NULL)) {
    signet_vault_response_clear(out);
    signet_vault_set_error(out, "data_json_object is not valid JSON");
    return false;
  }

  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    signet_vault_response_clear(out);
    signet_vault_set_error(out, "data_json_object must be a JSON object");
    return false;
  }

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "data");
  json_builder_add_value(b, json_node_copy(root));
  json_builder_end_object(b);

  JsonNode *wrap_root = json_builder_get_root(b);
  if (!wrap_root) {
    signet_vault_response_clear(out);
    signet_vault_set_error(out, "failed to build Vault request JSON");
    return false;
  }

  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, wrap_root);
  json_generator_set_pretty(gen, FALSE);

  gchar *wrap_body_g = json_generator_to_data(gen, NULL);
  json_node_unref(wrap_root);

  if (!wrap_body_g) {
    signet_vault_response_clear(out);
    signet_vault_set_error(out, "failed to serialize Vault request JSON");
    return false;
  }

  /* Copy to malloc() memory so we can securely wipe it after request. */
  char *wrap_body = strdup(wrap_body_g);
  g_free(wrap_body_g);
  if (!wrap_body) {
    signet_vault_response_clear(out);
    signet_vault_set_error(out, "OOM building Vault request JSON");
    return false;
  }

  char *url = signet_vault_build_kv2_url(c, mount, path);
  if (!url) {
    secure_wipe(wrap_body, strlen(wrap_body));
    free(wrap_body);
    signet_vault_response_clear(out);
    signet_vault_set_error(out, "failed to build Vault KV v2 URL");
    return false;
  }

  bool ok = signet_vault_http_request(c, "POST", url, wrap_body, strlen(wrap_body), out);

  secure_wipe(wrap_body, strlen(wrap_body));
  free(wrap_body);
  free(url);
  return ok;
}

bool signet_vault_kv2_delete_latest(SignetVaultClient *c,
                                    const char *mount,
                                    const char *path,
                                    SignetVaultResponse *out) {
  if (!c || !mount || mount[0] == '\0' || !path || path[0] == '\0' || !out) return false;

  char *url = signet_vault_build_kv2_url(c, mount, path);
  if (!url) {
    signet_vault_response_clear(out);
    signet_vault_set_error(out, "failed to build Vault KV v2 URL");
    return false;
  }

  bool ok = signet_vault_http_request(c, "DELETE", url, NULL, 0, out);
  free(url);
  return ok;
}