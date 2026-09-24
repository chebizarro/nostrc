/*
 * nh_syncd_sweep.c — weekly HEAD sweep against Blossom (§6.5).
 *
 * SPDX-License-Identifier: MIT
 *
 * For each blob referenced by the current snapshot, HEAD every
 * configured Blossom server. If ANY server returns 404, re-upload via
 * BUD-02 PUT so the target replication factor is restored — a home
 * that silently rots is worse than one that fails loudly. We never
 * call BUD-02 DELETE in v1 (§6.5).
 *
 * Per-server HEAD (not the "any server" aggregate that
 * nh_porthome_blossom_has() exposes) is done by calling libhanami
 * directly for each server: that keeps us honest about which servers
 * dropped the blob without touching src/porthome primitives (their
 * scope is stable — see the I3 plan).
 *
 * Bead: nostrc-p6qp.
 */

#define _GNU_SOURCE
#include "nh_syncd_cache.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <hanami/hanami-blossom-client.h>
#include <hanami/hanami-types.h>

/* Forward from nh_syncd_pin_ring.c — internal linkage helper. */
extern int nh_syncd__snapshot_hashes(const char *state_dir,
                                     uint64_t *out_generation,
                                     char ***out_hashes,
                                     size_t *out_n);

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("nh_syncd_sweep: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* Default per-server HEAD via libhanami. Returns 1/0 or -1 on error. */
static int default_head(void *ud, const char *server, const char *sha256_hex) {
    (void)ud;
    hanami_blossom_client_opts_t opts = {
        .endpoint = server,
        .timeout_seconds = 15,
        .user_agent = "nostr-home-syncd-sweep/0.1",
    };
    hanami_blossom_client_t *c = NULL;
    if (hanami_blossom_client_new(&opts, NULL, &c) != HANAMI_OK) return -1;
    bool exists = false;
    hanami_error_t rc = hanami_blossom_head(c, sha256_hex, &exists);
    hanami_blossom_client_free(c);
    if (rc != HANAMI_OK) return -1;
    return exists ? 1 : 0;
}

typedef struct {
    const hanami_signer_t *signer;
    long                   timeout;
    const char *const     *servers;
    size_t                 n_servers;
} default_upload_ctx;

/* Default re-upload: PUT to each server in turn until at least one
 * succeeds. Returns 0 on success. */
static int default_upload_all(default_upload_ctx *dc,
                              const char *sha256_hex,
                              const uint8_t *data, size_t len) {
    int last = -1;
    for (size_t si = 0; si < dc->n_servers; ++si) {
        hanami_blossom_client_opts_t opts = {
            .endpoint = dc->servers[si],
            .timeout_seconds = dc->timeout ? dc->timeout : 30,
            .user_agent = "nostr-home-syncd-sweep/0.1",
        };
        hanami_blossom_client_t *c = NULL;
        if (hanami_blossom_client_new(&opts, dc->signer, &c) != HANAMI_OK) continue;
        hanami_error_t rc = hanami_blossom_upload(c, data, len, sha256_hex, NULL);
        hanami_blossom_client_free(c);
        if (rc == HANAMI_OK) return 0;
        last = -1;
    }
    return last;
}

int nh_syncd_sweep_run_once(const nh_syncd_sweep_cfg *cfg,
                            const char *state_dir,
                            nh_syncd_cache *cache,
                            size_t *out_dropped,
                            size_t *out_reuploaded) {
    if (!cfg || !state_dir) return NH_SYNCD_CACHE_ERR_ARG;
    if (out_dropped) *out_dropped = 0;
    if (out_reuploaded) *out_reuploaded = 0;

    if (cfg->n_servers == 0) {
        log_msg("no servers configured — sweep skipped");
        return NH_SYNCD_CACHE_OK;
    }

    /* Enumerate blob hashes from the current snapshot. */
    uint64_t gen = 0;
    char **hashes = NULL;
    size_t n = 0;
    int rc = nh_syncd__snapshot_hashes(state_dir, &gen, &hashes, &n);
    if (rc != NH_SYNCD_CACHE_OK) {
        log_msg("snapshot enumeration failed rc=%d", rc);
        return rc;
    }
    if (n == 0) {
        free(hashes);
        return NH_SYNCD_CACHE_OK;
    }

    nh_syncd_head_fn head_fn = cfg->head_fn ? cfg->head_fn : default_head;
    void *head_ud = cfg->head_fn ? cfg->head_ud : NULL;

    default_upload_ctx duc = {
        .signer    = cfg->bud02_signer,
        .timeout   = cfg->timeout_seconds,
        .servers   = cfg->servers,
        .n_servers = cfg->n_servers,
    };

    size_t dropped = 0, reuploaded = 0;
    for (size_t i = 0; i < n; ++i) {
        int missing_anywhere = 0;
        for (size_t si = 0; si < cfg->n_servers; ++si) {
            int r = head_fn(head_ud, cfg->servers[si], hashes[i]);
            if (r == 0) {
                missing_anywhere = 1;
                log_msg("blob %.16s… missing on %s", hashes[i], cfg->servers[si]);
            } else if (r < 0) {
                log_msg("blob %.16s… HEAD error on %s (rc=%d)",
                        hashes[i], cfg->servers[si], r);
                /* Treat as unknown-not-missing: don't force a re-upload
                 * on a transient network hiccup. */
            }
        }
        if (!missing_anywhere) continue;
        dropped++;

        /* Re-upload: source plaintext from the local cache. */
        int uploaded = 0;
        if (cfg->upload_fn) {
            /* Test seam: caller supplies bytes. Pull them from cache
             * if we have them, else pass NULL/0 — the seam decides. */
            uint8_t *data = NULL; size_t dl = 0;
            if (cache) {
                char *path = nh_syncd_cache_get_path(cache, hashes[i]);
                if (path) {
                    FILE *f = fopen(path, "rb");
                    if (f) {
                        struct stat st; if (fstat(fileno(f), &st) == 0) {
                            data = (uint8_t *)malloc((size_t)st.st_size);
                            if (data) {
                                dl = fread(data, 1, (size_t)st.st_size, f);
                                if (dl != (size_t)st.st_size) { free(data); data = NULL; dl = 0; }
                            }
                        }
                        fclose(f);
                    }
                    free(path);
                }
            }
            if (cfg->upload_fn(cfg->upload_ud, hashes[i], data, dl) == 0)
                uploaded = 1;
            free(data);
        } else {
            /* Default path: need cache to source plaintext. */
            if (!cache) {
                log_msg("cannot re-upload %.16s…: no cache configured", hashes[i]);
                continue;
            }
            char *path = nh_syncd_cache_get_path(cache, hashes[i]);
            if (!path) {
                log_msg("cannot re-upload %.16s…: not in cache", hashes[i]);
                continue;
            }
            FILE *f = fopen(path, "rb");
            free(path);
            if (!f) continue;
            struct stat st;
            if (fstat(fileno(f), &st) != 0) { fclose(f); continue; }
            uint8_t *data = (uint8_t *)malloc((size_t)st.st_size);
            if (!data) { fclose(f); continue; }
            size_t got = fread(data, 1, (size_t)st.st_size, f);
            fclose(f);
            if (got != (size_t)st.st_size) { free(data); continue; }
            if (default_upload_all(&duc, hashes[i], data, got) == 0)
                uploaded = 1;
            free(data);
        }
        if (uploaded) reuploaded++;
    }

    for (size_t i = 0; i < n; ++i) free(hashes[i]);
    free(hashes);

    if (out_dropped) *out_dropped = dropped;
    if (out_reuploaded) *out_reuploaded = reuploaded;

    if (dropped > 0 && cfg->notify_fn) {
        char summary[256];
        snprintf(summary, sizeof summary,
                 "Your Blossom server dropped %zu blob%s (re-uploaded %zu)",
                 dropped, dropped == 1 ? "" : "s", reuploaded);
        cfg->notify_fn(cfg->notify_ud, dropped, reuploaded, summary);
    }
    log_msg("sweep done: %zu blobs checked, %zu dropped, %zu re-uploaded",
            n, dropped, reuploaded);
    return NH_SYNCD_CACHE_OK;
}

int nh_syncd_sweep_tick(const nh_syncd_sweep_cfg *cfg,
                        const char *state_dir,
                        nh_syncd_cache *cache,
                        uint64_t now_secs,
                        uint64_t *state_ptr,
                        size_t *out_dropped,
                        size_t *out_reuploaded) {
    if (!cfg || !state_dir || !state_ptr) return NH_SYNCD_CACHE_ERR_ARG;
    if (out_dropped) *out_dropped = 0;
    if (out_reuploaded) *out_reuploaded = 0;
    if (*state_ptr != 0 && now_secs < *state_ptr + NH_SYNCD_CACHE_SWEEP_PERIOD_SECS)
        return 0;
    int rc = nh_syncd_sweep_run_once(cfg, state_dir, cache,
                                     out_dropped, out_reuploaded);
    if (rc == NH_SYNCD_CACHE_OK) {
        *state_ptr = now_secs;
        return 1;
    }
    return rc;
}
