/*
 * test_syncd_sweep.c — weekly HEAD sweep behaviour.
 *
 * SPDX-License-Identifier: MIT
 *
 * Uses injected head_fn/upload_fn seams (no network) plus a minimal
 * fake `snapshot.json`. Two of three blobs are declared missing by
 * one server; the sweep must detect them, call the uploader, and
 * fire the notification exactly once with the right counters. The
 * fixed-clock tick driver refuses a second sweep inside the 7-day
 * window and accepts it once time crosses the threshold.
 */

#include "nh_syncd_cache.h"
#include "nh_porthome_crypto.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void rm_rf(const char *p) { char c[512]; snprintf(c,sizeof c,"rm -rf '%s'",p); (void)system(c); }

static void hex_of(const uint8_t *buf, size_t len, char out[65]) {
    uint8_t h[32];
    assert(nh_porthome_sha256(buf, len, h) == 0);
    nh_porthome_hex64(h, out);
}

/* ────── injected HEAD: server "A" always OK, server "B" 404s the
 *        blobs listed in `missing_on_B[]`. */
typedef struct {
    const char **missing_on_B;
    size_t n_missing;
    int head_calls;
} head_ctx;

static int head_seam(void *ud, const char *server, const char *sha) {
    head_ctx *h = (head_ctx *)ud;
    h->head_calls++;
    if (strcmp(server, "https://blossom-A.example") == 0) return 1;
    /* Server B: 0 if in missing list, 1 otherwise. */
    for (size_t i = 0; i < h->n_missing; ++i)
        if (memcmp(h->missing_on_B[i], sha, 64) == 0) return 0;
    return 1;
}

typedef struct {
    int upload_calls;
    char last_hex[65];
    size_t last_len;
} upl_ctx;

static int upload_seam(void *ud, const char *hex,
                       const uint8_t *data, size_t len) {
    upl_ctx *u = (upl_ctx *)ud;
    u->upload_calls++;
    memcpy(u->last_hex, hex, 65);
    u->last_len = len;
    /* Must be sourced from the local cache. */
    assert(data != NULL);
    assert(len > 0);
    return 0;
}

typedef struct { int notify_calls; size_t dropped; size_t reuploaded; } note_ctx;

static void notify_seam(void *ud, size_t d, size_t r, const char *s) {
    note_ctx *n = (note_ctx *)ud;
    n->notify_calls++;
    n->dropped = d;
    n->reuploaded = r;
    (void)s;
}

/* Write a minimal snapshot.json referencing three blob hashes. */
static void write_snapshot(const char *dir, const char *h1, const char *h2, const char *h3) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
    assert(system(cmd) == 0);
    char path[512]; snprintf(path, sizeof path, "%s/snapshot.json", dir);
    FILE *f = fopen(path, "w"); assert(f);
    fprintf(f,
        "{\"schema\":1,\"generation\":42,\"root\":\"/home/x\","
        "\"d_tag\":\"t\",\"account_pubkey_hex\":\"\",\"root_id_hex\":\"\","
        "\"files\":{"
          "\"a\":{\"kind\":\"file\",\"mode\":420,\"uid\":1000,\"gid\":1000,"
          "\"mtime_ns\":0,\"size\":1,\"content_hash_hex\":\"\","
          "\"chunk_addrs_hex\":[\"%s\",\"%s\"]},"
          "\"b\":{\"kind\":\"file\",\"mode\":420,\"uid\":1000,\"gid\":1000,"
          "\"mtime_ns\":0,\"size\":1,\"content_hash_hex\":\"\","
          "\"chunk_addrs_hex\":[\"%s\"]}"
        "}}",
        h1, h2, h3);
    fclose(f);
}

int main(void) {
    char state_dir[128]; snprintf(state_dir, sizeof state_dir,
        "/tmp/nh_syncd_sweep_st_%d", (int)getpid());
    char cache_dir[128]; snprintf(cache_dir, sizeof cache_dir,
        "/tmp/nh_syncd_sweep_ca_%d", (int)getpid());
    rm_rf(state_dir); rm_rf(cache_dir);

    /* Populate the cache with three known blobs. */
    nh_syncd_cache *cache = NULL;
    assert(nh_syncd_cache_open(cache_dir, 1ull << 20, &cache)
           == NH_SYNCD_CACHE_OK);

    uint8_t b1[] = "blob-one";
    uint8_t b2[] = "blob-two";
    uint8_t b3[] = "blob-three";
    char h1[65], h2[65], h3[65];
    hex_of(b1, sizeof b1 - 1, h1);
    hex_of(b2, sizeof b2 - 1, h2);
    hex_of(b3, sizeof b3 - 1, h3);
    assert(nh_syncd_cache_put(cache, h1, b1, sizeof b1 - 1) == 0);
    assert(nh_syncd_cache_put(cache, h2, b2, sizeof b2 - 1) == 0);
    assert(nh_syncd_cache_put(cache, h3, b3, sizeof b3 - 1) == 0);

    write_snapshot(state_dir, h1, h2, h3);

    /* Server B is missing h1 and h3. */
    const char *missing[] = { h1, h3 };
    head_ctx hctx = { missing, 2, 0 };
    upl_ctx  uctx = { 0, {0}, 0 };
    note_ctx nctx = { 0, 0, 0 };

    const char *servers[] = { "https://blossom-A.example", "https://blossom-B.example" };
    nh_syncd_sweep_cfg cfg = {0};
    cfg.servers = servers;
    cfg.n_servers = 2;
    cfg.head_fn = head_seam;
    cfg.head_ud = &hctx;
    cfg.upload_fn = upload_seam;
    cfg.upload_ud = &uctx;
    cfg.notify_fn = notify_seam;
    cfg.notify_ud = &nctx;

    size_t dropped = 0, reuploaded = 0;
    assert(nh_syncd_sweep_run_once(&cfg, state_dir, cache,
                                   &dropped, &reuploaded)
           == NH_SYNCD_CACHE_OK);
    /* 3 blobs × 2 servers = 6 HEADs. */
    assert(hctx.head_calls == 6);
    assert(dropped == 2);
    assert(reuploaded == 2);
    assert(uctx.upload_calls == 2);
    assert(nctx.notify_calls == 1);
    assert(nctx.dropped == 2);
    assert(nctx.reuploaded == 2);

    /* Second sweep with the same fixture (all servers now "healed" —
     * we clear the missing list) fires no notification. */
    hctx.missing_on_B = NULL; hctx.n_missing = 0;
    hctx.head_calls = 0; uctx.upload_calls = 0; nctx.notify_calls = 0;
    assert(nh_syncd_sweep_run_once(&cfg, state_dir, cache,
                                   &dropped, &reuploaded)
           == NH_SYNCD_CACHE_OK);
    assert(dropped == 0);
    assert(reuploaded == 0);
    assert(nctx.notify_calls == 0);

    /* Fixed-clock tick driver: first call from `state=0` fires; second
     * call inside the window skips; a third call after the period
     * fires again. */
    uint64_t tick_state = 0;
    hctx.head_calls = 0;
    assert(nh_syncd_sweep_tick(&cfg, state_dir, cache,
                               1000ull, &tick_state,
                               &dropped, &reuploaded) == 1);
    assert(hctx.head_calls == 6);
    /* Inside the window — no run, no HEAD calls. */
    int prev = hctx.head_calls;
    assert(nh_syncd_sweep_tick(&cfg, state_dir, cache,
                               1000ull + 3600ull, &tick_state,
                               &dropped, &reuploaded) == 0);
    assert(hctx.head_calls == prev);
    /* Past the window — runs again. */
    assert(nh_syncd_sweep_tick(&cfg, state_dir, cache,
                               1000ull + NH_SYNCD_CACHE_SWEEP_PERIOD_SECS + 1,
                               &tick_state,
                               &dropped, &reuploaded) == 1);
    assert(hctx.head_calls > prev);

    nh_syncd_cache_close(cache);
    rm_rf(state_dir); rm_rf(cache_dir);
    printf("ok\n");
    return 0;
}
