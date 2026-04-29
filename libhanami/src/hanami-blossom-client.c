/*
 * hanami-blossom-client.c - Blossom HTTP client (BUD-01/BUD-02)
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-blossom-client.h"
#include "hanami/hanami-bud02-auth.h"
#include "hanami/hanami-index.h"
#include "nostr-event.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

/* =========================================================================
 * Internal structures
 * ========================================================================= */

struct hanami_blossom_client {
    char *endpoint;         /* Owned, no trailing slash */
    char *user_agent;       /* Owned */
    long timeout;           /* Seconds */
    hanami_signer_t signer; /* Copied at creation */
    bool has_signer;
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

/* Find a JSON string value for a key. Returns malloc'd string or NULL. */
static char *json_get_string(const char *json, const char *key)
{
    if (!json || !key) return NULL;

    /* Search for "key": */
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;

    p += strlen(search);
    /* Skip whitespace and colon */
    while (*p && (*p == ' ' || *p == '\t' || *p == ':'))
        p++;
    if (*p != '"') return NULL;
    p++; /* skip opening quote */

    const char *end = p;
    while (*end && *end != '"') {
        if (*end == '\\') end++; /* skip escaped char */
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

    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return 0;

    p += strlen(search);
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

    *out = c;
    return HANAMI_OK;
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

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
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
    char body[2048];
    snprintf(body, sizeof(body), "{\"url\":\"%s\"}", source_url);

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
