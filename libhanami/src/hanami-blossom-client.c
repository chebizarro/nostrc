/*
 * hanami-blossom-client.c - Blossom HTTP client (BUD-01/BUD-02)
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-blossom-client.h"
#include "hanami/hanami-blossom-shim.h"
#include "hanami/hanami-bud02-auth.h"
#include "hanami/hanami-index.h"
#include "hanami/hanami-server-capability.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-keys.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/sha.h>

/* =========================================================================
 * Internal structures
 * ========================================================================= */

struct hanami_blossom_client {
    char *endpoint;         /* Owned, no trailing slash */
    char *user_agent;       /* Owned */
    long timeout;           /* Seconds */
    hanami_signer_t signer; /* Copied at creation */
    bool has_signer;
    /* Session-scoped per-server capability record (nostrc-xeby / ypn2).
     * Not persisted; lives for the lifetime of this client handle. */
    hanami_server_capabilities_t caps;
};

/* Dynamic buffer for curl write callbacks */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} dyn_buf_t;

static void dyn_buf_init(dyn_buf_t *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void dyn_buf_free(dyn_buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb,
                            void *userp)
{
    size_t real_size = size * nmemb;
    dyn_buf_t *b = (dyn_buf_t *)userp;

    if (b->len + real_size + 1 > b->cap) {
        size_t new_cap = (b->cap == 0) ? 4096 : b->cap * 2;
        while (new_cap < b->len + real_size + 1)
            new_cap *= 2;
        uint8_t *new_data = realloc(b->data, new_cap);
        if (!new_data)
            return 0; /* Signal error to curl */
        b->data = new_data;
        b->cap = new_cap;
    }

    memcpy(b->data + b->len, contents, real_size);
    b->len += real_size;
    b->data[b->len] = '\0'; /* NUL-terminate for convenience */
    return real_size;
}

/* =========================================================================
 * URL helpers
 * ========================================================================= */

/* Build URL: endpoint + "/" + path. Caller frees. */
static char *build_url(const hanami_blossom_client_t *c, const char *path)
{
    size_t elen = strlen(c->endpoint);
    size_t plen = strlen(path);
    char *url = malloc(elen + 1 + plen + 1);
    if (!url)
        return NULL;
    memcpy(url, c->endpoint, elen);
    url[elen] = '/';
    memcpy(url + elen + 1, path, plen);
    url[elen + 1 + plen] = '\0';
    return url;
}

/* Strip trailing slashes from a string (in-place) */
static void strip_trailing_slash(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == '/')
        s[--len] = '\0';
}

/* =========================================================================
 * Auth helper: create "Authorization: Nostr <base64>" for a signed event
 * ========================================================================= */

static hanami_error_t make_auth_header(const hanami_blossom_client_t *c,
                                       hanami_bud02_action_t action,
                                       const char *sha256_hex,
                                       char **out_header)
{
    if (!c->has_signer)
        return HANAMI_ERR_AUTH;

    /* Create the BUD-02 auth event */
    NostrEvent *ev = hanami_bud02_create_auth_event(
        action, sha256_hex, 0, c->endpoint);
    if (!ev)
        return HANAMI_ERR_NOSTR;

    /* Set pubkey from signer */
    nostr_event_set_pubkey(ev, c->signer.pubkey);

    /* Serialize to JSON for signing */
    char *unsigned_json = nostr_event_serialize_compact(ev);
    if (!unsigned_json) {
        nostr_event_free(ev);
        return HANAMI_ERR_NOSTR;
    }

    /* Sign via callback */
    char *signed_json = NULL;
    hanami_error_t err = c->signer.sign(unsigned_json, &signed_json,
                                         c->signer.user_data);
    free(unsigned_json);

    if (err != HANAMI_OK || !signed_json) {
        nostr_event_free(ev);
        free(signed_json);
        return (err != HANAMI_OK) ? err : HANAMI_ERR_AUTH;
    }

    /* Deserialize the signed event back */
    NostrEvent *signed_ev = nostr_event_new();
    if (!signed_ev) {
        nostr_event_free(ev);
        free(signed_json);
        return HANAMI_ERR_NOMEM;
    }

    if (nostr_event_deserialize_compact(signed_ev, signed_json, NULL) != 1) {
        nostr_event_free(ev);
        nostr_event_free(signed_ev);
        free(signed_json);
        return HANAMI_ERR_NOSTR;
    }
    free(signed_json);
    nostr_event_free(ev);

    /* Create the auth header */
    char *header_val = hanami_bud02_create_auth_header(signed_ev);
    nostr_event_free(signed_ev);
    if (!header_val)
        return HANAMI_ERR_AUTH;

    /* Format as "Authorization: Nostr <base64>" */
    size_t hlen = 16 + strlen(header_val) + 1; /* "Authorization: " + value + NUL */
    char *full_header = malloc(hlen);
    if (!full_header) {
        free(header_val);
        return HANAMI_ERR_NOMEM;
    }
    snprintf(full_header, hlen, "Authorization: %s", header_val);
    free(header_val);

    *out_header = full_header;
    return HANAMI_OK;
}

/* =========================================================================
 * Simple JSON helpers for blob descriptor parsing
 * ========================================================================= */

/* Escape a string for safe inclusion in JSON. Returns malloc'd string or NULL. */
static char *json_escape_string(const char *str)
{
    if (!str) return NULL;

    /* First pass: calculate required size */
    size_t len = 0;
    for (const char *p = str; *p; p++) {
        if (*p == '"' || *p == '\\')
            len += 2; /* \" or \\ */
        else if (*p >= 0x00 && *p <= 0x1F)
            len += 6; /* \uXXXX */
        else
            len += 1;
    }

    char *escaped = malloc(len + 1);
    if (!escaped) return NULL;

    /* Second pass: build escaped string */
    char *out = escaped;
    for (const char *p = str; *p; p++) {
        if (*p == '"') {
            *out++ = '\\';
            *out++ = '"';
        } else if (*p == '\\') {
            *out++ = '\\';
            *out++ = '\\';
        } else if (*p >= 0x00 && *p <= 0x1F) {
            /* Escape control chars as \uXXXX */
            sprintf(out, "\\u%04x", (unsigned char)*p);
            out += 6;
        } else {
            *out++ = *p;
        }
    }
    *out = '\0';
    return escaped;
}

/* Find a JSON string value for a key. Returns malloc'd string or NULL. */
static char *json_get_string(const char *json, const char *key)
{
    if (!json || !key) return NULL;

    /* Search for "key": - use dynamic allocation to avoid truncation */
    size_t key_len = strlen(key);
    char *search = malloc(key_len + 3); /* "key" + NUL */
    if (!search) return NULL;
    snprintf(search, key_len + 3, "\"%s\"", key);
    
    const char *p = strstr(json, search);
    free(search);
    if (!p) return NULL;

    p += key_len + 2; /* skip "key" */
    /* Skip whitespace and colon */
    while (*p && (*p == ' ' || *p == '\t' || *p == ':'))
        p++;
    if (*p != '"') return NULL;
    p++; /* skip opening quote */

    const char *end = p;
    while (*end && *end != '"') {
        if (*end == '\\') {
            end++; /* skip backslash */
            if (*end == 'u') {
                /* \uXXXX - skip 4 hex digits */
                for (int i = 0; i < 4 && end[1]; i++)
                    end++;
            }
        }
        if (*end) end++;
    }

    size_t vlen = (size_t)(end - p);
    char *val = malloc(vlen + 1);
    if (!val) return NULL;
    memcpy(val, p, vlen);
    val[vlen] = '\0';
    return val;
}

/* Find a JSON integer value for a key. Returns 0 if not found. */
static int64_t json_get_int(const char *json, const char *key)
{
    if (!json || !key) return 0;

    /* Use dynamic allocation to avoid truncation */
    size_t key_len = strlen(key);
    char *search = malloc(key_len + 3); /* "key" + NUL */
    if (!search) return 0;
    snprintf(search, key_len + 3, "\"%s\"", key);
    
    const char *p = strstr(json, search);
    free(search);
    if (!p) return 0;

    p += key_len + 2; /* skip "key" */
    while (*p && (*p == ' ' || *p == '\t' || *p == ':'))
        p++;

    return strtoll(p, NULL, 10);
}

/* Parse a Blossom blob descriptor JSON into a struct. */
static hanami_blob_descriptor_t *parse_blob_descriptor(const char *json)
{
    if (!json) return NULL;

    hanami_blob_descriptor_t *desc = calloc(1, sizeof(*desc));
    if (!desc) return NULL;

    char *sha = json_get_string(json, "sha256");
    if (sha) {
        strncpy(desc->sha256, sha, sizeof(desc->sha256) - 1);
        free(sha);
    }

    desc->size = (size_t)json_get_int(json, "size");
    desc->uploaded = json_get_int(json, "uploaded");
    desc->url = json_get_string(json, "url");
    desc->mime_type = json_get_string(json, "type");

    return desc;
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

hanami_error_t hanami_blossom_client_new(const hanami_blossom_client_opts_t *opts,
                                         const hanami_signer_t *signer,
                                         hanami_blossom_client_t **out)
{
    if (!opts || !opts->endpoint || !out)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    hanami_blossom_client_t *c = calloc(1, sizeof(*c));
    if (!c)
        return HANAMI_ERR_NOMEM;

    c->endpoint = strdup(opts->endpoint);
    if (!c->endpoint) {
        free(c);
        return HANAMI_ERR_NOMEM;
    }
    strip_trailing_slash(c->endpoint);

    c->user_agent = strdup(opts->user_agent ? opts->user_agent : "libhanami/0.1");
    if (!c->user_agent) {
        free(c->endpoint);
        free(c);
        return HANAMI_ERR_NOMEM;
    }

    c->timeout = (opts->timeout_seconds > 0) ? opts->timeout_seconds : 30;

    if (signer && signer->sign) {
        c->signer = *signer; /* shallow copy — caller must keep pointers valid */
        c->has_signer = true;
    }

    hanami_server_capabilities_init(&c->caps);

    *out = c;
    return HANAMI_OK;
}

const hanami_server_capabilities_t *
hanami_blossom_get_capabilities(hanami_blossom_client_t *client)
{
    return client ? &client->caps : NULL;
}

hanami_error_t
hanami_blossom_client_set_upload_content_type(hanami_blossom_client_t *client,
                                              const char *content_type)
{
    if (!client) return HANAMI_ERR_INVALID_ARG;
    if (!content_type || !*content_type) {
        client->caps.preferred_content_type[0] = '\0';
        return HANAMI_OK;
    }
    size_t n = strlen(content_type);
    if (n >= HANAMI_PREFERRED_CT_MAX) return HANAMI_ERR_INVALID_ARG;
    /* Reject CR/LF to prevent header injection. */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)content_type[i];
        if (c == '\r' || c == '\n' || c == '\0') return HANAMI_ERR_INVALID_ARG;
    }
    memcpy(client->caps.preferred_content_type, content_type, n + 1);
    return HANAMI_OK;
}

/* Resolve the Content-Type value to send on PUT /upload. Precedence:
 *   1. c->caps.preferred_content_type (per-server override; empty = skip)
 *   2. env HANAMI_BLOSSOM_UPLOAD_CT_ENV (lab / operator override)
 *   3. HANAMI_BLOSSOM_UPLOAD_CONTENT_TYPE (compile-time default)
 * Returns a pointer into a caller-supplied 128-byte buffer. Never NULL.
 * The buffer is used only when the env value is chosen; static strings
 * are returned directly when they fit. Any embedded CR/LF or NUL in the
 * env value is treated as "not set" (defence-in-depth against a malicious
 * env override).
 */
static const char *resolve_upload_content_type(const hanami_blossom_client_t *c,
                                               char buf[HANAMI_PREFERRED_CT_MAX])
{
    if (c && c->caps.preferred_content_type[0] != '\0') {
        /* Per-server override wins; copy into caller buf for uniformity. */
        size_t n = strnlen(c->caps.preferred_content_type,
                           HANAMI_PREFERRED_CT_MAX);
        if (n > 0 && n < HANAMI_PREFERRED_CT_MAX) {
            memcpy(buf, c->caps.preferred_content_type, n);
            buf[n] = '\0';
            return buf;
        }
    }
    const char *env = getenv(HANAMI_BLOSSOM_UPLOAD_CT_ENV);
    if (env && *env) {
        size_t n = strnlen(env, HANAMI_PREFERRED_CT_MAX);
        if (n > 0 && n < HANAMI_PREFERRED_CT_MAX) {
            int clean = 1;
            for (size_t i = 0; i < n; i++) {
                unsigned char ch = (unsigned char)env[i];
                if (ch == '\r' || ch == '\n' || ch == '\0') { clean = 0; break; }
            }
            if (clean) {
                memcpy(buf, env, n);
                buf[n] = '\0';
                return buf;
            }
        }
    }
    return HANAMI_BLOSSOM_UPLOAD_CONTENT_TYPE;
}

/* Format a full "Content-Type: <value>" header string into `out` of size
 * `cap`. Returns 0 on success, -1 if truncated. */
static int format_content_type_header(const hanami_blossom_client_t *c,
                                      char *out, size_t cap)
{
    char ctbuf[HANAMI_PREFERRED_CT_MAX];
    const char *ct = resolve_upload_content_type(c, ctbuf);
    int n = snprintf(out, cap, "Content-Type: %s", ct);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

void hanami_blossom_client_free(hanami_blossom_client_t *client)
{
    if (!client) return;
    free(client->endpoint);
    free(client->user_agent);
    free(client);
}

/* =========================================================================
 * Common curl setup
 * ========================================================================= */

static CURL *setup_curl(const hanami_blossom_client_t *c, const char *url)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, c->timeout);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, c->user_agent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);

    return curl;
}

static hanami_error_t curl_code_to_error(CURLcode code)
{
    switch (code) {
    case CURLE_OK:               return HANAMI_OK;
    case CURLE_OPERATION_TIMEDOUT: return HANAMI_ERR_TIMEOUT;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:  return HANAMI_ERR_NETWORK;
    default:                     return HANAMI_ERR_NETWORK;
    }
}

static hanami_error_t http_status_to_error(long status)
{
    if (status >= 200 && status < 300) return HANAMI_OK;
    if (status == 401 || status == 403) return HANAMI_ERR_AUTH;
    if (status == 404) return HANAMI_ERR_NOT_FOUND;
    if (status >= 400 && status < 500) return HANAMI_ERR_BLOSSOM;
    return HANAMI_ERR_NETWORK;
}

/* =========================================================================
 * BUD-01: GET
 * ========================================================================= */

hanami_error_t hanami_blossom_get(hanami_blossom_client_t *client,
                                  const char *sha256_hex,
                                  uint8_t **out_data,
                                  size_t *out_len)
{
    if (!client || !sha256_hex || !out_data || !out_len)
        return HANAMI_ERR_INVALID_ARG;

    *out_data = NULL;
    *out_len = 0;

    char *url = build_url(client, sha256_hex);
    if (!url)
        return HANAMI_ERR_NOMEM;

    CURL *curl = setup_curl(client, url);
    if (!curl) {
        free(url);
        return HANAMI_ERR_NOMEM;
    }

    dyn_buf_t buf;
    dyn_buf_init(&buf);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    free(url);

    if (res != CURLE_OK) {
        dyn_buf_free(&buf);
        curl_easy_cleanup(curl);
        return curl_code_to_error(res);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    hanami_error_t err = http_status_to_error(status);
    if (err != HANAMI_OK) {
        dyn_buf_free(&buf);
        return err;
    }

    *out_data = buf.data;
    *out_len = buf.len;
    return HANAMI_OK;
}

/* =========================================================================
 * BUD-01: HEAD
 * ========================================================================= */

hanami_error_t hanami_blossom_head(hanami_blossom_client_t *client,
                                   const char *sha256_hex,
                                   bool *out_exists)
{
    if (!client || !sha256_hex || !out_exists)
        return HANAMI_ERR_INVALID_ARG;

    *out_exists = false;

    char *url = build_url(client, sha256_hex);
    if (!url)
        return HANAMI_ERR_NOMEM;

    CURL *curl = setup_curl(client, url);
    if (!curl) {
        free(url);
        return HANAMI_ERR_NOMEM;
    }

    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); /* HEAD request */

    CURLcode res = curl_easy_perform(curl);
    free(url);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return curl_code_to_error(res);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    *out_exists = (status >= 200 && status < 300);
    return HANAMI_OK;
}

/* =========================================================================
 * BUD-02: PUT /upload
 * ========================================================================= */

hanami_error_t hanami_blossom_upload(hanami_blossom_client_t *client,
                                     const uint8_t *data,
                                     size_t len,
                                     const char *sha256_hex,
                                     hanami_blob_descriptor_t **out_desc)
{
    if (!client || !data)
        return HANAMI_ERR_INVALID_ARG;
    if (!client->has_signer)
        return HANAMI_ERR_AUTH;

    /* Compute SHA-256 if not provided */
    char hash_buf[65];
    if (!sha256_hex) {
        hanami_error_t herr = hanami_hash_blossom(data, len, hash_buf);
        if (herr != HANAMI_OK) return herr;
        sha256_hex = hash_buf;
    }

    /* Create auth header */
    char *auth_header = NULL;
    hanami_error_t err = make_auth_header(client, HANAMI_BUD02_ACTION_UPLOAD,
                                          sha256_hex, &auth_header);
    if (err != HANAMI_OK) return err;

    char *url = build_url(client, "upload");
    if (!url) {
        free(auth_header);
        return HANAMI_ERR_NOMEM;
    }

    CURL *curl = setup_curl(client, url);
    if (!curl) {
        free(url);
        free(auth_header);
        return HANAMI_ERR_NOMEM;
    }

    /* PUT with binary body.
     * Use CUSTOMREQUEST + POSTFIELDS (not CURLOPT_UPLOAD which expects
     * a read callback and conflicts with POSTFIELDS). */
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);

    char cth[HANAMI_PREFERRED_CT_MAX + 16];
    if (format_content_type_header(client, cth, sizeof cth) != 0) {
        /* Should be unreachable — resolver-produced values are bounded
         * by HANAMI_PREFERRED_CT_MAX; fall back to the compile-time default. */
        snprintf(cth, sizeof cth, "Content-Type: %s",
                 HANAMI_BLOSSOM_UPLOAD_CONTENT_TYPE);
    }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, cth);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    dyn_buf_t resp;
    dyn_buf_init(&resp);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    free(url);
    free(auth_header);

    if (res != CURLE_OK) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        dyn_buf_free(&resp);
        return curl_code_to_error(res);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    err = http_status_to_error(status);
    if (err != HANAMI_OK) {
        dyn_buf_free(&resp);
        return err;
    }

    /* Parse blob descriptor from response */
    if (out_desc && resp.data) {
        *out_desc = parse_blob_descriptor((const char *)resp.data);
    }

    dyn_buf_free(&resp);
    return HANAMI_OK;
}

/* =========================================================================
 * BUD-02: DELETE
 * ========================================================================= */

hanami_error_t hanami_blossom_delete(hanami_blossom_client_t *client,
                                     const char *sha256_hex)
{
    if (!client || !sha256_hex)
        return HANAMI_ERR_INVALID_ARG;
    if (!client->has_signer)
        return HANAMI_ERR_AUTH;

    /* Create auth header */
    char *auth_header = NULL;
    hanami_error_t err = make_auth_header(client, HANAMI_BUD02_ACTION_DELETE,
                                          sha256_hex, &auth_header);
    if (err != HANAMI_OK) return err;

    char *url = build_url(client, sha256_hex);
    if (!url) {
        free(auth_header);
        return HANAMI_ERR_NOMEM;
    }

    CURL *curl = setup_curl(client, url);
    if (!curl) {
        free(url);
        free(auth_header);
        return HANAMI_ERR_NOMEM;
    }

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    free(url);
    free(auth_header);

    if (res != CURLE_OK) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return curl_code_to_error(res);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return http_status_to_error(status);
}

/* =========================================================================
 * BUD-01: LIST
 * ========================================================================= */

hanami_error_t hanami_blossom_list(hanami_blossom_client_t *client,
                                   const char *pubkey_hex,
                                   char **out_json,
                                   size_t *out_len)
{
    if (!client || !pubkey_hex || !out_json || !out_len)
        return HANAMI_ERR_INVALID_ARG;

    *out_json = NULL;
    *out_len = 0;

    /* Build /list/<pubkey> URL */
    char path[140];
    snprintf(path, sizeof(path), "list/%s", pubkey_hex);

    char *url = build_url(client, path);
    if (!url)
        return HANAMI_ERR_NOMEM;

    CURL *curl = setup_curl(client, url);
    if (!curl) {
        free(url);
        return HANAMI_ERR_NOMEM;
    }

    dyn_buf_t buf;
    dyn_buf_init(&buf);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode res = curl_easy_perform(curl);
    free(url);

    if (res != CURLE_OK) {
        dyn_buf_free(&buf);
        curl_easy_cleanup(curl);
        return curl_code_to_error(res);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    hanami_error_t err = http_status_to_error(status);
    if (err != HANAMI_OK) {
        dyn_buf_free(&buf);
        return err;
    }

    *out_json = (char *)buf.data;
    *out_len = buf.len;
    return HANAMI_OK;
}

/* =========================================================================
 * Batch upload + capability probe (nostrc-xeby / nostrc-ypn2)
 * ========================================================================= */

/* Execute one PUT of `data` (len) to endpoint/upload with the given
 * "Nostr <b64>" header value (NOT the "Authorization:" prefix — this
 * helper adds it). Records HTTP status + hanami_error into out params.
 *
 * The sha256_hex parameter is currently only used for a future
 * per-blob path — Blossom BUD-02 accepts PUTs at /upload (not /<hash>);
 * the server derives the address from sha256(body).
 */
static void put_one_blob(hanami_blossom_client_t *c,
                         const char *sha256_hex,
                         const uint8_t *data, size_t len,
                         const char *auth_header_value,
                         long *out_status,
                         hanami_error_t *out_err)
{
    (void)sha256_hex;
    *out_status = 0;
    *out_err = HANAMI_ERR_NETWORK;

    char *url = build_url(c, "upload");
    if (!url) { *out_err = HANAMI_ERR_NOMEM; return; }

    CURL *curl = setup_curl(c, url);
    if (!curl) { free(url); *out_err = HANAMI_ERR_NOMEM; return; }

    size_t hlen = 15 + 1 + strlen(auth_header_value) + 1; /* "Authorization: " + val + NUL */
    char *full = malloc(hlen);
    if (!full) { curl_easy_cleanup(curl); free(url); *out_err = HANAMI_ERR_NOMEM; return; }
    snprintf(full, hlen, "Authorization: %s", auth_header_value);

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);

    char cth[HANAMI_PREFERRED_CT_MAX + 16];
    if (format_content_type_header(c, cth, sizeof cth) != 0) {
        snprintf(cth, sizeof cth, "Content-Type: %s",
                 HANAMI_BLOSSOM_UPLOAD_CONTENT_TYPE);
    }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, full);
    headers = curl_slist_append(headers, cth);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    dyn_buf_t resp; dyn_buf_init(&resp);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(url);
    free(full);
    dyn_buf_free(&resp);

    if (rc != CURLE_OK) {
        *out_err = curl_code_to_error(rc);
        *out_status = 0;
    } else {
        *out_status = status;
        *out_err = http_status_to_error(status);
    }
}

/* Sign an unsigned event with the client's signer. Returns a NEW event
 * (parsed back from the signed JSON) or NULL. The input `ev` is not
 * modified in a durable way — caller still owns it. */
static NostrEvent *sign_event_via_client(const hanami_blossom_client_t *c,
                                          NostrEvent *ev)
{
    if (!c->has_signer) return NULL;
    nostr_event_set_pubkey(ev, c->signer.pubkey);

    char *unsigned_json = nostr_event_serialize_compact(ev);
    if (!unsigned_json) return NULL;

    char *signed_json = NULL;
    hanami_error_t err = c->signer.sign(unsigned_json, &signed_json,
                                         c->signer.user_data);
    free(unsigned_json);
    if (err != HANAMI_OK || !signed_json) {
        free(signed_json);
        return NULL;
    }

    NostrEvent *signed_ev = nostr_event_new();
    if (!signed_ev) { free(signed_json); return NULL; }
    if (nostr_event_deserialize_compact(signed_ev, signed_json, NULL) != 1) {
        nostr_event_free(signed_ev);
        free(signed_json);
        return NULL;
    }
    free(signed_json);
    return signed_ev;
}

/* Mint a batch auth header value ("Nostr <b64>") shared across N blobs.
 * server_url_or_null: pass NULL to OMIT the server tag (default). */
static char *mint_batch_auth_header(hanami_blossom_client_t *c,
                                    const char *const *hashes, size_t count,
                                    const char *server_url_or_null)
{
    NostrEvent *ev = hanami_bud02_create_batch_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, hashes, count, 0, server_url_or_null);
    if (!ev) return NULL;
    NostrEvent *signed_ev = sign_event_via_client(c, ev);
    nostr_event_free(ev);
    if (!signed_ev) return NULL;
    char *hv = hanami_bud02_create_auth_header(signed_ev);
    nostr_event_free(signed_ev);
    return hv;
}

/* Mint a per-blob auth header value. Server tag omitted by default
 * (cross-server-safe posture; the legacy hanami_blossom_upload keeps
 * its old behaviour of including the tag for backward compat). */
static char *mint_single_auth_header(hanami_blossom_client_t *c,
                                     const char *hash)
{
    NostrEvent *ev = hanami_bud02_create_auth_event(
        HANAMI_BUD02_ACTION_UPLOAD, hash, 0, NULL);
    if (!ev) return NULL;
    NostrEvent *signed_ev = sign_event_via_client(c, ev);
    nostr_event_free(ev);
    if (!signed_ev) return NULL;
    char *hv = hanami_bud02_create_auth_header(signed_ev);
    nostr_event_free(signed_ev);
    return hv;
}

hanami_error_t hanami_blossom_upload_batch(hanami_blossom_client_t *c,
                                           const hanami_blossom_blob_t *blobs,
                                           size_t count,
                                           hanami_blossom_batch_result_t *out_results)
{
    if (!c || !blobs || count == 0 || !out_results)
        return HANAMI_ERR_INVALID_ARG;
    if (!c->has_signer)
        return HANAMI_ERR_AUTH;

    /* Pre-zero results. */
    for (size_t i = 0; i < count; i++) {
        out_results[i].http_status = 0;
        out_results[i].error = HANAMI_ERR_NETWORK;
        out_results[i].error_class = NULL;
    }

    /* Validate hashes up-front (avoids doing PUTs on a bad batch). */
    for (size_t i = 0; i < count; i++) {
        if (!blobs[i].sha256_hex || strlen(blobs[i].sha256_hex) != 64)
            return HANAMI_ERR_INVALID_ARG;
        if (!blobs[i].bytes && blobs[i].len > 0)
            return HANAMI_ERR_INVALID_ARG;
    }

    /* Optional pre-connect probe (nostrc-ypn2). Skipped if the kill-switch
     * env var is set, or if we've already probed this session. */
    if (c->caps.last_probe_ts == 0) {
        const char *skip = getenv("NOSTR_HOMED_HANAMI_SKIP_CAPABILITY_PROBE");
        if (!skip || strcmp(skip, "1") != 0) {
            (void)hanami_server_probe_capabilities(c);
        }
    }

    /* Decide whether to try the batch path first. */
    bool try_batch = (c->caps.batch_ok != HANAMI_CAP_NO);

    const char **hashes = NULL;
    char *batch_header = NULL;

    if (try_batch) {
        hashes = malloc(count * sizeof(*hashes));
        if (!hashes) return HANAMI_ERR_NOMEM;
        for (size_t i = 0; i < count; i++)
            hashes[i] = blobs[i].sha256_hex;

        const char *srv = (c->caps.server_tag_ok == HANAMI_CAP_YES)
                              ? c->endpoint : NULL;
        batch_header = mint_batch_auth_header(c, hashes, count, srv);
        if (!batch_header) {
            free(hashes);
            for (size_t i = 0; i < count; i++)
                out_results[i].error_class = "auth";
            return HANAMI_ERR_AUTH;
        }
    }

    bool any_failed = false;
    hanami_error_t last_err = HANAMI_OK;
    bool fell_back_this_call = false;

    for (size_t i = 0; i < count; i++) {
        const hanami_blossom_blob_t *b = &blobs[i];
        long status = 0;
        hanami_error_t err = HANAMI_ERR_NETWORK;
        const char *cls = NULL;

        if (try_batch) {
            put_one_blob(c, b->sha256_hex, b->bytes, b->len,
                         batch_header, &status, &err);
            if (status == 401) {
                /* Definitive rejection of the batch header. Flip cache,
                 * fall back for THIS blob and every subsequent blob. */
                c->caps.batch_ok = HANAMI_CAP_NO;
                c->caps.last_401_ts = (int64_t)time(NULL);
                try_batch = false;
                fell_back_this_call = true;
                free(batch_header);
                batch_header = NULL;

                char *hdr = mint_single_auth_header(c, b->sha256_hex);
                if (!hdr) {
                    err = HANAMI_ERR_AUTH;
                    status = 0;
                    cls = "auth";
                } else {
                    put_one_blob(c, b->sha256_hex, b->bytes, b->len,
                                 hdr, &status, &err);
                    free(hdr);
                    cls = "batch-fell-back";
                }
            }
        } else {
            char *hdr = mint_single_auth_header(c, b->sha256_hex);
            if (!hdr) {
                err = HANAMI_ERR_AUTH;
                status = 0;
                cls = "auth";
            } else {
                put_one_blob(c, b->sha256_hex, b->bytes, b->len,
                             hdr, &status, &err);
                free(hdr);
                if (fell_back_this_call)
                    cls = "batch-fell-back";
            }
        }

        out_results[i].http_status = status;
        out_results[i].error = err;

        if (err == HANAMI_OK) {
            out_results[i].error_class =
                (cls && strcmp(cls, "batch-fell-back") == 0) ? cls : NULL;
        } else {
            if (!cls) {
                if (status == 401 || status == 403)      cls = "auth";
                else if (status >= 400 && status < 500)  cls = "http-4xx";
                else if (status >= 500)                  cls = "http-5xx";
                else                                     cls = "network";
            }
            out_results[i].error_class = cls;
            any_failed = true;
            last_err = err;
        }
    }

    free(batch_header);
    free(hashes);

    /* Full batch succeeded end-to-end: latch batch_ok=YES so future calls
     * skip re-probing the batch question. */
    if (!any_failed && !fell_back_this_call &&
        c->caps.batch_ok == HANAMI_CAP_UNKNOWN)
        c->caps.batch_ok = HANAMI_CAP_YES;

    return any_failed ? last_err : HANAMI_OK;
}

/* ---- Probe internals (ephemeral key, session-random blobs) ---- */

static int probe_random_bytes(uint8_t *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, buf + off, len - off);
        if (n <= 0) { close(fd); return -1; }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

static void probe_sha256_hex(const uint8_t *data, size_t len, char out_hex[65])
{
    uint8_t md[32];
    SHA256(data, len, md);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[2*i]   = hx[md[i] >> 4];
        out_hex[2*i+1] = hx[md[i] & 0xF];
    }
    out_hex[64] = '\0';
}

/* Mint a probe auth header signed with an ephemeral key.
 * `batch=true` uses the batch constructor with all N hashes;
 * `batch=false` uses the single constructor with hashes[0]. */
static char *probe_mint_header(const char *sk_hex,
                               const char *const *hashes, size_t count,
                               const char *server_url_or_null,
                               bool batch,
                               hanami_bud02_action_t action)
{
    NostrEvent *ev = NULL;
    if (batch) {
        ev = hanami_bud02_create_batch_auth_event(
            action, hashes, count, 0, server_url_or_null);
    } else {
        ev = hanami_bud02_create_auth_event(
            action, hashes ? hashes[0] : NULL, 0, server_url_or_null);
    }
    if (!ev) return NULL;
    if (nostr_event_sign(ev, sk_hex) != 0) {
        nostr_event_free(ev);
        return NULL;
    }
    char *hv = hanami_bud02_create_auth_header(ev);
    nostr_event_free(ev);
    return hv;
}

/* Best-effort DELETE with the given "Nostr <b64>" header. Ignores errors. */
static void probe_delete_blob(hanami_blossom_client_t *c,
                              const char *sha256_hex,
                              const char *auth_header_value)
{
    char *url = build_url(c, sha256_hex);
    if (!url) return;
    CURL *curl = setup_curl(c, url);
    if (!curl) { free(url); return; }

    size_t hlen = 15 + 1 + strlen(auth_header_value) + 1;
    char *full = malloc(hlen);
    if (!full) { curl_easy_cleanup(curl); free(url); return; }
    snprintf(full, hlen, "Authorization: %s", auth_header_value);

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, full);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    /* Cap probe cleanup at 5 s so a slow server can't wedge us. */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    (void)curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(url);
    free(full);
}

hanami_error_t hanami_server_probe_capabilities(hanami_blossom_client_t *c)
{
    if (!c) return HANAMI_ERR_INVALID_ARG;

    /* Ephemeral secp256k1 keypair — never leaves this stack frame. */
    char *sk_hex = nostr_key_generate_private();
    if (!sk_hex) return HANAMI_ERR_NOSTR;

    /* Two 1 KiB session-random blobs. */
    uint8_t blob1[1024], blob2[1024];
    if (probe_random_bytes(blob1, sizeof blob1) != 0 ||
        probe_random_bytes(blob2, sizeof blob2) != 0) {
        free(sk_hex);
        return HANAMI_ERR_NETWORK;
    }
    char h1[65], h2[65];
    probe_sha256_hex(blob1, sizeof blob1, h1);
    probe_sha256_hex(blob2, sizeof blob2, h2);

    /* (a) Reachability probe — BUD-01 HEAD on a session-random hash.
     * Any HTTP response (2xx/4xx) means the server answered. */
    {
        uint8_t reach_rand[32];
        char reach_hash[65];
        if (probe_random_bytes(reach_rand, sizeof reach_rand) == 0) {
            static const char hx[] = "0123456789abcdef";
            for (int i = 0; i < 32; i++) {
                reach_hash[2*i]   = hx[reach_rand[i] >> 4];
                reach_hash[2*i+1] = hx[reach_rand[i] & 0xF];
            }
            reach_hash[64] = '\0';

            char *url = build_url(c, reach_hash);
            if (url) {
                CURL *curl = curl_easy_init();
                if (curl) {
                    curl_easy_setopt(curl, CURLOPT_URL, url);
                    curl_easy_setopt(curl, CURLOPT_USERAGENT, c->user_agent);
                    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
                    CURLcode rc = curl_easy_perform(curl);
                    if (rc == CURLE_OK) {
                        long st = 0;
                        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &st);
                        if (st > 0) c->caps.reachable = true;
                    }
                    curl_easy_cleanup(curl);
                }
                free(url);
            }
        }
    }

    /* (b) server-tag tolerance — PUT h1 with single-x event that has
     * a `server` tag. 2xx -> YES; 401/403 -> NO; other 4xx -> UNKNOWN. */
    bool put_b_success = false;
    {
        const char *hh[1] = { h1 };
        char *hv = probe_mint_header(sk_hex, hh, 1, c->endpoint,
                                     false, HANAMI_BUD02_ACTION_UPLOAD);
        if (hv) {
            long st = 0; hanami_error_t err;
            put_one_blob(c, h1, blob1, sizeof blob1, hv, &st, &err);
            free(hv);
            if (st >= 200 && st < 300) {
                c->caps.server_tag_ok = HANAMI_CAP_YES;
                put_b_success = true;
            } else if (st == 401 || st == 403) {
                c->caps.server_tag_ok = HANAMI_CAP_NO;
            }
        }
    }

    /* (c) batch viability — one event with x=[h1,h2], PUT h1 then h2.
     * Any 401 -> NO; both 2xx -> YES; other 4xx -> UNKNOWN. */
    bool put_c_success_h1 = false, put_c_success_h2 = false;
    {
        const char *hh[2] = { h1, h2 };
        char *hv = probe_mint_header(sk_hex, hh, 2, NULL,
                                     true, HANAMI_BUD02_ACTION_UPLOAD);
        if (hv) {
            long st1 = 0, st2 = 0; hanami_error_t err;
            put_one_blob(c, h1, blob1, sizeof blob1, hv, &st1, &err);
            put_one_blob(c, h2, blob2, sizeof blob2, hv, &st2, &err);
            free(hv);
            bool any_401 = (st1 == 401 || st2 == 401);
            bool both_ok = (st1 >= 200 && st1 < 300) &&
                           (st2 >= 200 && st2 < 300);
            if (any_401) {
                c->caps.batch_ok = HANAMI_CAP_NO;
                c->caps.last_401_ts = (int64_t)time(NULL);
            } else if (both_ok) {
                c->caps.batch_ok = HANAMI_CAP_YES;
                put_c_success_h1 = true;
                put_c_success_h2 = true;
            }
        }
    }

    /* (d) strict-x binding — event with x=[h1], PUT blob2 (hash H2).
     * 401 -> STRICT; 2xx/non-auth 4xx -> PERMISSIVE. */
    {
        const char *hh[1] = { h1 };
        char *hv = probe_mint_header(sk_hex, hh, 1, NULL,
                                     false, HANAMI_BUD02_ACTION_UPLOAD);
        if (hv) {
            long st = 0; hanami_error_t err;
            put_one_blob(c, h2, blob2, sizeof blob2, hv, &st, &err);
            free(hv);
            if (st == 401 || st == 403) {
                c->caps.strict_x_binding = HANAMI_CAP_YES;
                c->caps.last_401_ts = (int64_t)time(NULL);
            } else if (st > 0) {
                c->caps.strict_x_binding = HANAMI_CAP_NO;
            }
        }
    }

    /* (e) raw-random body-sniffer probe (nostrc-prli). Upload one
     * fresh 1 KiB session-random blob with a single-x auth event and
     * the client's default Content-Type (typically
     * application/octet-stream). 2xx -> raw_random_ok=YES; a body-
     * sniffer 4xx (415 unsupported media type, 400 content-mismatch)
     * -> raw_random_ok=NO. 401/403 leaves the flag UNKNOWN — that
     * failure is auth, not content-policy, and shouldn't feed into
     * the shim auto-decide. */
    uint8_t blob3[1024];
    char h3[65];
    bool put_e_success = false;
    if (probe_random_bytes(blob3, sizeof blob3) == 0) {
        probe_sha256_hex(blob3, sizeof blob3, h3);
        const char *hh[1] = { h3 };
        char *hv = probe_mint_header(sk_hex, hh, 1, NULL,
                                     false, HANAMI_BUD02_ACTION_UPLOAD);
        if (hv) {
            long st = 0; hanami_error_t err;
            put_one_blob(c, h3, blob3, sizeof blob3, hv, &st, &err);
            free(hv);
            if (st >= 200 && st < 300) {
                c->caps.raw_random_ok = HANAMI_CAP_YES;
                put_e_success = true;
            } else if (st == 400 || st == 415 || st == 422 ||
                       (st >= 500 && st < 600)) {
                /* 4xx content-policy or 5xx decoder blowup — either way
                 * this server did not accept the raw random bytes. */
                c->caps.raw_random_ok = HANAMI_CAP_NO;
            }
        }
    }

    /* (f) PNG-shim body-sniffer probe (nostrc-prli). Wrap the SAME
     * fresh 1 KiB random blob with the deterministic 41-byte PNG
     * shim, upload with Content-Type: image/png, single-x auth over
     * the shim-inclusive sha256. 2xx -> png_shim_ok=YES; 4xx/5xx ->
     * png_shim_ok=NO. This is the empirical test that unlocked
     * blossom.primal.net in hy3e §2 while blossom.band's full-PNG
     * decoder still returned 500 on the truncated IDAT stream. */
    bool put_f_success = false;
    char h_shim[65] = {0};
    {
        size_t enc_len = hanami_blossom_shim_encoded_len(sizeof blob3);
        uint8_t *shim_buf = (uint8_t *)malloc(enc_len);
        if (shim_buf) {
            if (hanami_blossom_shim_encode(blob3, sizeof blob3,
                                           shim_buf, enc_len) == HANAMI_OK) {
                probe_sha256_hex(shim_buf, enc_len, h_shim);
                const char *hh[1] = { h_shim };
                char *hv = probe_mint_header(sk_hex, hh, 1, NULL,
                                             false,
                                             HANAMI_BUD02_ACTION_UPLOAD);
                if (hv) {
                    /* Temporarily flip the client's preferred
                     * Content-Type to image/png so put_one_blob picks
                     * it up; restore afterwards. */
                    char saved_ct[HANAMI_PREFERRED_CT_MAX];
                    memcpy(saved_ct, c->caps.preferred_content_type,
                           sizeof saved_ct);
                    (void)hanami_blossom_client_set_upload_content_type(
                              c, "image/png");

                    long st = 0; hanami_error_t err;
                    put_one_blob(c, h_shim, shim_buf, enc_len, hv,
                                 &st, &err);
                    free(hv);

                    /* Restore whatever was there before (including
                     * "empty" == client default). */
                    memcpy(c->caps.preferred_content_type, saved_ct,
                           sizeof saved_ct);

                    if (st >= 200 && st < 300) {
                        c->caps.png_shim_ok = HANAMI_CAP_YES;
                        put_f_success = true;
                    } else if (st == 400 || st == 415 || st == 422 ||
                               (st >= 500 && st < 600)) {
                        c->caps.png_shim_ok = HANAMI_CAP_NO;
                    }
                }
            }
            free(shim_buf);
        }
    }

    c->caps.last_probe_ts = (int64_t)time(NULL);

    /* Best-effort cleanup. Only bothers with blobs we saw stored (2xx).
     * Uses the same ephemeral key so per-pubkey delete auth matches. */
    if (put_b_success || put_c_success_h1) {
        const char *hh[1] = { h1 };
        char *hv = probe_mint_header(sk_hex, hh, 1, NULL,
                                     false, HANAMI_BUD02_ACTION_DELETE);
        if (hv) { probe_delete_blob(c, h1, hv); free(hv); }
    }
    if (put_c_success_h2) {
        const char *hh[1] = { h2 };
        char *hv = probe_mint_header(sk_hex, hh, 1, NULL,
                                     false, HANAMI_BUD02_ACTION_DELETE);
        if (hv) { probe_delete_blob(c, h2, hv); free(hv); }
    }
    if (put_e_success) {
        const char *hh[1] = { h3 };
        char *hv = probe_mint_header(sk_hex, hh, 1, NULL,
                                     false, HANAMI_BUD02_ACTION_DELETE);
        if (hv) { probe_delete_blob(c, h3, hv); free(hv); }
    }
    if (put_f_success) {
        const char *hh[1] = { h_shim };
        char *hv = probe_mint_header(sk_hex, hh, 1, NULL,
                                     false, HANAMI_BUD02_ACTION_DELETE);
        if (hv) { probe_delete_blob(c, h_shim, hv); free(hv); }
    }

    free(sk_hex);
    return HANAMI_OK;
}

/* =========================================================================
 * BUD-04: MIRROR
 * ========================================================================= */

hanami_error_t hanami_blossom_mirror(hanami_blossom_client_t *client,
                                     const char *source_url,
                                     hanami_blob_descriptor_t **out_desc)
{
    if (!client || !source_url)
        return HANAMI_ERR_INVALID_ARG;
    if (!client->has_signer)
        return HANAMI_ERR_AUTH;

    /* Create auth header (no specific hash for mirror) */
    char *auth_header = NULL;
    hanami_error_t err = make_auth_header(client, HANAMI_BUD02_ACTION_MIRROR,
                                          NULL, &auth_header);
    if (err != HANAMI_OK) return err;

    char *url = build_url(client, "mirror");
    if (!url) {
        free(auth_header);
        return HANAMI_ERR_NOMEM;
    }

    CURL *curl = setup_curl(client, url);
    if (!curl) {
        free(url);
        free(auth_header);
        return HANAMI_ERR_NOMEM;
    }

    /* PUT /mirror with JSON body containing the source URL */
    char *escaped_url = json_escape_string(source_url);
    if (!escaped_url) {
        curl_easy_cleanup(curl);
        free(url);
        free(auth_header);
        return HANAMI_ERR_NOMEM;
    }
    
    size_t body_len = 9 + strlen(escaped_url) + 2; /* {"url":""} + NUL */
    char *body = malloc(body_len);
    if (!body) {
        free(escaped_url);
        curl_easy_cleanup(curl);
        free(url);
        free(auth_header);
        return HANAMI_ERR_NOMEM;
    }
    snprintf(body, body_len, "{\"url\":\"%s\"}", escaped_url);
    free(escaped_url);

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    dyn_buf_t resp;
    dyn_buf_init(&resp);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    free(url);
    free(auth_header);
    free(body);

    if (res != CURLE_OK) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        dyn_buf_free(&resp);
        return curl_code_to_error(res);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    err = http_status_to_error(status);
    if (err != HANAMI_OK) {
        dyn_buf_free(&resp);
        return err;
    }

    if (out_desc && resp.data)
        *out_desc = parse_blob_descriptor((const char *)resp.data);

    dyn_buf_free(&resp);
    return HANAMI_OK;
}
