/*
 * client.c - Blossom HTTP client (BUD-01/BUD-02)
 *
 * SPDX-License-Identifier: MIT
 */

#include "blossom/client.h"
#include "blossom/shim.h"
#include <nostr/bud02/auth.h>

#include "blossom/capability.h"
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
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <jansson.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <ctype.h>
#include "internal.h"

/* =========================================================================
 * Internal structures
 * ========================================================================= */

struct blossom_client {
    char *endpoint;         /* Owned, no trailing slash */
    char *user_agent;       /* Owned */
    long timeout;           /* Seconds */
    size_t max_blob_bytes;
    bool allow_insecure;
    blossom_transport_t transport;
    bool proxy_connection;
    struct curl_slist *dns_pins;
    blossom_error_t setup_error;
    long http_status;
    char http_reason[201];
    const char *upload_auth; /* Borrowed only during put_signed(). */
    char upload_content_type[BLOSSOM_PREFERRED_CT_MAX];
    blossom_signer_t signer; /* Copied at creation */
    bool has_signer;
    /* Session-scoped per-server capability record (nostrc-xeby / ypn2).
     * Not persisted; lives for the lifetime of this client handle. */
    blossom_server_capabilities_t caps;
};

static bool valid_hex(const char *s) {
    if (!s || strlen(s) != 64) return false;
    for (size_t i = 0; i < 64; i++) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

static bool public_ipv4(const uint8_t *b) {
    return !(b[0] == 0 || b[0] == 10 || b[0] == 127 || b[0] >= 224 ||
        (b[0] == 169 && b[1] == 254) || (b[0] == 172 && (b[1] & 0xf0) == 16) ||
        (b[0] == 192 && b[1] == 168) || (b[0] == 100 && (b[1] & 0xc0) == 64) ||
        (b[0] == 192 && b[1] == 0 && b[2] == 0) || (b[0] == 198 && (b[1] & 0xfe) == 18));
}
static bool public_ipv6(const uint8_t *b) {
    static const uint8_t zero[12] = {0};
    if (!memcmp(b, zero, 10) && ((!b[10] && !b[11]) || (b[10] == 255 && b[11] == 255)))
        return public_ipv4(b + 12);
    if (!memcmp(b, zero, 8) && b[8] == 255 && b[9] == 255 && !b[10] && !b[11])
        return public_ipv4(b + 12);
    if (!memcmp(b, "\x00\x64\xff\x9b", 4)) {
        if (!memcmp(b + 4, zero, 8)) return public_ipv4(b + 12);
        if (b[4] || b[5] != 1 || b[8]) return false;
        static const uint8_t layouts[][5] = {{6,7,9,10,11}, {7,9,10,11,12}, {9,10,11,12,13}, {12,13,14,15,16}};
        bool any = false;
        for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
            bool fits = true;
            for (size_t j = layouts[i][4]; j < 16; j++) fits = fits && !b[j];
            if (!fits) continue;
            uint8_t v4[4] = {b[layouts[i][0]], b[layouts[i][1]], b[layouts[i][2]], b[layouts[i][3]]};
            if (!public_ipv4(v4)) return false;
            any = true;
        }
        return any;
    }
    if (b[0] == 0x20 && b[1] == 2) return public_ipv4(b + 2);
    if (b[0] == 0x20 && b[1] == 1 && !b[2] && !b[3]) {
        uint8_t client[4] = {b[12] ^ 255, b[13] ^ 255, b[14] ^ 255, b[15] ^ 255};
        return public_ipv4(b + 4) && public_ipv4(client);
    }
    if (!memcmp(b, "\x20\x01\x0d\xb8", 4)) return false;
    return (b[0] & 0xe0) == 0x20;
}
static bool public_address(const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) return public_ipv4((const uint8_t *)&((const struct sockaddr_in *)sa)->sin_addr);
    if (sa->sa_family == AF_INET6) return public_ipv6(((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr);
    return false;
}
blossom_error_t blossom_url_check(const char *url, bool insecure) {
    if (!url || !*url) return BLOSSOM_ERR_INVALID_ARG;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p <= 0x20 || *p == 0x7f || *p == '#' || *p == '?') return BLOSSOM_ERR_INVALID_ARG;
    CURLU *u = curl_url();
    if (!u) return BLOSSOM_ERR_NOMEM;
    char *scheme = NULL, *host = NULL, *user = NULL, *password = NULL;
    bool ok = curl_url_set(u, CURLUPART_URL, url, 0) == CURLUE_OK;
    ok = ok && curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
               curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK;
    if (ok && strcasecmp(scheme, "https") && !(insecure && !strcasecmp(scheme, "http"))) ok = false;
    if (curl_url_get(u, CURLUPART_USER, &user, 0) == CURLUE_OK ||
        curl_url_get(u, CURLUPART_PASSWORD, &password, 0) == CURLUE_OK) ok = false;
    if (ok && !insecure) {
        size_t n = strlen(host);
        if (!strcasecmp(host, "localhost") || !strcasecmp(host, "localhost.") ||
            (n > 10 && !strcasecmp(host + n - 10, ".localhost"))) ok = false;
        struct sockaddr_in v4 = { .sin_family = AF_INET };
        struct sockaddr_in6 v6 = { .sin6_family = AF_INET6 };
        if (inet_pton(AF_INET, host, &v4.sin_addr) == 1 && !public_address((struct sockaddr *)&v4)) ok = false;
        if (host[0] == '[' && n > 2) {
            char literal[INET6_ADDRSTRLEN];
            if (n - 2 >= sizeof literal) ok = false;
            else {
                memcpy(literal, host + 1, n - 2); literal[n - 2] = 0;
                if (inet_pton(AF_INET6, literal, &v6.sin6_addr) != 1 ||
                    !public_address((struct sockaddr *)&v6)) ok = false;
            }
        }
    }
    curl_free(scheme); curl_free(host); curl_free(user); curl_free(password); curl_url_cleanup(u);
    return ok ? BLOSSOM_OK : BLOSSOM_ERR_INVALID_ARG;
}
static curl_socket_t blossom_open_socket(void *data, curlsocktype purpose, struct curl_sockaddr *addr) {
    blossom_client_t *c = data;
    if (purpose != CURLSOCKTYPE_IPCXN || (!c->allow_insecure && !c->transport.allow_private_addresses &&
        !c->proxy_connection && !public_address(&addr->addr))) {
        c->setup_error = BLOSSOM_ERR_POLICY;
        return CURL_SOCKET_BAD;
    }
    return socket(addr->family, addr->socktype, addr->protocol);
}
static size_t discard_body(void *p, size_t size, size_t count, void *ctx) {
    (void)p; (void)ctx;
    return size && count > SIZE_MAX / size ? 0 : size * count;
}
blossom_error_t blossom_hash_blossom(const uint8_t *data, size_t len, char out[65]) {
    if ((!data && len) || !out) return BLOSSOM_ERR_INVALID_ARG;
    unsigned char digest[32];
    if (!SHA256(data, len, digest)) return BLOSSOM_ERR_NOMEM;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) { out[2*i] = hex[digest[i] >> 4]; out[2*i+1] = hex[digest[i] & 15]; }
    out[64] = 0;
    return BLOSSOM_OK;
}

/* Dynamic buffer for curl write callbacks */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t limit;
} dyn_buf_t;

static void dyn_buf_init(dyn_buf_t *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->limit = BLOSSOM_DEFAULT_MAX_BLOB_BYTES;
}

static void dyn_buf_free(dyn_buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->limit = BLOSSOM_DEFAULT_MAX_BLOB_BYTES;
}

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb,
                            void *userp)
{
    dyn_buf_t *b = (dyn_buf_t *)userp;
    if (size && nmemb > SIZE_MAX / size) return 0;
    size_t real_size = size * nmemb;
    if (b->len > b->limit || real_size > b->limit - b->len ||
        real_size > SIZE_MAX - b->len - 1) return 0;

    if (b->len + real_size + 1 > b->cap) {
        size_t new_cap = b->len + real_size + 1;
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
static char *build_url(const blossom_client_t *c, const char *path)
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

static NostrEvent *sign_event_via_client(const blossom_client_t *, NostrEvent *);

static blossom_error_t make_auth_header(const blossom_client_t *c,
                                       nostr_bud02_action_t action,
                                       const char *sha256_hex,
                                       char **out_header)
{
    if (c->upload_auth && action == NOSTR_BUD02_ACTION_UPLOAD) {
        NostrEvent *signed_ev = nostr_event_new();
        nostr_bud02_validate_options_t validation = { .expected_sha256 = sha256_hex };
        bool ok = signed_ev && nostr_event_deserialize_signed(signed_ev, c->upload_auth, NULL) == NOSTR_EVENT_VALIDATION_OK &&
            nostr_event_validate(signed_ev, NULL) == NOSTR_EVENT_VALIDATION_OK &&
            nostr_bud02_validate_auth_event(signed_ev, action, &validation) == NOSTR_BUD02_OK;
        char *value = ok ? nostr_bud02_create_auth_header(signed_ev) : NULL;
        nostr_event_free(signed_ev);
        if (!value) return BLOSSOM_ERR_AUTH;
        /* BUD-11: unpadded base64url for media upload authorization. */
        for (char *p = value; *p; p++) {
            if (*p == '+') *p = '-';
            else if (*p == '/') *p = '_';
            else if (*p == '=') { *p = 0; break; }
        }
        size_t n = strlen(value) + 16;
        *out_header = malloc(n);
        if (*out_header) snprintf(*out_header, n, "Authorization: %s", value);
        free(value);
        return *out_header ? BLOSSOM_OK : BLOSSOM_ERR_NOMEM;
    }
    if (!c->has_signer)
        return BLOSSOM_ERR_AUTH;

    NostrEvent *ev = nostr_bud02_create_auth_event(action, sha256_hex, 0,
        c->caps.server_tag_ok == BLOSSOM_CAP_YES ? c->endpoint : NULL);
    if (!ev) return BLOSSOM_ERR_NOSTR;
    NostrEvent *signed_ev = sign_event_via_client(c, ev);
    nostr_event_free(ev);
    if (!signed_ev) return BLOSSOM_ERR_AUTH;

    nostr_bud02_validate_options_t validation = {
        .expected_sha256 = sha256_hex, .expected_pubkey = c->signer.pubkey
    };
    if (nostr_bud02_validate_auth_event(signed_ev, action, &validation) != NOSTR_BUD02_OK) {
        nostr_event_free(signed_ev); return BLOSSOM_ERR_AUTH;
    }
    char *header_val = nostr_bud02_create_auth_header(signed_ev);
    nostr_event_free(signed_ev);
    if (!header_val)
        return BLOSSOM_ERR_AUTH;

    /* Format as "Authorization: Nostr <base64>" */
    size_t hlen = 16 + strlen(header_val) + 1; /* "Authorization: " + value + NUL */
    char *full_header = malloc(hlen);
    if (!full_header) {
        free(header_val);
        return BLOSSOM_ERR_NOMEM;
    }
    snprintf(full_header, hlen, "Authorization: %s", header_val);
    free(header_val);

    *out_header = full_header;
    return BLOSSOM_OK;
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
        else if ((unsigned char)*p <= 0x1F)
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
        } else if ((unsigned char)*p <= 0x1F) {
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

static blossom_blob_descriptor_t *parse_blob_descriptor(const char *text)
{
    json_t *obj = text ? json_loads(text, JSON_REJECT_DUPLICATES, NULL) : NULL;
    if (!json_is_object(obj)) { json_decref(obj); return NULL; }
    const char *hash = json_string_value(json_object_get(obj, "sha256"));
    json_int_t size = json_integer_value(json_object_get(obj, "size"));
    if (!valid_hex(hash) || !json_is_integer(json_object_get(obj, "size")) || size < 0) {
        json_decref(obj); return NULL;
    }
    blossom_blob_descriptor_t *d = calloc(1, sizeof(*d));
    if (d) {
        memcpy(d->sha256, hash, 65); d->size = (size_t)size;
        d->uploaded = json_integer_value(json_object_get(obj, "uploaded"));
        const char *url = json_string_value(json_object_get(obj, "url"));
        const char *type = json_string_value(json_object_get(obj, "type"));
        if (url) d->url = strdup(url);
        if (type) d->mime_type = strdup(type);
    }
    json_decref(obj); return d;
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

blossom_error_t blossom_client_new(const blossom_client_opts_t *opts,
                                         const blossom_signer_t *signer,
                                         blossom_client_t **out)
{
    if (!out) return BLOSSOM_ERR_INVALID_ARG;
    *out = NULL;
    if (!opts || !opts->endpoint) return BLOSSOM_ERR_INVALID_ARG;
    bool relaxed = opts->allow_insecure || (opts->transport &&
        (opts->transport->allow_http || opts->transport->allow_private_addresses));
    blossom_error_t url_error = blossom_url_check(opts->endpoint, relaxed);
    if (url_error != BLOSSOM_OK) return url_error;

    blossom_client_t *c = calloc(1, sizeof(*c));
    if (!c)
        return BLOSSOM_ERR_NOMEM;

    c->endpoint = strdup(opts->endpoint);
    if (!c->endpoint) {
        free(c);
        return BLOSSOM_ERR_NOMEM;
    }
    strip_trailing_slash(c->endpoint);

    c->user_agent = strdup(opts->user_agent ? opts->user_agent : "libhanami/0.1");
    if (!c->user_agent) {
        free(c->endpoint);
        free(c);
        return BLOSSOM_ERR_NOMEM;
    }

    c->timeout = (opts->timeout_seconds > 0) ? opts->timeout_seconds : 30;
    c->max_blob_bytes = opts->max_blob_bytes ? opts->max_blob_bytes : BLOSSOM_DEFAULT_MAX_BLOB_BYTES;
    c->allow_insecure = opts->allow_insecure;
    if (opts->transport) c->transport = *opts->transport;

    if (signer && signer->sign) {
        c->signer = *signer; /* shallow copy — caller must keep pointers valid */
        c->has_signer = true;
    }

    blossom_server_capabilities_init(&c->caps);
    (void)blossom_capability_cache_get(c->endpoint, &c->caps);

    *out = c;
    return BLOSSOM_OK;
}

const blossom_server_capabilities_t *
blossom_client_get_capabilities(blossom_client_t *client)
{
    if (client && client->upload_content_type[0])
        memcpy(client->caps.preferred_content_type, client->upload_content_type, BLOSSOM_PREFERRED_CT_MAX);
    return client ? &client->caps : NULL;
}

blossom_error_t
blossom_client_set_upload_content_type(blossom_client_t *client,
                                              const char *content_type)
{
    if (!client) return BLOSSOM_ERR_INVALID_ARG;
    if (!content_type || !*content_type) {
        client->upload_content_type[0] = '\0';
        client->caps.preferred_content_type[0] = '\0';
        return BLOSSOM_OK;
    }
    size_t n = strlen(content_type);
    if (n >= BLOSSOM_PREFERRED_CT_MAX) return BLOSSOM_ERR_INVALID_ARG;
    /* Reject CR/LF to prevent header injection. */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)content_type[i];
        if (c == '\r' || c == '\n' || c == '\0') return BLOSSOM_ERR_INVALID_ARG;
    }
    memcpy(client->upload_content_type, content_type, n + 1);
    memcpy(client->caps.preferred_content_type, content_type, n + 1);
    return BLOSSOM_OK;
}

/* Resolve the Content-Type value to send on PUT /upload. Precedence:
 *   1. c->caps.preferred_content_type (per-server override; empty = skip)
 *   2. env BLOSSOM_UPLOAD_CT_ENV (lab / operator override)
 *   3. BLOSSOM_UPLOAD_CONTENT_TYPE (compile-time default)
 * Returns a pointer into a caller-supplied 128-byte buffer. Never NULL.
 * The buffer is used only when the env value is chosen; static strings
 * are returned directly when they fit. Any embedded CR/LF or NUL in the
 * env value is treated as "not set" (defence-in-depth against a malicious
 * env override).
 */
static const char *resolve_upload_content_type(const blossom_client_t *c,
                                               char buf[BLOSSOM_PREFERRED_CT_MAX])
{
    if (c && c->upload_content_type[0] != '\0') {
        memcpy(buf, c->upload_content_type, BLOSSOM_PREFERRED_CT_MAX);
        return buf;
    }
    if (c && c->caps.preferred_content_type[0] != '\0') {
        /* Per-server override wins; copy into caller buf for uniformity. */
        size_t n = strnlen(c->caps.preferred_content_type,
                           BLOSSOM_PREFERRED_CT_MAX);
        if (n > 0 && n < BLOSSOM_PREFERRED_CT_MAX) {
            memcpy(buf, c->caps.preferred_content_type, n);
            buf[n] = '\0';
            return buf;
        }
    }
    const char *env = getenv(BLOSSOM_UPLOAD_CT_ENV);
    if (env && *env) {
        size_t n = strnlen(env, BLOSSOM_PREFERRED_CT_MAX);
        if (n > 0 && n < BLOSSOM_PREFERRED_CT_MAX) {
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
    return BLOSSOM_UPLOAD_CONTENT_TYPE;
}

/* Format a full "Content-Type: <value>" header string into `out` of size
 * `cap`. Returns 0 on success, -1 if truncated. */
static int format_content_type_header(const blossom_client_t *c,
                                      char *out, size_t cap)
{
    char ctbuf[BLOSSOM_PREFERRED_CT_MAX];
    const char *ct = resolve_upload_content_type(c, ctbuf);
    int n = snprintf(out, cap, "Content-Type: %s", ct);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

void blossom_client_free(blossom_client_t *client)
{
    if (!client) return;
    free(client->endpoint);
    free(client->user_agent);
    curl_slist_free_all(client->dns_pins);
    free(client);
}

/* =========================================================================
 * Common curl setup
 * ========================================================================= */

static int transfer_progress(void *data, curl_off_t a, curl_off_t b, curl_off_t d, curl_off_t e)
{
    (void)a; (void)b; (void)d; (void)e;
    blossom_client_t *c = data;
    return c->transport.cancelled && c->transport.cancelled(c->transport.user_data);
}

static CURLcode private_tls_context(CURL *curl, void *ctx, void *data)
{
    (void)curl; (void)data;
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    return CURLE_OK;
}

long blossom_client_http_status(const blossom_client_t *c) { return c ? c->http_status : 0; }
const char *blossom_client_http_reason(const blossom_client_t *c) { return c ? c->http_reason : ""; }
static size_t response_header(char *line, size_t size, size_t count, void *data)
{
    blossom_client_t *c = data;
    if (size && count > SIZE_MAX / size) return 0;
    size_t n = size * count;
    if (n > 5 && !strncasecmp(line, "HTTP/", 5)) {
        const char *space = memchr(line, ' ', n);
        c->http_status = space && line + n - space >= 4 &&
            isdigit((unsigned char)space[1]) && isdigit((unsigned char)space[2]) && isdigit((unsigned char)space[3])
            ? (space[1] - '0') * 100 + (space[2] - '0') * 10 + space[3] - '0' : 0;
        c->http_reason[0] = 0;
    } else if (n > 9 && !strncasecmp(line, "X-Reason:", 9)) {
        size_t j = 0;
        for (size_t i = 9; i < n && j < sizeof c->http_reason - 1; i++) {
            unsigned char ch = (unsigned char)line[i];
            if (ch == '\r' || ch == '\n') break;
            if (ch >= 32 && ch < 127) c->http_reason[j++] = (char)ch;
        }
        c->http_reason[j] = 0;
    }
    return n;
}

static CURL *setup_curl(blossom_client_t *c, const char *url)
{
    c->setup_error = BLOSSOM_ERR_POLICY;
    c->http_status = 0; c->http_reason[0] = 0;
    if (transfer_progress(c, 0, 0, 0, 0)) { c->setup_error = BLOSSOM_ERR_CANCELLED; return NULL; }
    bool http = !strncasecmp(url, "http://", 7);
    if (blossom_url_check(url, c->allow_insecure || c->transport.allow_http ||
                          c->transport.allow_private_addresses) != BLOSSOM_OK ||
        (http && !c->allow_insecure && !c->transport.allow_http)) return NULL;
    if (!c->allow_insecure && !c->transport.allow_private_addresses && !http &&
        blossom_url_check(url, false) != BLOSSOM_OK) return NULL;
    char *proxy = NULL, *addresses = NULL;
    curl_slist_free_all(c->dns_pins); c->dns_pins = NULL;
    if (c->transport.prepare) {
        c->setup_error = c->transport.prepare(url, &proxy, &addresses, c->transport.user_data);
        if (c->setup_error != BLOSSOM_OK) { free(proxy); free(addresses); return NULL; }
    }
    if (addresses && *addresses && (!proxy || !*proxy)) {
        CURLU *u = curl_url();
        char *host = NULL, *port = NULL;
        if (u && curl_url_set(u, CURLUPART_URL, url, 0) == CURLUE_OK &&
            curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
            curl_url_get(u, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) == CURLUE_OK) {
            size_t n = strlen(host) + strlen(port) + strlen(addresses) + 3;
            char *record = malloc(n);
            if (record) {
                snprintf(record, n, "%s:%s:%s", host, port, addresses);
                c->dns_pins = curl_slist_append(NULL, record);
                free(record);
            }
        }
        curl_free(host); curl_free(port); curl_url_cleanup(u);
        if (!c->dns_pins) { free(proxy); free(addresses); c->setup_error = BLOSSOM_ERR_NOMEM; return NULL; }
    }
    free(addresses);
    c->proxy_connection = proxy && *proxy;
    if (c->transport.disable_tls_sessions && !http) {
        const char *backend = curl_version_info(CURLVERSION_NOW)->ssl_version;
        if (!backend || strncmp(backend, "OpenSSL/", 8)) {
            free(proxy); c->setup_error = BLOSSOM_ERR_UNSUPPORTED; return NULL;
        }
    }
    CURL *curl = curl_easy_init();
    c->setup_error = BLOSSOM_ERR_NOMEM;
    if (!curl) { free(proxy); return NULL; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (c->dns_pins) curl_easy_setopt(curl, CURLOPT_RESOLVE, c->dns_pins);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, c->timeout);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, c->user_agent);
    /* No redirect/proxy may bypass destination checks or leak auth. */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, (c->allow_insecure || c->transport.allow_http) ? "http,https" : "https");
    /* Required route controls must not silently fail open. */
    if (curl_easy_setopt(curl, CURLOPT_PROXY, proxy ? proxy : "") != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "") != CURLE_OK) {
        free(proxy); curl_easy_cleanup(curl); c->setup_error = BLOSSOM_ERR_UNSUPPORTED; return NULL;
    }
    free(proxy);
    curl_easy_setopt(curl, CURLOPT_SSL_SESSIONID_CACHE, 0L);
    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
    if (c->transport.disable_tls_sessions && !http &&
        curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, private_tls_context) != CURLE_OK) {
        curl_easy_cleanup(curl); c->setup_error = BLOSSOM_ERR_UNSUPPORTED; return NULL;
    }
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, c);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, blossom_open_socket);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, (void *)c);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, response_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, c);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)c->max_blob_bytes);

    return curl;
}

static blossom_error_t curl_code_to_error(CURLcode code)
{
    switch (code) {
    case CURLE_OK:               return BLOSSOM_OK;
    case CURLE_ABORTED_BY_CALLBACK: return BLOSSOM_ERR_CANCELLED;
    case CURLE_WRITE_ERROR:
    case CURLE_FILESIZE_EXCEEDED: return BLOSSOM_ERR_TOO_LARGE;
    case CURLE_OPERATION_TIMEDOUT: return BLOSSOM_ERR_TIMEOUT;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:  return BLOSSOM_ERR_NETWORK;
    default:                     return BLOSSOM_ERR_NETWORK;
    }
}

static blossom_error_t transfer_error(blossom_client_t *c, CURLcode code)
{
    if (code != CURLE_OK && c->setup_error == BLOSSOM_ERR_POLICY) return BLOSSOM_ERR_POLICY;
    return curl_code_to_error(code);
}

static blossom_error_t http_status_to_error(long status)
{
    if (status >= 200 && status < 300) return BLOSSOM_OK;
    if (status == 401 || status == 403) return BLOSSOM_ERR_AUTH;
    if (status == 404) return BLOSSOM_ERR_NOT_FOUND;
    if (status == 413) return BLOSSOM_ERR_TOO_LARGE;
    if (status >= 400 && status < 500) return BLOSSOM_ERR_BLOSSOM;
    return BLOSSOM_ERR_NETWORK;
}

/* =========================================================================
 * BUD-01: GET
 * ========================================================================= */

blossom_error_t blossom_client_get(blossom_client_t *client,
                                  const char *sha256_hex,
                                  uint8_t **out_data,
                                  size_t *out_len)
{
    if (!client || !valid_hex(sha256_hex) || !out_data || !out_len)
        return BLOSSOM_ERR_INVALID_ARG;

    *out_data = NULL;
    *out_len = 0;

    char *url = build_url(client, sha256_hex);
    if (!url)
        return BLOSSOM_ERR_NOMEM;

    CURL *curl = setup_curl(client, url);
    if (!curl) {
        free(url);
        return client->setup_error;
    }

    dyn_buf_t buf;
    dyn_buf_init(&buf);
    buf.limit = client->max_blob_bytes;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    char *auth = NULL;
    struct curl_slist *headers = NULL;
    if (client->has_signer) {
        blossom_error_t ae = make_auth_header(client, NOSTR_BUD02_ACTION_GET, sha256_hex, &auth);
        if (ae != BLOSSOM_OK) { free(url); curl_easy_cleanup(curl); return ae; }
        headers = curl_slist_append(NULL, auth);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers); free(auth);
    free(url);

    if (res != CURLE_OK) {
        dyn_buf_free(&buf);
        curl_easy_cleanup(curl);
        return transfer_error(client, res);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    blossom_error_t err = http_status_to_error(status);
    if (err != BLOSSOM_OK) {
        dyn_buf_free(&buf);
        return err;
    }

    char actual[65];
    if (blossom_hash_blossom(buf.data, buf.len, actual) != BLOSSOM_OK ||
        strcasecmp(actual, sha256_hex)) {
        dyn_buf_free(&buf);
        return BLOSSOM_ERR_HASH_MISMATCH;
    }
    *out_data = buf.data;
    *out_len = buf.len;
    return BLOSSOM_OK;
}

blossom_error_t blossom_client_get_url(blossom_client_t *c, const char *url,
    const char *hash, uint8_t **out_data, size_t *out_len)
{
    if (!c || !url || !valid_hex(hash) || !out_data || !out_len) return BLOSSOM_ERR_INVALID_ARG;
    *out_data = NULL; *out_len = 0;
    char *current = strdup(url);
    if (!current) return BLOSSOM_ERR_NOMEM;
    for (unsigned hop = 0; ; hop++) {
        CURL *curl = setup_curl(c, current);
        free(current); current = NULL;
        if (!curl) return c->setup_error;
        dyn_buf_t buf; dyn_buf_init(&buf); buf.limit = c->max_blob_bytes;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        CURLcode rc = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (rc == CURLE_OK && status >= 300 && status < 400 && hop < 3) {
            char *next = NULL;
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &next);
            if (next) current = strdup(next);
        }
        curl_easy_cleanup(curl);
        if (current) { dyn_buf_free(&buf); continue; }
        blossom_error_t error = rc == CURLE_OK ? http_status_to_error(status) :
            (c->setup_error == BLOSSOM_ERR_POLICY ? BLOSSOM_ERR_POLICY : curl_code_to_error(rc));
        if (error == BLOSSOM_OK) {
            char actual[65];
            error = blossom_hash_blossom(buf.data, buf.len, actual);
            if (error == BLOSSOM_OK && strcasecmp(actual, hash)) error = BLOSSOM_ERR_HASH_MISMATCH;
        }
        if (error != BLOSSOM_OK) { dyn_buf_free(&buf); return error; }
        *out_data = buf.data; *out_len = buf.len;
        return BLOSSOM_OK;
    }
}

/* =========================================================================
 * BUD-01: HEAD
 * ========================================================================= */

blossom_error_t blossom_client_head(blossom_client_t *client,
                                   const char *sha256_hex,
                                   bool *out_exists)
{
    if (!client || !valid_hex(sha256_hex) || !out_exists)
        return BLOSSOM_ERR_INVALID_ARG;

    *out_exists = false;

    char *url = build_url(client, sha256_hex);
    if (!url)
        return BLOSSOM_ERR_NOMEM;

    CURL *curl = setup_curl(client, url);
    if (!curl) {
        free(url);
        return client->setup_error;
    }

    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); /* HEAD request */

    char *auth = NULL;
    struct curl_slist *headers = NULL;
    if (client->has_signer) {
        blossom_error_t ae = make_auth_header(client, NOSTR_BUD02_ACTION_GET, sha256_hex, &auth);
        if (ae != BLOSSOM_OK) { free(url); curl_easy_cleanup(curl); return ae; }
        headers = curl_slist_append(NULL, auth);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers); free(auth);
    free(url);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return transfer_error(client, res);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    *out_exists = (status >= 200 && status < 300);
    return status == 404 ? BLOSSOM_OK : http_status_to_error(status);
}

/* =========================================================================
 * BUD-02: PUT /upload
 * ========================================================================= */

blossom_error_t blossom_put_wire(blossom_client_t *client,
                                     const uint8_t *data,
                                     size_t len,
                                     const char *sha256_hex,
                                     blossom_blob_descriptor_t **out_desc)
{
    if (!client || (!data && len)) return BLOSSOM_ERR_INVALID_ARG;
    if (out_desc) *out_desc = NULL;
    if (len > client->max_blob_bytes ||
        (client->caps.max_size && len > client->caps.max_size)) return BLOSSOM_ERR_TOO_LARGE;
    if (!client->has_signer && !client->upload_auth)
        return BLOSSOM_ERR_AUTH;

    char hash_buf[65];
    blossom_error_t hash_error = blossom_hash_blossom(data, len, hash_buf);
    if (hash_error != BLOSSOM_OK) return hash_error;
    if (sha256_hex && (!valid_hex(sha256_hex) || strcasecmp(sha256_hex, hash_buf)))
        return BLOSSOM_ERR_HASH_MISMATCH;
    sha256_hex = hash_buf;

    /* Create auth header */
    char *auth_header = NULL;
    blossom_error_t err = make_auth_header(client, NOSTR_BUD02_ACTION_UPLOAD,
                                          sha256_hex, &auth_header);
    if (err != BLOSSOM_OK) return err;

    char *url = build_url(client, "upload");
    if (!url) {
        free(auth_header);
        return BLOSSOM_ERR_NOMEM;
    }

    CURL *curl = setup_curl(client, url);
    if (!curl) {
        free(url);
        free(auth_header);
        return client->setup_error;
    }

    /* PUT with binary body.
     * Use CUSTOMREQUEST + POSTFIELDS (not CURLOPT_UPLOAD which expects
     * a read callback and conflicts with POSTFIELDS). */
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);

    char cth[BLOSSOM_PREFERRED_CT_MAX + 16];
    if (format_content_type_header(client, cth, sizeof cth) != 0) {
        /* Should be unreachable — resolver-produced values are bounded
         * by BLOSSOM_PREFERRED_CT_MAX; fall back to the compile-time default. */
        snprintf(cth, sizeof cth, "Content-Type: %s",
                 BLOSSOM_UPLOAD_CONTENT_TYPE);
    }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, cth);
    char hash_header[80];
    snprintf(hash_header, sizeof hash_header, "X-SHA-256: %s", sha256_hex);
    headers = curl_slist_append(headers, hash_header);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    dyn_buf_t resp;
    dyn_buf_init(&resp);
    resp.limit = 64u * 1024u;
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)resp.limit);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    free(url);
    free(auth_header);

    if (res != CURLE_OK) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        dyn_buf_free(&resp);
        return transfer_error(client, res);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    err = http_status_to_error(status);
    if (err != BLOSSOM_OK) {
        dyn_buf_free(&resp);
        return err;
    }

    /* Parse blob descriptor from response */
    if (out_desc && !resp.data) { dyn_buf_free(&resp); return BLOSSOM_ERR_BLOSSOM; }
    if (out_desc) {
        *out_desc = parse_blob_descriptor((const char *)resp.data);
        if (!*out_desc || strcasecmp((*out_desc)->sha256, sha256_hex) || (*out_desc)->size != len) {
            blossom_blob_descriptor_free(*out_desc); *out_desc = NULL;
            dyn_buf_free(&resp); return BLOSSOM_ERR_HASH_MISMATCH;
        }
    }

    dyn_buf_free(&resp);
    return BLOSSOM_OK;
}

blossom_error_t blossom_client_put_signed(blossom_client_t *c, const uint8_t *data,
    size_t len, const char *hash, const char *signed_json, blossom_blob_descriptor_t **desc)
{
    if (!c || !valid_hex(hash) || !signed_json) return BLOSSOM_ERR_INVALID_ARG;
    c->upload_auth = signed_json;
    blossom_error_t result = blossom_put_wire(c, data, len, hash, desc);
    c->upload_auth = NULL;
    return result;
}

/* =========================================================================
 * BUD-02: DELETE
 * ========================================================================= */

blossom_error_t blossom_client_delete(blossom_client_t *client,
                                     const char *sha256_hex)
{
    if (!client || !valid_hex(sha256_hex))
        return BLOSSOM_ERR_INVALID_ARG;
    if (!client->has_signer)
        return BLOSSOM_ERR_AUTH;

    /* Create auth header */
    char *auth_header = NULL;
    blossom_error_t err = make_auth_header(client, NOSTR_BUD02_ACTION_DELETE,
                                          sha256_hex, &auth_header);
    if (err != BLOSSOM_OK) return err;

    char *url = build_url(client, sha256_hex);
    if (!url) {
        free(auth_header);
        return BLOSSOM_ERR_NOMEM;
    }

    CURL *curl = setup_curl(client, url);
    if (!curl) {
        free(url);
        free(auth_header);
        return client->setup_error;
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
        return transfer_error(client, res);
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

blossom_error_t blossom_client_list(blossom_client_t *client,
                                   const char *pubkey_hex,
                                   char **out_json,
                                   size_t *out_len)
{
    if (!client || !valid_hex(pubkey_hex) || !out_json || !out_len)
        return BLOSSOM_ERR_INVALID_ARG;

    *out_json = NULL;
    *out_len = 0;

    /* Build /list/<pubkey> URL */
    char path[140];
    snprintf(path, sizeof(path), "list/%s", pubkey_hex);

    char *url = build_url(client, path);
    if (!url)
        return BLOSSOM_ERR_NOMEM;

    CURL *curl = setup_curl(client, url);
    if (!curl) {
        free(url);
        return client->setup_error;
    }

    dyn_buf_t buf;
    dyn_buf_init(&buf);
    buf.limit = client->max_blob_bytes;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    char *auth = NULL;
    struct curl_slist *headers = NULL;
    if (client->has_signer) {
        blossom_error_t ae = make_auth_header(client, NOSTR_BUD02_ACTION_LIST, NULL, &auth);
        if (ae != BLOSSOM_OK) { free(url); curl_easy_cleanup(curl); return ae; }
        headers = curl_slist_append(NULL, auth);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers); free(auth);
    free(url);

    if (res != CURLE_OK) {
        dyn_buf_free(&buf);
        curl_easy_cleanup(curl);
        return transfer_error(client, res);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    blossom_error_t err = http_status_to_error(status);
    if (err != BLOSSOM_OK) {
        dyn_buf_free(&buf);
        return err;
    }

    *out_json = (char *)buf.data;
    *out_len = buf.len;
    return BLOSSOM_OK;
}

/* =========================================================================
 * Batch upload + capability probe (nostrc-xeby / nostrc-ypn2)
 * ========================================================================= */

/* Execute one PUT of `data` (len) to endpoint/upload with the given
 * "Nostr <b64>" header value (NOT the "Authorization:" prefix — this
 * helper adds it). Records HTTP status + blossom_error into out params.
 *
 * The sha256_hex parameter is currently only used for a future
 * per-blob path — Blossom BUD-02 accepts PUTs at /upload (not /<hash>);
 * the server derives the address from sha256(body).
 */
static void put_one_blob(blossom_client_t *c,
                         const char *sha256_hex,
                         const uint8_t *data, size_t len,
                         const char *auth_header_value,
                         long *out_status,
                         blossom_error_t *out_err)
{
    (void)sha256_hex;
    *out_status = 0;
    *out_err = BLOSSOM_ERR_NETWORK;

    char *url = build_url(c, "upload");
    if (!url) { *out_err = BLOSSOM_ERR_NOMEM; return; }

    CURL *curl = setup_curl(c, url);
    if (!curl) { free(url); *out_err = c->setup_error; return; }

    size_t hlen = 15 + 1 + strlen(auth_header_value) + 1; /* "Authorization: " + val + NUL */
    char *full = malloc(hlen);
    if (!full) { curl_easy_cleanup(curl); free(url); *out_err = BLOSSOM_ERR_NOMEM; return; }
    snprintf(full, hlen, "Authorization: %s", auth_header_value);

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);

    char cth[BLOSSOM_PREFERRED_CT_MAX + 16];
    if (format_content_type_header(c, cth, sizeof cth) != 0) {
        snprintf(cth, sizeof cth, "Content-Type: %s",
                 BLOSSOM_UPLOAD_CONTENT_TYPE);
    }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, full);
    headers = curl_slist_append(headers, cth);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    dyn_buf_t resp; dyn_buf_init(&resp);
    resp.limit = c->max_blob_bytes;
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
        *out_status = status; /* Retain any HTTP refusal even if sending also failed. */
    } else {
        *out_status = status;
        *out_err = http_status_to_error(status);
    }
}

/* Sign an unsigned event with the client's signer. Returns a NEW event
 * (parsed back from the signed JSON) or NULL. The input `ev` is not
 * modified in a durable way — caller still owns it. */
static NostrEvent *sign_event_via_client(const blossom_client_t *c,
                                          NostrEvent *ev)
{
    if (!c->has_signer) return NULL;
    nostr_event_set_pubkey(ev, c->signer.pubkey);
    char requested_id[65];
    if (nostr_event_compute_id(ev, requested_id) != NOSTR_EVENT_VALIDATION_OK) return NULL;

    char *unsigned_json = nostr_event_serialize_compact(ev);
    if (!unsigned_json) return NULL;

    char *signed_json = NULL;
    blossom_error_t err = c->signer.sign(unsigned_json, &signed_json,
                                         c->signer.user_data);
    free(unsigned_json);
    if (err != BLOSSOM_OK || !signed_json) {
        free(signed_json);
        return NULL;
    }

    NostrEvent *signed_ev = nostr_event_new();
    if (!signed_ev) { free(signed_json); return NULL; }
    bool valid = nostr_event_deserialize_signed(signed_ev, signed_json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
        nostr_event_check_signature(signed_ev) &&
        c->signer.pubkey && nostr_event_get_pubkey(signed_ev) &&
        !strcmp(c->signer.pubkey, nostr_event_get_pubkey(signed_ev)) &&
        nostr_event_get_kind(signed_ev) == NOSTR_BUD02_KIND;
    char *signed_id = valid ? nostr_event_get_id(signed_ev) : NULL;
    valid = signed_id && !strcmp(requested_id, signed_id);
    free(signed_id);
    free(signed_json);
    if (!valid) {
        nostr_event_free(signed_ev);
        return NULL;
    }
    return signed_ev;
}

/* Mint a batch auth header value ("Nostr <b64>") shared across N blobs.
 * server_url_or_null: pass NULL to OMIT the server tag (default). */
static char *mint_batch_auth_header(blossom_client_t *c,
                                    const char *const *hashes, size_t count,
                                    const char *server_url_or_null)
{
    NostrEvent *ev = nostr_bud02_create_batch_auth_event(
        NOSTR_BUD02_ACTION_UPLOAD, hashes, count, 0, server_url_or_null);
    if (!ev) return NULL;
    NostrEvent *signed_ev = sign_event_via_client(c, ev);
    nostr_event_free(ev);
    if (!signed_ev) return NULL;
    nostr_bud02_validate_options_t validation = { .expected_pubkey = c->signer.pubkey };
    /* The canonical id check above binds ALL requested x-tags, not just
     * the first x-tag checked by the codec's single-blob validator. */
    if (nostr_bud02_validate_auth_event(signed_ev, NOSTR_BUD02_ACTION_UPLOAD, &validation) != NOSTR_BUD02_OK) {
        nostr_event_free(signed_ev); return NULL;
    }
    char *hv = nostr_bud02_create_auth_header(signed_ev);
    nostr_event_free(signed_ev);
    return hv;
}

/* Mint a per-blob auth header value. Server tag omitted by default
 * (cross-server-safe posture; the legacy blossom_client_put keeps
 * its old behaviour of including the tag for backward compat). */
static char *mint_single_auth_header(blossom_client_t *c,
                                     const char *hash)
{
    NostrEvent *ev = nostr_bud02_create_auth_event(
        NOSTR_BUD02_ACTION_UPLOAD, hash, 0, NULL);
    if (!ev) return NULL;
    NostrEvent *signed_ev = sign_event_via_client(c, ev);
    nostr_event_free(ev);
    if (!signed_ev) return NULL;
    nostr_bud02_validate_options_t validation = { .expected_pubkey = c->signer.pubkey, .expected_sha256 = hash };
    if (nostr_bud02_validate_auth_event(signed_ev, NOSTR_BUD02_ACTION_UPLOAD, &validation) != NOSTR_BUD02_OK) {
        nostr_event_free(signed_ev); return NULL;
    }
    char *hv = nostr_bud02_create_auth_header(signed_ev);
    nostr_event_free(signed_ev);
    return hv;
}

blossom_error_t blossom_put_batch_wire(blossom_client_t *c,
                                           const blossom_blob_t *blobs,
                                           size_t count,
                                           blossom_batch_result_t *out_results)
{
    if (!c || !blobs || count == 0 || !out_results)
        return BLOSSOM_ERR_INVALID_ARG;
    /* Pre-zero results before any signer failure; pool consumers must never
     * mistake calloc's zero error code for a successful unattempted PUT. */
    for (size_t i = 0; i < count; i++) {
        out_results[i].http_status = 0;
        out_results[i].error = BLOSSOM_ERR_NETWORK;
        out_results[i].error_class = NULL;
    }

    if (!c->has_signer) {
        for (size_t i = 0; i < count; i++) {
            out_results[i].error = BLOSSOM_ERR_AUTH;
            out_results[i].error_class = "auth";
        }
        return BLOSSOM_ERR_AUTH;
    }

    /* Validate hashes up-front (avoids doing PUTs on a bad batch). */
    for (size_t i = 0; i < count; i++) {
        if (!valid_hex(blobs[i].sha256_hex)) return BLOSSOM_ERR_INVALID_ARG;
        if (blobs[i].len > c->max_blob_bytes ||
            (c->caps.max_size && blobs[i].len > c->caps.max_size)) return BLOSSOM_ERR_TOO_LARGE;
        char actual[65];
        if (blossom_hash_blossom(blobs[i].bytes, blobs[i].len, actual) != BLOSSOM_OK ||
            strcasecmp(actual, blobs[i].sha256_hex)) return BLOSSOM_ERR_HASH_MISMATCH;
        if (!blobs[i].bytes && blobs[i].len > 0)
            return BLOSSOM_ERR_INVALID_ARG;
    }

    /* Caller prepared bytes under a fresh observation. */

    /* UNKNOWN is not authority to share a batch header. */
    bool try_batch = (c->caps.batch_ok == BLOSSOM_CAP_YES);

    const char **hashes = NULL;
    char *batch_header = NULL;

    if (try_batch) {
        hashes = malloc(count * sizeof(*hashes));
        if (!hashes) return BLOSSOM_ERR_NOMEM;
        for (size_t i = 0; i < count; i++)
            hashes[i] = blobs[i].sha256_hex;

        const char *srv = (c->caps.server_tag_ok == BLOSSOM_CAP_YES)
                              ? c->endpoint : NULL;
        batch_header = mint_batch_auth_header(c, hashes, count, srv);
        if (!batch_header) {
            free(hashes);
            for (size_t i = 0; i < count; i++) {
                out_results[i].error = BLOSSOM_ERR_AUTH;
                out_results[i].error_class = "auth";
            }
            return BLOSSOM_ERR_AUTH;
        }
    }

    bool any_failed = false;
    blossom_error_t last_err = BLOSSOM_OK;
    bool fell_back_this_call = false;

    for (size_t i = 0; i < count; i++) {
        const blossom_blob_t *b = &blobs[i];
        long status = 0;
        blossom_error_t err = BLOSSOM_ERR_NETWORK;
        const char *cls = NULL;

        if (try_batch) {
            put_one_blob(c, b->sha256_hex, b->bytes, b->len,
                         batch_header, &status, &err);
            if (status == 401) {
                /* Definitive rejection of the batch header. Flip cache,
                 * fall back for THIS blob and every subsequent blob. */
                c->caps.batch_ok = BLOSSOM_CAP_NO;
                c->caps.last_401_ts = (int64_t)time(NULL);
                try_batch = false;
                fell_back_this_call = true;
                free(batch_header);
                batch_header = NULL;

                char *hdr = mint_single_auth_header(c, b->sha256_hex);
                if (!hdr) {
                    err = BLOSSOM_ERR_AUTH;
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
                err = BLOSSOM_ERR_AUTH;
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

        if (err == BLOSSOM_OK) {
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
        c->caps.batch_ok == BLOSSOM_CAP_UNKNOWN)
        c->caps.batch_ok = BLOSSOM_CAP_YES;

    blossom_capability_cache_set(c->endpoint, &c->caps);
    return any_failed ? last_err : BLOSSOM_OK;
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
                               nostr_bud02_action_t action)
{
    NostrEvent *ev = NULL;
    if (batch) {
        ev = nostr_bud02_create_batch_auth_event(
            action, hashes, count, 0, server_url_or_null);
    } else {
        ev = nostr_bud02_create_auth_event(
            action, hashes ? hashes[0] : NULL, 0, server_url_or_null);
    }
    if (!ev) return NULL;
    if (nostr_event_sign(ev, sk_hex) != 0) {
        nostr_event_free(ev);
        return NULL;
    }
    char *hv = nostr_bud02_create_auth_header(ev);
    nostr_event_free(ev);
    return hv;
}

/* Best-effort DELETE with the given "Nostr <b64>" header. Ignores errors. */
static void probe_delete_blob(blossom_client_t *c,
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

blossom_error_t blossom_probe_capabilities(blossom_client_t *c)
{
    if (!c) return BLOSSOM_ERR_INVALID_ARG;

    if (blossom_capability_cache_get(c->endpoint, &c->caps)) return BLOSSOM_OK;
    blossom_server_capabilities_init(&c->caps);
    c->caps.last_probe_ts = (int64_t)time(NULL);
    char *info_url = build_url(c, "");
    CURL *info_curl = info_url ? setup_curl(c, info_url) : NULL;
    if (info_curl) {
        dyn_buf_t info; dyn_buf_init(&info); info.limit = 65536;
        struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/json");
        curl_easy_setopt(info_curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(info_curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(info_curl, CURLOPT_WRITEDATA, &info);
        curl_easy_setopt(info_curl, CURLOPT_TIMEOUT, 5L);
        CURLcode rc = curl_easy_perform(info_curl);
        long st = 0; curl_easy_getinfo(info_curl, CURLINFO_RESPONSE_CODE, &st);
        if (rc == CURLE_OK && st >= 200 && st < 300 && info.data) {
            json_t *obj = json_loads((const char *)info.data, JSON_REJECT_DUPLICATES, NULL);
            json_t *priv = json_object_get(obj, "private");
            if (json_is_boolean(priv)) c->caps.private_server = json_is_true(priv) ? BLOSSOM_CAP_YES : BLOSSOM_CAP_NO;
            json_t *max = json_object_get(obj, "max_size");
            if (!max) max = json_object_get(obj, "maxSize");
            if (json_is_integer(max) && json_integer_value(max) > 0) c->caps.max_size = (uint64_t)json_integer_value(max);
            json_decref(obj);
        }
        dyn_buf_free(&info); curl_slist_free_all(headers); curl_easy_cleanup(info_curl);
    }
    free(info_url);

    /* Ephemeral secp256k1 keypair — never leaves this stack frame. */
    char *sk_hex = nostr_key_generate_private();
    if (!sk_hex) return BLOSSOM_ERR_NOSTR;

    /* Two 1 KiB session-random blobs. */
    uint8_t blob1[1024], blob2[1024];
    if (probe_random_bytes(blob1, sizeof blob1) != 0 ||
        probe_random_bytes(blob2, sizeof blob2) != 0) {
        free(sk_hex);
        return BLOSSOM_ERR_NETWORK;
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
                CURL *curl = setup_curl(c, url);
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

    if (c->caps.private_server == BLOSSOM_CAP_YES) {
        /* An ephemeral identity has no allowlist authority. Do not mislabel
         * its refusal as a batch/sniffer failure or probe with the real key. */
        OPENSSL_cleanse(sk_hex, strlen(sk_hex)); free(sk_hex);
        blossom_capability_cache_set(c->endpoint, &c->caps);
        return c->caps.reachable ? BLOSSOM_OK : BLOSSOM_ERR_NETWORK;
    }

    /* (b) server-tag tolerance — PUT h1 with single-x event that has
     * a `server` tag. 2xx -> YES; 401/403 -> NO; other 4xx -> UNKNOWN. */
    bool put_b_success = false;
    {
        const char *hh[1] = { h1 };
        char *hv = probe_mint_header(sk_hex, hh, 1, c->endpoint,
                                     false, NOSTR_BUD02_ACTION_UPLOAD);
        if (hv) {
            long st = 0; blossom_error_t err;
            put_one_blob(c, h1, blob1, sizeof blob1, hv, &st, &err);
            free(hv);
            if (st >= 200 && st < 300) {
                c->caps.server_tag_ok = BLOSSOM_CAP_YES;
                put_b_success = true;
            } else if (st == 401 || st == 403) {
                c->caps.server_tag_ok = BLOSSOM_CAP_NO;
            }
        }
    }

    /* (c) batch viability — one event with x=[h1,h2], PUT h1 then h2.
     * Any 401 -> NO; both 2xx -> YES; other 4xx -> UNKNOWN. */
    bool put_c_success_h1 = false, put_c_success_h2 = false;
    {
        const char *hh[2] = { h1, h2 };
        char *hv = probe_mint_header(sk_hex, hh, 2, NULL,
                                     true, NOSTR_BUD02_ACTION_UPLOAD);
        if (hv) {
            long st1 = 0, st2 = 0; blossom_error_t err;
            put_one_blob(c, h1, blob1, sizeof blob1, hv, &st1, &err);
            put_one_blob(c, h2, blob2, sizeof blob2, hv, &st2, &err);
            free(hv);
            bool any_401 = (st1 == 401 || st2 == 401);
            bool both_ok = (st1 >= 200 && st1 < 300) &&
                           (st2 >= 200 && st2 < 300);
            if (any_401) {
                c->caps.batch_ok = BLOSSOM_CAP_NO;
                c->caps.last_401_ts = (int64_t)time(NULL);
            } else if (both_ok) {
                c->caps.batch_ok = BLOSSOM_CAP_YES;
                put_c_success_h1 = true;
                put_c_success_h2 = true;
            }
        }
    }

    bool put_d_success = false;
    /* (d) strict-x binding — event with x=[h1], PUT blob2 (hash H2).
     * 401 -> STRICT; 2xx/non-auth 4xx -> PERMISSIVE. */
    {
        const char *hh[1] = { h1 };
        char *hv = probe_mint_header(sk_hex, hh, 1, NULL,
                                     false, NOSTR_BUD02_ACTION_UPLOAD);
        if (hv) {
            long st = 0; blossom_error_t err;
            put_one_blob(c, h2, blob2, sizeof blob2, hv, &st, &err);
            free(hv);
            if (st == 401 || st == 403) {
                c->caps.strict_x_binding = BLOSSOM_CAP_YES;
                c->caps.last_401_ts = (int64_t)time(NULL);
            } else if (st > 0) {
                c->caps.strict_x_binding = BLOSSOM_CAP_NO;
                put_d_success = st >= 200 && st < 300;
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
    uint8_t blob3[1024] = {0};
    char h3[65];
    bool put_e_success = false;
    bool random3_ok = probe_random_bytes(blob3, sizeof blob3) == 0;
    if (random3_ok) {
        probe_sha256_hex(blob3, sizeof blob3, h3);
        const char *hh[1] = { h3 };
        char *hv = probe_mint_header(sk_hex, hh, 1, NULL,
                                     false, NOSTR_BUD02_ACTION_UPLOAD);
        if (hv) {
            long st = 0; blossom_error_t err;
            put_one_blob(c, h3, blob3, sizeof blob3, hv, &st, &err);
            free(hv);
            if (st >= 200 && st < 300) {
                c->caps.raw_random_ok = BLOSSOM_CAP_YES;
                put_e_success = true;
            } else if (st == 400 || st == 415 || st == 422) {
                /* 4xx content-policy or 5xx decoder blowup — either way
                 * this server did not accept the raw random bytes. */
                c->caps.raw_random_ok = BLOSSOM_CAP_NO;
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
        size_t enc_len = blossom_shim_encoded_len(sizeof blob3);
        uint8_t *shim_buf = random3_ok ? (uint8_t *)malloc(enc_len) : NULL;
        if (shim_buf) {
            if (blossom_shim_wrap(blob3, sizeof blob3,
                                           shim_buf, enc_len) == BLOSSOM_OK) {
                probe_sha256_hex(shim_buf, enc_len, h_shim);
                const char *hh[1] = { h_shim };
                char *hv = probe_mint_header(sk_hex, hh, 1, NULL,
                                             false,
                                             NOSTR_BUD02_ACTION_UPLOAD);
                if (hv) {
                    /* Temporarily flip the client's preferred
                     * Content-Type to image/png so put_one_blob picks
                     * it up; restore afterwards. */
                    char saved_ct[BLOSSOM_PREFERRED_CT_MAX];
                    memcpy(saved_ct, c->upload_content_type,
                           sizeof saved_ct);
                    snprintf(c->upload_content_type, sizeof c->upload_content_type, "image/png");

                    long st = 0; blossom_error_t err;
                    put_one_blob(c, h_shim, shim_buf, enc_len, hv,
                                 &st, &err);
                    free(hv);

                    /* Restore whatever was there before (including
                     * "empty" == client default). */
                    memcpy(c->upload_content_type, saved_ct,
                           sizeof saved_ct);

                    if (st >= 200 && st < 300) {
                        c->caps.png_shim_ok = BLOSSOM_CAP_YES;
                        put_f_success = true;
                    } else if (st == 400 || st == 415 || st == 422) {
                        c->caps.png_shim_ok = BLOSSOM_CAP_NO;
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
                                     false, NOSTR_BUD02_ACTION_DELETE);
        if (hv) { probe_delete_blob(c, h1, hv); free(hv); }
    }
    if (put_c_success_h2 || put_d_success) {
        const char *hh[1] = { h2 };
        char *hv = probe_mint_header(sk_hex, hh, 1, NULL,
                                     false, NOSTR_BUD02_ACTION_DELETE);
        if (hv) { probe_delete_blob(c, h2, hv); free(hv); }
    }
    if (put_e_success) {
        const char *hh[1] = { h3 };
        char *hv = probe_mint_header(sk_hex, hh, 1, NULL,
                                     false, NOSTR_BUD02_ACTION_DELETE);
        if (hv) { probe_delete_blob(c, h3, hv); free(hv); }
    }
    if (put_f_success) {
        const char *hh[1] = { h_shim };
        char *hv = probe_mint_header(sk_hex, hh, 1, NULL,
                                     false, NOSTR_BUD02_ACTION_DELETE);
        if (hv) { probe_delete_blob(c, h_shim, hv); free(hv); }
    }

    OPENSSL_cleanse(sk_hex, strlen(sk_hex)); free(sk_hex);
    blossom_capability_cache_set(c->endpoint, &c->caps);
    return c->caps.reachable ? BLOSSOM_OK : BLOSSOM_ERR_NETWORK;
}

bool blossom_shim_active(void) { return false; }

bool blossom_shim_active_for_ex(const char *const *urls, size_t n,
    blossom_shim_reason_t *reason, char *who, size_t cap)
{
    if (reason) *reason = BLOSSOM_SHIM_REASON_AUTO_FALLBACK;
    if (who && cap) *who = 0;
    if (!urls || !n) return false;
    bool all_raw = true;
    for (size_t i = 0; i < n; i++) {
        blossom_server_capabilities_t caps = {0};
        if (!blossom_capability_cache_get(urls[i], &caps)) {
            blossom_client_t *c = NULL;
            blossom_client_opts_t opts = { .endpoint = urls[i], .timeout_seconds = 5 };
            if (blossom_client_new(&opts, NULL, &c) == BLOSSOM_OK) {
                (void)blossom_probe_capabilities(c); caps = c->caps;
                blossom_client_free(c);
            }
        }
        if (caps.raw_random_ok == BLOSSOM_CAP_NO && caps.png_shim_ok == BLOSSOM_CAP_YES) {
            if (reason) *reason = BLOSSOM_SHIM_REASON_AUTO_REQUIRED;
            if (who && cap) snprintf(who, cap, "%s", urls[i]);
            return true;
        }
        if (caps.raw_random_ok != BLOSSOM_CAP_YES) all_raw = false;
    }
    if (all_raw && reason) *reason = BLOSSOM_SHIM_REASON_AUTO_RAW_OK;
    return false;
}
bool blossom_shim_active_for(const char *const *urls, size_t n) {
    return blossom_shim_active_for_ex(urls, n, NULL, NULL, 0);
}

static blossom_error_t prepare_blob(blossom_client_t *c, const uint8_t *data,
    size_t len, uint8_t **owned, const uint8_t **wire, size_t *wire_len, char hash[65])
{
    *owned = NULL; *wire = data; *wire_len = len;
    if ((!data && len) || len > c->max_blob_bytes) return BLOSSOM_ERR_TOO_LARGE;
    if (!blossom_shim_detect(data, len) && c->caps.raw_random_ok == BLOSSOM_CAP_NO && c->caps.png_shim_ok == BLOSSOM_CAP_YES) {
        size_t n = blossom_shim_encoded_len(len);
        if (!n || n > c->max_blob_bytes) return BLOSSOM_ERR_TOO_LARGE;
        *owned = malloc(n);
        if (!*owned) return BLOSSOM_ERR_NOMEM;
        blossom_error_t e = blossom_shim_wrap(data, len, *owned, n);
        if (e != BLOSSOM_OK) { free(*owned); *owned = NULL; return e; }
        *wire = *owned; *wire_len = n;
    }
    if (c->caps.max_size && *wire_len > c->caps.max_size) {
        free(*owned); *owned = NULL; return BLOSSOM_ERR_TOO_LARGE;
    }
    return blossom_hash_blossom(*wire, *wire_len, hash);
}
blossom_error_t blossom_client_get_content(blossom_client_t *c, const char *hash, uint8_t **data, size_t *len) {
    blossom_error_t e = blossom_client_get(c, hash, data, len);
    if (e == BLOSSOM_OK && blossom_shim_detect(*data, *len)) {
        const uint8_t *view; size_t n;
        e = blossom_shim_unwrap(*data, *len, &view, &n);
        if (e == BLOSSOM_OK) { memmove(*data, view, n); *len = n; }
    }
    return e;
}
blossom_error_t blossom_client_address(blossom_client_t *c, const uint8_t *data, size_t len, char hash[65]) {
    if (!c || (!data && len) || !hash) return BLOSSOM_ERR_INVALID_ARG;
    if (len > c->max_blob_bytes) return BLOSSOM_ERR_TOO_LARGE;
    (void)blossom_probe_capabilities(c);
    uint8_t *owned; const uint8_t *wire; size_t n;
    blossom_error_t e = prepare_blob(c, data, len, &owned, &wire, &n, hash);
    free(owned); return e;
}
blossom_error_t blossom_client_put(blossom_client_t *c, const uint8_t *data,
    size_t len, const char *expected, blossom_blob_descriptor_t **desc)
{
    if (desc) *desc = NULL;
    if (!c || (!data && len)) return BLOSSOM_ERR_INVALID_ARG;
    if (len > c->max_blob_bytes) return BLOSSOM_ERR_TOO_LARGE;
    if (!c->has_signer) return BLOSSOM_ERR_AUTH;
    (void)blossom_probe_capabilities(c);
    uint8_t *owned = NULL; const uint8_t *wire; size_t wire_len; char hash[65];
    blossom_error_t e = prepare_blob(c, data, len, &owned, &wire, &wire_len, hash);
    if (e == BLOSSOM_OK && expected && (!valid_hex(expected) || strcasecmp(expected, hash)))
        e = BLOSSOM_ERR_HASH_MISMATCH;
    char ct[BLOSSOM_PREFERRED_CT_MAX]; memcpy(ct, c->upload_content_type, sizeof ct);
    if (blossom_shim_detect(wire, wire_len)) snprintf(c->upload_content_type, sizeof c->upload_content_type, "image/png");
    if (e == BLOSSOM_OK) e = blossom_put_wire(c, wire, wire_len, hash, desc);
    memcpy(c->upload_content_type, ct, sizeof ct); free(owned);
    return e;
}
blossom_error_t blossom_client_put_batch(blossom_client_t *c,
    const blossom_blob_t *blobs, size_t n, blossom_batch_result_t *results)
{
    if (!c || !blobs || !n || !results || n > SIZE_MAX / 65) return BLOSSOM_ERR_INVALID_ARG;
    for (size_t i = 0; i < n; i++) {
        results[i] = (blossom_batch_result_t){ .error = BLOSSOM_ERR_NETWORK };
        if ((!blobs[i].bytes && blobs[i].len) || (blobs[i].sha256_hex && !valid_hex(blobs[i].sha256_hex)))
            return BLOSSOM_ERR_INVALID_ARG;
        if (blobs[i].len > c->max_blob_bytes) return BLOSSOM_ERR_TOO_LARGE;
    }
    if (!c->has_signer) return BLOSSOM_ERR_AUTH;
    (void)blossom_probe_capabilities(c);
    blossom_blob_t *wire = calloc(n, sizeof(*wire));
    uint8_t **owned = calloc(n, sizeof(*owned));
    char (*hashes)[65] = calloc(n, sizeof(*hashes));
    if (!wire || !owned || !hashes) { free(wire); free(owned); free(hashes); return BLOSSOM_ERR_NOMEM; }
    blossom_error_t e = BLOSSOM_OK;
    for (size_t i = 0; i < n; i++) {
        e = prepare_blob(c, blobs[i].bytes, blobs[i].len, &owned[i], &wire[i].bytes, &wire[i].len, hashes[i]);
        if (e != BLOSSOM_OK) break;
        if (blobs[i].sha256_hex && strcasecmp(blobs[i].sha256_hex, hashes[i])) { e = BLOSSOM_ERR_HASH_MISMATCH; break; }
        wire[i].sha256_hex = hashes[i];
    }
    char ct[BLOSSOM_PREFERRED_CT_MAX]; memcpy(ct, c->upload_content_type, sizeof ct);
    if (blossom_shim_detect(wire[0].bytes, wire[0].len)) snprintf(c->upload_content_type, sizeof c->upload_content_type, "image/png");
    if (e == BLOSSOM_OK) e = blossom_put_batch_wire(c, wire, n, results);
    else for (size_t i = 0; i < n; i++) results[i].error = e;
    memcpy(c->upload_content_type, ct, sizeof ct);
    for (size_t i = 0; i < n; i++) free(owned[i]);
    free(wire); free(owned); free(hashes); return e;
}

/* =========================================================================
 * BUD-04: MIRROR
 * ========================================================================= */

blossom_error_t blossom_client_mirror(blossom_client_t *client,
                                     const char *source_url,
                                     blossom_blob_descriptor_t **out_desc)
{
    if (!client || !source_url) return BLOSSOM_ERR_INVALID_ARG;
    if (out_desc) *out_desc = NULL;
    if (blossom_url_check(source_url, client->allow_insecure) != BLOSSOM_OK)
        return BLOSSOM_ERR_INVALID_ARG;
    if (!client->has_signer)
        return BLOSSOM_ERR_AUTH;

    /* BUD-04 reuses an UPLOAD token bound to the origin's content hash,
     * not a separate t=mirror action. The URL may carry a file extension. */
    CURLU *origin = curl_url();
    char *path = NULL;
    if (!origin) return BLOSSOM_ERR_NOMEM;
    bool parsed = curl_url_set(origin, CURLUPART_URL, source_url, 0) == CURLUE_OK &&
        curl_url_get(origin, CURLUPART_PATH, &path, 0) == CURLUE_OK;
    const char *name = parsed ? strrchr(path, '/') : NULL;
    name = name ? name + 1 : NULL;
    char expected[65] = {0};
    if (name && strlen(name) >= 64) memcpy(expected, name, 64);
    bool hash_ok = valid_hex(expected) && (name[64] == '\0' || name[64] == '.');
    curl_free(path); curl_url_cleanup(origin);
    if (!hash_ok) return BLOSSOM_ERR_INVALID_ARG;
    char *auth_header = NULL;
    blossom_error_t err = make_auth_header(client, NOSTR_BUD02_ACTION_UPLOAD,
                                          expected, &auth_header);
    if (err != BLOSSOM_OK) return err;

    char *url = build_url(client, "mirror");
    if (!url) {
        free(auth_header);
        return BLOSSOM_ERR_NOMEM;
    }

    CURL *curl = setup_curl(client, url);
    if (!curl) {
        free(url);
        free(auth_header);
        return client->setup_error;
    }

    /* PUT /mirror with JSON body containing the source URL */
    char *escaped_url = json_escape_string(source_url);
    if (!escaped_url) {
        curl_easy_cleanup(curl);
        free(url);
        free(auth_header);
        return BLOSSOM_ERR_NOMEM;
    }
    
    size_t body_len = 9 + strlen(escaped_url) + 2; /* {"url":""} + NUL */
    char *body = malloc(body_len);
    if (!body) {
        free(escaped_url);
        curl_easy_cleanup(curl);
        free(url);
        free(auth_header);
        return BLOSSOM_ERR_NOMEM;
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
    resp.limit = 64u * 1024u;
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)resp.limit);
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
        return transfer_error(client, res);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    err = http_status_to_error(status);
    if (err != BLOSSOM_OK) {
        dyn_buf_free(&resp);
        return err;
    }

    blossom_blob_descriptor_t *desc = parse_blob_descriptor((const char *)resp.data);
    if (!desc || strcasecmp(desc->sha256, expected) || desc->size > client->max_blob_bytes) {
        blossom_blob_descriptor_free(desc); dyn_buf_free(&resp);
        return BLOSSOM_ERR_HASH_MISMATCH;
    }
    if (out_desc) *out_desc = desc;
    else blossom_blob_descriptor_free(desc);

    dyn_buf_free(&resp);
    return BLOSSOM_OK;
}
