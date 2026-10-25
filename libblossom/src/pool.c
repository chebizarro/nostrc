/* Multi-server content-addressed policy, extracted from porthome. */
#include <blossom/pool.h>
#include <blossom/shim.h>
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
struct blossom_pool {
    char **servers;             /* owned */
    size_t n_servers;
    long   timeout_s;
    int    max_retries;
    size_t max_blob_bytes;
    bool allow_insecure;
    const blossom_signer_t *signer; /* borrowed */
};

static int pool_shim_enabled_for(const blossom_pool_t *c) {
    return c && blossom_shim_active_for((const char *const *)c->servers, c->n_servers);
}
int blossom_pool_url_ok(const char *url) {
    if (!url || !*url || url[strlen(url)-1] == '/') return BLOSSOM_POOL_ERR_ARG;
    if (strncasecmp(url, "https://", 8)) return BLOSSOM_POOL_ERR_HTTPS_ONLY;
    return blossom_url_check(url, false) == BLOSSOM_OK ? 0 : BLOSSOM_POOL_ERR_ARG;
}
static int pool_hash(const uint8_t *bytes, size_t len, char out[65]) {
    return blossom_hash_blossom(bytes, len, out) == BLOSSOM_OK ? 0 : -1;
}
int blossom_pool_new(const blossom_pool_opts_t *opts,
                            const blossom_signer_t *signer,
                            blossom_pool_t **out) {
    if (!out) return BLOSSOM_POOL_ERR_ARG;
    *out = NULL;
    if (!opts) return BLOSSOM_POOL_ERR_ARG;
    if (opts->n_servers == 0 || !opts->servers) return BLOSSOM_POOL_ERR_ARG;
    for (size_t i = 0; i < opts->n_servers; i++) {
        int rc = opts->allow_insecure ?
            (blossom_url_check(opts->servers[i], true) == BLOSSOM_OK ? 0 : BLOSSOM_POOL_ERR_ARG) :
            blossom_pool_url_ok(opts->servers[i]);
        if (rc != BLOSSOM_POOL_OK) return rc;
    }

    blossom_pool_t *c = (blossom_pool_t *)calloc(1, sizeof(*c));
    if (!c) return BLOSSOM_POOL_ERR_OOM;
    c->servers = (char **)calloc(opts->n_servers, sizeof(char *));
    if (!c->servers) { free(c); return BLOSSOM_POOL_ERR_OOM; }
    for (size_t i = 0; i < opts->n_servers; i++) {
        c->servers[i] = strdup(opts->servers[i]);
        if (!c->servers[i]) {
            for (size_t j = 0; j < i; j++) free(c->servers[j]);
            free(c->servers); free(c);
            return BLOSSOM_POOL_ERR_OOM;
        }
    }
    c->n_servers = opts->n_servers;
    c->timeout_s = opts->timeout_seconds > 0 ? opts->timeout_seconds
                                             : BLOSSOM_POOL_DEFAULT_TIMEOUT_S;
    c->max_retries = opts->max_retries > 0 ? opts->max_retries
                                           : BLOSSOM_POOL_DEFAULT_MAX_RETRIES;
    c->max_blob_bytes = opts->max_blob_bytes > 0 ? opts->max_blob_bytes
                                                 : BLOSSOM_POOL_DEFAULT_MAX_BLOB_BYTES;
    c->signer = signer;
    c->allow_insecure = opts->allow_insecure;
    *out = c;
    return BLOSSOM_POOL_OK;
}

void blossom_pool_free(blossom_pool_t *c) {
    if (!c) return;
    for (size_t i = 0; i < c->n_servers; i++) free(c->servers[i]);
    free(c->servers);
    free(c);
}

/* Per-server hanami client. libhanami's client is per-endpoint. */
static blossom_error_t open_client(blossom_pool_t *c, size_t idx,
                                  blossom_client_t **out) {
    blossom_client_opts_t opts = {0};
    opts.endpoint = c->servers[idx];
    opts.timeout_seconds = c->timeout_s;
    opts.user_agent = "libblossom/0.1";
    opts.max_blob_bytes = c->max_blob_bytes;
    opts.allow_insecure = c->allow_insecure;
    return blossom_client_new(&opts, c->signer, out);
}

static void backoff(int attempt) {
    /* 1s, 2s, 4s, capped at 4s. */
    if (attempt <= 0) return;
    int s = 1;
    for (int i = 1; i < attempt && s < 4; i++) s *= 2;
    struct timespec ts = { .tv_sec = s, .tv_nsec = 0 };
    nanosleep(&ts, NULL);
}

int blossom_pool_upload(blossom_pool_t *c,
                               const uint8_t *data, size_t len,
                               const char *expected_sha256_hex,
                               char out_sha256_hex[65]) {
    if (!c || (!data && len) || !out_sha256_hex) return BLOSSOM_POOL_ERR_ARG;
    if (len > c->max_blob_bytes) return BLOSSOM_POOL_ERR_TOO_LARGE;
    if (!c->signer || !c->signer->sign) return BLOSSOM_POOL_ERR_AUTH;

    /* PNG-shim (nostrc-bpum): if enabled, wrap the ciphertext in a
     * deterministic PNG prefix BEFORE hashing. The manifest chunk_hash
     * IS sha256(shim||ct); the fetcher recognises the PNG signature at
     * the head of the downloaded blob and strips 41 bytes before
     * decrypting. Convergence is preserved: same ciphertext -> same
     * shimmed bytes -> same sha256. */
    const uint8_t *upload_bytes = data;
    size_t         upload_len   = len;
    uint8_t       *shim_buf     = NULL;
    if (pool_shim_enabled_for(c)) {
        size_t enclen = blossom_shim_encoded_len(len);
        if (enclen > c->max_blob_bytes) return BLOSSOM_POOL_ERR_TOO_LARGE;
        shim_buf = (uint8_t *)malloc(enclen);
        if (!shim_buf) return BLOSSOM_POOL_ERR_OOM;
        if (blossom_shim_wrap(data, len, shim_buf, enclen) != BLOSSOM_OK) {
            free(shim_buf);
            return BLOSSOM_POOL_ERR_ARG;
        }
        upload_bytes = shim_buf;
        upload_len   = enclen;
    }

    char hex[65];
    if (pool_hash(upload_bytes, upload_len, hex) != 0) {
        free(shim_buf);
        return BLOSSOM_POOL_ERR_OOM;
    }
    if (expected_sha256_hex && (strlen(expected_sha256_hex) != 64 || strcasecmp(expected_sha256_hex, hex))) {
        free(shim_buf);
        return BLOSSOM_POOL_ERR_HASH_MISMATCH;
    }
    memcpy(out_sha256_hex, hex, 65);

    /* Upload to EVERY server so a subsequent read from any live server
     * finds the blob. Return OK iff at least one server accepted. This
     * matches design §5.2 D14 in spirit — commit ≥ 2 confirmed copies —
     * without a hard "≥ 2" requirement in the Phase-1 wrapper (that
     * gate is the caller's policy). */
    int successes = 0;
    int last = BLOSSOM_POOL_ERR_NETWORK;
    for (size_t si = 0; si < c->n_servers; si++) {
        int server_ok = 0;
        for (int a = 0; a <= c->max_retries && !server_ok; a++) {
            blossom_client_t *hc = NULL;
            if (open_client(c, si, &hc) != BLOSSOM_OK) { last = BLOSSOM_POOL_ERR_NETWORK; goto next_try; }
            if (shim_buf) (void)blossom_client_set_upload_content_type(hc, "image/png");
            blossom_blob_descriptor_t *desc = NULL;
            blossom_error_t rc = blossom_put_wire(hc, upload_bytes, upload_len, hex, &desc);
            blossom_client_free(hc);
            if (desc) blossom_blob_descriptor_free(desc);
            if (rc == BLOSSOM_OK) { server_ok = 1; break; }
            if (rc == BLOSSOM_ERR_AUTH) { last = BLOSSOM_POOL_ERR_AUTH; break; }
            if (rc == BLOSSOM_ERR_INVALID_ARG) { last = BLOSSOM_POOL_ERR_ARG; break; }
            last = BLOSSOM_POOL_ERR_NETWORK;
        next_try:
            if (a < c->max_retries && !server_ok) backoff(a + 1);
        }
        if (server_ok) successes++;
    }
    free(shim_buf);
    return successes >= 1 ? BLOSSOM_POOL_OK : last;
}

int blossom_pool_fetch(blossom_pool_t *c,
                              const char *sha256_hex,
                              uint8_t **out_data, size_t *out_len) {
    if (!c || !sha256_hex || !out_data || !out_len) return BLOSSOM_POOL_ERR_ARG;
    *out_data = NULL; *out_len = 0;

    /* Syntactic hash-shape validation. */
    if (strlen(sha256_hex) != 64) return BLOSSOM_POOL_ERR_ARG;
    for (size_t i = 0; i < 64; i++)
        if (!strchr("0123456789abcdefABCDEF", sha256_hex[i])) return BLOSSOM_POOL_ERR_ARG;

    int last = BLOSSOM_POOL_ERR_NOT_FOUND;
    for (size_t si = 0; si < c->n_servers; si++) {
        for (int a = 0; a <= c->max_retries; a++) {
            blossom_client_t *hc = NULL;
            if (open_client(c, si, &hc) != BLOSSOM_OK) { last = BLOSSOM_POOL_ERR_NETWORK; goto next_try; }
            uint8_t *buf = NULL; size_t blen = 0;
            blossom_error_t rc = blossom_client_get(hc, sha256_hex, &buf, &blen);
            blossom_client_free(hc);
            if (rc == BLOSSOM_ERR_NOT_FOUND) { last = BLOSSOM_POOL_ERR_NOT_FOUND; goto next_try; }
            if (rc != BLOSSOM_OK) {
                last = rc == BLOSSOM_ERR_TOO_LARGE ? BLOSSOM_POOL_ERR_TOO_LARGE :
                    rc == BLOSSOM_ERR_HASH_MISMATCH ? BLOSSOM_POOL_ERR_HASH_MISMATCH :
                    rc == BLOSSOM_ERR_AUTH ? BLOSSOM_POOL_ERR_AUTH : BLOSSOM_POOL_ERR_NETWORK;
                if (rc == BLOSSOM_ERR_TOO_LARGE || rc == BLOSSOM_ERR_HASH_MISMATCH || rc == BLOSSOM_ERR_AUTH) break;
                goto next_try;
            }

            /* The unified client has already bounded and verified the body. */
            *out_data = buf; *out_len = blen;
            return BLOSSOM_POOL_OK;
        next_try:
            if (a < c->max_retries) backoff(a + 1);
        }
    }
    return last;
}

int blossom_pool_has(blossom_pool_t *c,
                            const char *sha256_hex,
                            bool *out_exists) {
    if (!c || !sha256_hex || !out_exists) return BLOSSOM_POOL_ERR_ARG;
    *out_exists = false;
    if (strlen(sha256_hex) != 64) return BLOSSOM_POOL_ERR_ARG;

    int last = BLOSSOM_POOL_ERR_NETWORK;
    for (size_t si = 0; si < c->n_servers; si++) {
        blossom_client_t *hc = NULL;
        if (open_client(c, si, &hc) != BLOSSOM_OK) { last = BLOSSOM_POOL_ERR_NETWORK; continue; }
        bool exists = false;
        blossom_error_t rc = blossom_client_head(hc, sha256_hex, &exists);
        blossom_client_free(hc);
        if (rc == BLOSSOM_OK) {
            if (exists) { *out_exists = true; return BLOSSOM_POOL_OK; }
            last = BLOSSOM_POOL_OK; /* answered "no" — final if no yes elsewhere */
        }
    }
    return last;
}

int blossom_pool_delete(blossom_pool_t *c, const char *sha256_hex) {
    if (!c || !sha256_hex) return BLOSSOM_POOL_ERR_ARG;
    if (strlen(sha256_hex) != 64) return BLOSSOM_POOL_ERR_ARG;

    int fatal = BLOSSOM_POOL_OK;
    for (size_t si = 0; si < c->n_servers; si++) {
        blossom_client_t *hc = NULL;
        if (open_client(c, si, &hc) != BLOSSOM_OK) { fatal = BLOSSOM_POOL_ERR_NETWORK; continue; }
        blossom_error_t rc = blossom_client_delete(hc, sha256_hex);
        blossom_client_free(hc);
        if (rc != BLOSSOM_OK && rc != BLOSSOM_ERR_NOT_FOUND) fatal = BLOSSOM_POOL_ERR_NETWORK;
    }
    return fatal;
}

/* ---- Batch upload (nostrc-xeby wire-in) ------------------------------
 *
 * Provisioner-first: the primary consumer today is the Phase-2 provisioner
 * (a fresh $HOME uploads dozens of chunks all at once). syncd's on-going
 * single-chunk pushes stay on blossom_pool_upload; changing that
 * loop would require touching per-server accounting in nh_syncd_pusher.c
 * beyond a small refactor — deferred to a follow-up bead if wanted.
 */

int blossom_pool_upload_batch(blossom_pool_t *c,
                                     blossom_pool_batch_blob_t *blobs,
                                     size_t n,
                                     blossom_pool_batch_per_server_t *out_per_server) {
    if (!c || !blobs || n == 0) return BLOSSOM_POOL_ERR_ARG;
    if (c->n_servers == 0) return BLOSSOM_POOL_ERR_ARG;
    for (size_t i = 0; i < n; i++) {
        if (!blobs[i].bytes && blobs[i].len) return BLOSSOM_POOL_ERR_ARG;
        if (blobs[i].len > c->max_blob_bytes) return BLOSSOM_POOL_ERR_TOO_LARGE;
    }
    if (!c->signer || !c->signer->sign) return BLOSSOM_POOL_ERR_AUTH;

    /* PNG-shim (nostrc-bpum): if enabled, wrap every blob's ciphertext
     * BEFORE hashing so the manifest chunk_hash reflects sha256(shim||
     * ct). Shimmed bytes are held in a parallel array whose lifetime
     * runs the length of this call. */
    int shim_on = pool_shim_enabled_for(c);
    uint8_t **shim_bufs = NULL;
    if (shim_on) {
        shim_bufs = (uint8_t **)calloc(n, sizeof(*shim_bufs));
        if (!shim_bufs) return BLOSSOM_POOL_ERR_OOM;
    }

    /* Hash + size-cap check per blob, up-front. */
    for (size_t i = 0; i < n; i++) {
        if (!blobs[i].bytes && blobs[i].len > 0) {
            if (shim_bufs) {
                for (size_t j = 0; j < i; j++) free(shim_bufs[j]);
                free(shim_bufs);
            }
            return BLOSSOM_POOL_ERR_ARG;
        }
        if (blobs[i].len > c->max_blob_bytes) {
            if (shim_bufs) {
                for (size_t j = 0; j < i; j++) free(shim_bufs[j]);
                free(shim_bufs);
            }
            return BLOSSOM_POOL_ERR_TOO_LARGE;
        }

        const uint8_t *up_bytes = blobs[i].bytes;
        size_t         up_len   = blobs[i].len;
        if (shim_on) {
            size_t enclen = blossom_shim_encoded_len(blobs[i].len);
            if (enclen > c->max_blob_bytes) {
                for (size_t j = 0; j < i; j++) free(shim_bufs[j]);
                free(shim_bufs);
                return BLOSSOM_POOL_ERR_TOO_LARGE;
            }
            shim_bufs[i] = (uint8_t *)malloc(enclen);
            if (!shim_bufs[i]) {
                for (size_t j = 0; j < i; j++) free(shim_bufs[j]);
                free(shim_bufs);
                return BLOSSOM_POOL_ERR_OOM;
            }
            if (blossom_shim_wrap(blobs[i].bytes, blobs[i].len,
                                           shim_bufs[i], enclen) != BLOSSOM_OK) {
                for (size_t j = 0; j <= i; j++) free(shim_bufs[j]);
                free(shim_bufs);
                return BLOSSOM_POOL_ERR_ARG;
            }
            up_bytes = shim_bufs[i];
            up_len   = enclen;

        }

        char hex[65];
        if (pool_hash(up_bytes, up_len, hex) != 0) {
            if (shim_bufs) {
                for (size_t j = 0; j < n; j++) free(shim_bufs[j]);
                free(shim_bufs);
            }
            return BLOSSOM_POOL_ERR_OOM;
        }
        if (blobs[i].expected_sha256_hex &&
            (strlen(blobs[i].expected_sha256_hex) != 64 || strcasecmp(blobs[i].expected_sha256_hex, hex))) {
            if (shim_bufs) {
                for (size_t j = 0; j < n; j++) free(shim_bufs[j]);
                free(shim_bufs);
            }
            return BLOSSOM_POOL_ERR_HASH_MISMATCH;
        }
        memcpy(blobs[i].sha256_hex, hex, 65);
    }

    if (out_per_server) {
        for (size_t si = 0; si < c->n_servers; si++) {
            out_per_server[si].server_url      = c->servers[si];
            out_per_server[si].chunks_uploaded = 0;
            out_per_server[si].bytes_uploaded  = 0;
            out_per_server[si].chunks_failed   = 0;
            out_per_server[si].batch_fell_back = 0;
        }
    }

    /* Build the libhanami blob array once. */
    blossom_blob_t *hblobs = (blossom_blob_t *)calloc(n, sizeof(*hblobs));
    blossom_batch_result_t *results =
        (blossom_batch_result_t *)calloc(n, sizeof(*results));
    if (!hblobs || !results) {
        free(hblobs); free(results);
        if (shim_bufs) { for (size_t i = 0; i < n; i++) free(shim_bufs[i]); free(shim_bufs); }
        return BLOSSOM_POOL_ERR_OOM;
    }
    for (size_t i = 0; i < n; i++) {
        hblobs[i].sha256_hex = blobs[i].sha256_hex;
        hblobs[i].bytes      = shim_on ? shim_bufs[i] : blobs[i].bytes;
        hblobs[i].len        = shim_on ? blossom_shim_encoded_len(blobs[i].len) : blobs[i].len;
    }

    /* Track per-blob acceptance across servers. A blob is "success" iff
     * at least one server accepted it. */
    unsigned char *accepted = (unsigned char *)calloc(n, 1);
    if (!accepted) {
        free(hblobs); free(results);
        if (shim_bufs) { for (size_t i = 0; i < n; i++) free(shim_bufs[i]); free(shim_bufs); }
        return BLOSSOM_POOL_ERR_OOM;
    }

    int last = BLOSSOM_POOL_ERR_NETWORK;

    for (size_t si = 0; si < c->n_servers; si++) {
        blossom_client_t *hc = NULL;
        if (open_client(c, si, &hc) != BLOSSOM_OK) {
            last = BLOSSOM_POOL_ERR_NETWORK;
            if (out_per_server) out_per_server[si].chunks_failed = n;
            continue;
        }

        if (shim_on) (void)blossom_client_set_upload_content_type(hc, "image/png");
        blossom_error_t rc = blossom_put_batch_wire(hc, hblobs, n, results);
        (void)rc; /* aggregate rc; we look at per-blob results directly */

        int server_fell_back = 0;
        for (size_t i = 0; i < n; i++) {
            if (results[i].error == BLOSSOM_OK) {
                accepted[i] = 1;
                if (out_per_server) {
                    out_per_server[si].chunks_uploaded++;
                    out_per_server[si].bytes_uploaded += hblobs[i].len;
                }
            } else {
                if (out_per_server) out_per_server[si].chunks_failed++;
                if (results[i].error == BLOSSOM_ERR_AUTH)
                    last = BLOSSOM_POOL_ERR_AUTH;
                else
                    last = BLOSSOM_POOL_ERR_NETWORK;
            }
            if (results[i].error_class &&
                strcmp(results[i].error_class, "batch-fell-back") == 0)
                server_fell_back = 1;
        }
        if (out_per_server) out_per_server[si].batch_fell_back = server_fell_back;

        blossom_client_free(hc);
    }

    /* Every blob needs ≥ 1 server accept. */
    int all_ok = 1;
    for (size_t i = 0; i < n; i++) {
        if (!accepted[i]) { all_ok = 0; break; }
    }

    free(accepted); free(hblobs); free(results);
    if (shim_bufs) {
        for (size_t j = 0; j < n; j++) free(shim_bufs[j]);
        free(shim_bufs);
    }
    return all_ok ? BLOSSOM_POOL_OK : last;
}
