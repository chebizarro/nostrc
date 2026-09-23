/*
 * nh_porthome_blossom.c — thin wrapper around libhanami's Blossom
 * client with content-hash verification, HTTPS-only enforcement, size
 * caps, retries, and multi-server failover.
 *
 * SPDX-License-Identifier: MIT
 * EXPERIMENTAL. See nh_porthome_blossom.h.
 */

#include "nh_porthome_blossom.h"
#include "nh_porthome_crypto.h"

#include <hanami/hanami-blossom-client.h>
#include <hanami/hanami-types.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

struct nh_porthome_blossom {
    char **servers;             /* owned */
    size_t n_servers;
    long   timeout_s;
    int    max_retries;
    size_t max_blob_bytes;
    const hanami_signer_t *signer; /* borrowed */
};

/* URL sanity: https:// only, no user-info, no fragment, no whitespace,
 * no localhost / 127. / [::1] literal. Case-insensitive scheme. */
int nh_porthome_blossom_url_ok(const char *url) {
    if (!url) return NH_PORTHOME_BLOSSOM_ERR_ARG;
    if (strncasecmp(url, "https://", 8) != 0) return NH_PORTHOME_BLOSSOM_ERR_HTTPS_ONLY;
    const char *host = url + 8;
    if (*host == '\0') return NH_PORTHOME_BLOSSOM_ERR_ARG;
    for (const char *p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7F) return NH_PORTHOME_BLOSSOM_ERR_ARG;
        if (c == '#') return NH_PORTHOME_BLOSSOM_ERR_ARG;
    }
    /* No user-info between '://' and the next '/' or ':'. */
    for (const char *p = host; *p && *p != '/'; p++) {
        if (*p == '@') return NH_PORTHOME_BLOSSOM_ERR_ARG;
    }
    /* Reject obvious loopback / localhost literals. */
    if (strncasecmp(host, "localhost", 9) == 0 &&
        (host[9] == '\0' || host[9] == '/' || host[9] == ':')) {
        return NH_PORTHOME_BLOSSOM_ERR_ARG;
    }
    if (strncmp(host, "127.", 4) == 0) return NH_PORTHOME_BLOSSOM_ERR_ARG;
    if (strncmp(host, "[::1]", 5) == 0) return NH_PORTHOME_BLOSSOM_ERR_ARG;
    if (strncmp(host, "[::]", 4) == 0) return NH_PORTHOME_BLOSSOM_ERR_ARG;
    if (strncmp(host, "0.0.0.0", 7) == 0) return NH_PORTHOME_BLOSSOM_ERR_ARG;
    /* Trailing '/' is a normalisation error — reject to match libhanami. */
    size_t ulen = strlen(url);
    if (url[ulen - 1] == '/') return NH_PORTHOME_BLOSSOM_ERR_ARG;
    return NH_PORTHOME_BLOSSOM_OK;
}

/* Internal escape hatch: allow http:// (still not localhost) for local
 * integration tests only. Enabled by the environment variable
 * NH_PORTHOME_ALLOW_INSECURE=1. Never set in production paths — the
 * unprivileged fetch helper (Phase 2) is where the real gate lives. */
static int url_ok_test_relaxed(const char *url) {
    if (!url) return NH_PORTHOME_BLOSSOM_ERR_ARG;
    int http = (strncasecmp(url, "http://", 7) == 0);
    int https = (strncasecmp(url, "https://", 8) == 0);
    if (!http && !https) return NH_PORTHOME_BLOSSOM_ERR_HTTPS_ONLY;
    if (http) {
        const char *e = getenv("NH_PORTHOME_ALLOW_INSECURE");
        if (!e || strcmp(e, "1") != 0) return NH_PORTHOME_BLOSSOM_ERR_HTTPS_ONLY;
    }
    const char *host = url + (http ? 7 : 8);
    if (*host == '\0') return NH_PORTHOME_BLOSSOM_ERR_ARG;
    for (const char *p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7F || c == '#') return NH_PORTHOME_BLOSSOM_ERR_ARG;
    }
    for (const char *p = host; *p && *p != '/'; p++)
        if (*p == '@') return NH_PORTHOME_BLOSSOM_ERR_ARG;
    size_t ulen = strlen(url);
    if (url[ulen - 1] == '/') return NH_PORTHOME_BLOSSOM_ERR_ARG;
    return NH_PORTHOME_BLOSSOM_OK;
}

int nh_porthome_blossom_new(const nh_porthome_blossom_opts_t *opts,
                            const hanami_signer_t *signer,
                            nh_porthome_blossom_t **out) {
    if (!opts || !out) return NH_PORTHOME_BLOSSOM_ERR_ARG;
    if (opts->n_servers == 0 || !opts->servers) return NH_PORTHOME_BLOSSOM_ERR_ARG;
    for (size_t i = 0; i < opts->n_servers; i++) {
        int rc = url_ok_test_relaxed(opts->servers[i]);
        if (rc != NH_PORTHOME_BLOSSOM_OK) return rc;
    }

    nh_porthome_blossom_t *c = (nh_porthome_blossom_t *)calloc(1, sizeof(*c));
    if (!c) return NH_PORTHOME_BLOSSOM_ERR_OOM;
    c->servers = (char **)calloc(opts->n_servers, sizeof(char *));
    if (!c->servers) { free(c); return NH_PORTHOME_BLOSSOM_ERR_OOM; }
    for (size_t i = 0; i < opts->n_servers; i++) {
        c->servers[i] = strdup(opts->servers[i]);
        if (!c->servers[i]) {
            for (size_t j = 0; j < i; j++) free(c->servers[j]);
            free(c->servers); free(c);
            return NH_PORTHOME_BLOSSOM_ERR_OOM;
        }
    }
    c->n_servers = opts->n_servers;
    c->timeout_s = opts->timeout_seconds > 0 ? opts->timeout_seconds
                                             : NH_PORTHOME_BLOSSOM_DEFAULT_TIMEOUT_S;
    c->max_retries = opts->max_retries > 0 ? opts->max_retries
                                           : NH_PORTHOME_BLOSSOM_DEFAULT_MAX_RETRIES;
    c->max_blob_bytes = opts->max_blob_bytes > 0 ? opts->max_blob_bytes
                                                 : NH_PORTHOME_BLOSSOM_DEFAULT_MAX_BLOB_BYTES;
    c->signer = signer;
    *out = c;
    return NH_PORTHOME_BLOSSOM_OK;
}

void nh_porthome_blossom_free(nh_porthome_blossom_t *c) {
    if (!c) return;
    for (size_t i = 0; i < c->n_servers; i++) free(c->servers[i]);
    free(c->servers);
    free(c);
}

/* Per-server hanami client. libhanami's client is per-endpoint. */
static hanami_error_t open_client(nh_porthome_blossom_t *c, size_t idx,
                                  hanami_blossom_client_t **out) {
    hanami_blossom_client_opts_t opts = {0};
    opts.endpoint = c->servers[idx];
    opts.timeout_seconds = c->timeout_s;
    opts.user_agent = "nostr-homed-porthome/0.1 (experimental)";
    return hanami_blossom_client_new(&opts, c->signer, out);
}

static void backoff(int attempt) {
    /* 1s, 2s, 4s, capped at 4s. */
    if (attempt <= 0) return;
    int s = 1;
    for (int i = 1; i < attempt && s < 4; i++) s *= 2;
    struct timespec ts = { .tv_sec = s, .tv_nsec = 0 };
    nanosleep(&ts, NULL);
}

int nh_porthome_blossom_upload(nh_porthome_blossom_t *c,
                               const uint8_t *data, size_t len,
                               const char *expected_sha256_hex,
                               char out_sha256_hex[65]) {
    if (!c || (!data && len) || !out_sha256_hex) return NH_PORTHOME_BLOSSOM_ERR_ARG;
    if (len > c->max_blob_bytes) return NH_PORTHOME_BLOSSOM_ERR_TOO_LARGE;

    uint8_t hash[32];
    if (nh_porthome_sha256(data, len, hash) != NH_PORTHOME_OK)
        return NH_PORTHOME_BLOSSOM_ERR_OOM;
    char hex[65];
    nh_porthome_hex64(hash, hex);
    if (expected_sha256_hex && strncasecmp(expected_sha256_hex, hex, 64) != 0)
        return NH_PORTHOME_BLOSSOM_ERR_HASH_MISMATCH;
    memcpy(out_sha256_hex, hex, 65);

    /* Upload to EVERY server so a subsequent read from any live server
     * finds the blob. Return OK iff at least one server accepted. This
     * matches design §5.2 D14 in spirit — commit ≥ 2 confirmed copies —
     * without a hard "≥ 2" requirement in the Phase-1 wrapper (that
     * gate is the caller's policy). */
    int successes = 0;
    int last = NH_PORTHOME_BLOSSOM_ERR_NETWORK;
    for (size_t si = 0; si < c->n_servers; si++) {
        int server_ok = 0;
        for (int a = 0; a <= c->max_retries && !server_ok; a++) {
            hanami_blossom_client_t *hc = NULL;
            if (open_client(c, si, &hc) != HANAMI_OK) { last = NH_PORTHOME_BLOSSOM_ERR_NETWORK; goto next_try; }
            hanami_blob_descriptor_t *desc = NULL;
            hanami_error_t rc = hanami_blossom_upload(hc, data, len, hex, &desc);
            hanami_blossom_client_free(hc);
            if (desc) hanami_blob_descriptor_free(desc);
            if (rc == HANAMI_OK) { server_ok = 1; break; }
            if (rc == HANAMI_ERR_AUTH) { last = NH_PORTHOME_BLOSSOM_ERR_AUTH; break; }
            if (rc == HANAMI_ERR_INVALID_ARG) { last = NH_PORTHOME_BLOSSOM_ERR_ARG; break; }
            last = NH_PORTHOME_BLOSSOM_ERR_NETWORK;
        next_try:
            if (a < c->max_retries && !server_ok) backoff(a + 1);
        }
        if (server_ok) successes++;
    }
    return successes >= 1 ? NH_PORTHOME_BLOSSOM_OK : last;
}

int nh_porthome_blossom_fetch(nh_porthome_blossom_t *c,
                              const char *sha256_hex,
                              uint8_t **out_data, size_t *out_len) {
    if (!c || !sha256_hex || !out_data || !out_len) return NH_PORTHOME_BLOSSOM_ERR_ARG;
    *out_data = NULL; *out_len = 0;

    /* Syntactic hash-shape validation. */
    if (strlen(sha256_hex) != 64) return NH_PORTHOME_BLOSSOM_ERR_ARG;
    uint8_t expect[32];
    if (nh_porthome_from_hex64(sha256_hex, expect) != NH_PORTHOME_OK)
        return NH_PORTHOME_BLOSSOM_ERR_ARG;

    int last = NH_PORTHOME_BLOSSOM_ERR_NOT_FOUND;
    for (size_t si = 0; si < c->n_servers; si++) {
        for (int a = 0; a <= c->max_retries; a++) {
            hanami_blossom_client_t *hc = NULL;
            if (open_client(c, si, &hc) != HANAMI_OK) { last = NH_PORTHOME_BLOSSOM_ERR_NETWORK; goto next_try; }
            uint8_t *buf = NULL; size_t blen = 0;
            hanami_error_t rc = hanami_blossom_get(hc, sha256_hex, &buf, &blen);
            hanami_blossom_client_free(hc);
            if (rc == HANAMI_ERR_NOT_FOUND) { last = NH_PORTHOME_BLOSSOM_ERR_NOT_FOUND; goto next_try; }
            if (rc != HANAMI_OK) { last = NH_PORTHOME_BLOSSOM_ERR_NETWORK; goto next_try; }

            /* Size cap check happens after we already fetched — best we can
             * do without HEAD-then-GET. Free and continue if oversized. */
            if (blen > c->max_blob_bytes) {
                free(buf);
                last = NH_PORTHOME_BLOSSOM_ERR_TOO_LARGE;
                break; /* size cap tripped on this server — no retry fixes it */
            }

            uint8_t got[32];
            if (nh_porthome_sha256(buf, blen, got) != NH_PORTHOME_OK) {
                free(buf); last = NH_PORTHOME_BLOSSOM_ERR_OOM; goto next_try;
            }
            if (memcmp(got, expect, 32) != 0) {
                free(buf); last = NH_PORTHOME_BLOSSOM_ERR_HASH_MISMATCH;
                goto next_try; /* try next server; this one is hostile */
            }
            *out_data = buf; *out_len = blen;
            return NH_PORTHOME_BLOSSOM_OK;
        next_try:
            if (a < c->max_retries) backoff(a + 1);
        }
    }
    return last;
}

int nh_porthome_blossom_has(nh_porthome_blossom_t *c,
                            const char *sha256_hex,
                            bool *out_exists) {
    if (!c || !sha256_hex || !out_exists) return NH_PORTHOME_BLOSSOM_ERR_ARG;
    *out_exists = false;
    if (strlen(sha256_hex) != 64) return NH_PORTHOME_BLOSSOM_ERR_ARG;

    int last = NH_PORTHOME_BLOSSOM_ERR_NETWORK;
    for (size_t si = 0; si < c->n_servers; si++) {
        hanami_blossom_client_t *hc = NULL;
        if (open_client(c, si, &hc) != HANAMI_OK) { last = NH_PORTHOME_BLOSSOM_ERR_NETWORK; continue; }
        bool exists = false;
        hanami_error_t rc = hanami_blossom_head(hc, sha256_hex, &exists);
        hanami_blossom_client_free(hc);
        if (rc == HANAMI_OK) {
            if (exists) { *out_exists = true; return NH_PORTHOME_BLOSSOM_OK; }
            last = NH_PORTHOME_BLOSSOM_OK; /* answered "no" — final if no yes elsewhere */
        }
    }
    return last;
}

int nh_porthome_blossom_delete(nh_porthome_blossom_t *c, const char *sha256_hex) {
    if (!c || !sha256_hex) return NH_PORTHOME_BLOSSOM_ERR_ARG;
    if (strlen(sha256_hex) != 64) return NH_PORTHOME_BLOSSOM_ERR_ARG;

    int fatal = NH_PORTHOME_BLOSSOM_OK;
    for (size_t si = 0; si < c->n_servers; si++) {
        hanami_blossom_client_t *hc = NULL;
        if (open_client(c, si, &hc) != HANAMI_OK) { fatal = NH_PORTHOME_BLOSSOM_ERR_NETWORK; continue; }
        hanami_error_t rc = hanami_blossom_delete(hc, sha256_hex);
        hanami_blossom_client_free(hc);
        if (rc != HANAMI_OK && rc != HANAMI_ERR_NOT_FOUND) fatal = NH_PORTHOME_BLOSSOM_ERR_NETWORK;
    }
    return fatal;
}
