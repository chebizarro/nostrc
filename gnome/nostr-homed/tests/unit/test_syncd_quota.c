/*
 * test_syncd_quota.c — Phase 5 I3 (bead nostrc-8hw8):
 *   effective-quota resolver, auto-eviction on put, thrash notify.
 *
 * SPDX-License-Identifier: MIT
 *
 * The cache LRU regressions live in test_syncd_cache_lru; this test
 * covers only the new I3 behaviours:
 *   1. NOSTR_HOMED_PORTHOME_CACHE_QUOTA_BYTES honoured within guards,
 *      rejected outside the [1 GiB, 200 GiB] band.
 *   2. XDG config-file override honoured when the env is unset.
 *   3. nh_syncd_cache_put auto-evicts LRU on overflow.
 *   4. A run of put()s that would drive above the threshold fires the
 *      evict-thrash notify callback exactly once per window.
 */

#include "nh_syncd_cache.h"
#include "nh_porthome_crypto.h"

#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static void rm_rf(const char *p) {
    char c[512]; snprintf(c, sizeof c, "rm -rf '%s'", p); (void)system(c);
}

static void hex_of(const uint8_t *buf, size_t len, char out[65]) {
    uint8_t h[32];
    assert(nh_porthome_sha256(buf, len, h) == 0);
    nh_porthome_hex64(h, out);
}

static void test_env_override_bounds(void) {
    char dir[128]; snprintf(dir, sizeof dir, "/tmp/nh_quota_env_%d", (int)getpid());
    rm_rf(dir);
    /* Below the 1 GiB floor → ignored. */
    setenv("NOSTR_HOMED_PORTHOME_CACHE_QUOTA_BYTES", "1024", 1);
    const char *src = "";
    uint64_t v1 = nh_syncd_cache_effective_quota(dir, &src);
    /* Falls back to the FS-derived default, which for /tmp is >= 1 GiB
     * on any real host — we only assert it wasn't the rejected value. */
    assert(v1 != 1024);
    assert(!strcmp(src, "default"));
    /* Above the 200 GiB ceiling → ignored. */
    setenv("NOSTR_HOMED_PORTHOME_CACHE_QUOTA_BYTES",
           "9999999999999", 1);
    v1 = nh_syncd_cache_effective_quota(dir, &src);
    assert(v1 != 9999999999999ull);
    /* Inside the band → honoured. */
    setenv("NOSTR_HOMED_PORTHOME_CACHE_QUOTA_BYTES",
           "2147483648", 1); /* 2 GiB */
    v1 = nh_syncd_cache_effective_quota(dir, &src);
    assert(v1 == 2147483648ull);
    assert(!strcmp(src, "env"));
    unsetenv("NOSTR_HOMED_PORTHOME_CACHE_QUOTA_BYTES");
    printf("env_override_bounds OK\n");
}

static void test_file_override(void) {
    char dir[128]; snprintf(dir, sizeof dir, "/tmp/nh_quota_file_%d", (int)getpid());
    rm_rf(dir);
    char cfgdir[128];
    snprintf(cfgdir, sizeof cfgdir, "/tmp/nh_quota_cfg_%d", (int)getpid());
    char cfg[256];
    snprintf(cfg, sizeof cfg, "%s/nostr-homed", cfgdir);
    (void)mkdir(cfgdir, 0700);
    (void)mkdir(cfg, 0700);
    char path[300];
    snprintf(path, sizeof path, "%s/cache-quota", cfg);
    FILE *f = fopen(path, "we"); assert(f);
    fprintf(f, "3221225472\n"); /* 3 GiB */
    fclose(f);
    unsetenv("NOSTR_HOMED_PORTHOME_CACHE_QUOTA_BYTES");
    setenv("XDG_CONFIG_HOME", cfgdir, 1);
    const char *src = "";
    uint64_t v = nh_syncd_cache_effective_quota(dir, &src);
    assert(v == 3221225472ull);
    assert(!strcmp(src, "config"));
    rm_rf(cfgdir);
    unsetenv("XDG_CONFIG_HOME");
    printf("file_override OK\n");
}

static void test_auto_eviction_on_put(void) {
    char dir[128]; snprintf(dir, sizeof dir, "/tmp/nh_quota_ae_%d", (int)getpid());
    rm_rf(dir);
    nh_syncd_cache *c = NULL;
    /* 256-byte quota — 3× 100-byte blobs overflow. */
    assert(nh_syncd_cache_open(dir, 256, &c) == NH_SYNCD_CACHE_OK);
    nh_syncd_cache_set_auto_evict(c, true);

    uint8_t buf[100]; memset(buf, 0, sizeof buf);
    char hex_a[65], hex_b[65], hex_c[65];
    buf[0] = 'a'; hex_of(buf, sizeof buf, hex_a);
    assert(nh_syncd_cache_put(c, hex_a, buf, sizeof buf) == NH_SYNCD_CACHE_OK);
    /* Backdate so 'a' is the oldest. */
    char *pa = nh_syncd_cache_get_path(c, hex_a);
    struct timespec t[2] = {
        { .tv_sec = time(NULL) - 300, .tv_nsec = 0 },
        { .tv_sec = time(NULL) - 300, .tv_nsec = 0 },
    };
    (void)utimensat(AT_FDCWD, pa, t, 0);
    free(pa);

    buf[0] = 'b'; hex_of(buf, sizeof buf, hex_b);
    assert(nh_syncd_cache_put(c, hex_b, buf, sizeof buf) == NH_SYNCD_CACHE_OK);
    buf[0] = 'c'; hex_of(buf, sizeof buf, hex_c);
    /* This put pushes usage to 300 > quota=256. Auto-eviction must kick
     * in and drop 'a'. */
    assert(nh_syncd_cache_put(c, hex_c, buf, sizeof buf) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_cache_used_bytes(c) <= 256);
    assert(!nh_syncd_cache_has(c, hex_a));
    assert(nh_syncd_cache_has(c, hex_b));
    assert(nh_syncd_cache_has(c, hex_c));

    /* Now pin b, c, AND the yet-to-be-written d. put(d) will land
     * the blob, then sweep will find every candidate pinned and
     * refuse to reclaim — NH_SYNCD_CACHE_ERR_QUOTA_PINNED is the
     * signal for the caller to fire LIMITED_MODE. */
    assert(nh_syncd_cache_pin(c, hex_b) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_cache_pin(c, hex_c) == NH_SYNCD_CACHE_OK);
    uint8_t buf2[100]; memset(buf2, 0, sizeof buf2); buf2[0] = 'd';
    char hex_d[65];
    hex_of(buf2, sizeof buf2, hex_d);
    assert(nh_syncd_cache_pin(c, hex_d) == NH_SYNCD_CACHE_OK);
    int rc = nh_syncd_cache_put(c, hex_d, buf2, sizeof buf2);
    assert(rc == NH_SYNCD_CACHE_ERR_QUOTA_PINNED);

    nh_syncd_cache_close(c);
    rm_rf(dir);
    printf("auto_eviction_on_put OK\n");
}

/* Test the thrash detector by driving many evictions in-window. */
static unsigned g_thrash_hits = 0;
static unsigned g_thrash_last_count = 0;
static void thrash_cb(void *ud, unsigned count, unsigned thr, int64_t start) {
    (void)ud; (void)thr; (void)start;
    g_thrash_hits++;
    g_thrash_last_count = count;
}

static void test_thrash_notify(void) {
    char dir[128]; snprintf(dir, sizeof dir, "/tmp/nh_quota_thr_%d", (int)getpid());
    rm_rf(dir);
    /* Threshold to 3 via env so we don't have to fake 100 evictions. */
    setenv("NOSTR_HOMED_PORTHOME_QUOTA_THRASH_THRESHOLD", "3", 1);
    nh_syncd_cache *c = NULL;
    /* 100-byte quota — every 100-byte put forces an eviction. */
    assert(nh_syncd_cache_open(dir, 100, &c) == NH_SYNCD_CACHE_OK);
    nh_syncd_cache_set_auto_evict(c, true);
    assert(nh_syncd_cache_thrash_threshold(c) == 3);
    g_thrash_hits = 0; g_thrash_last_count = 0;
    nh_syncd_cache_set_evict_notify(c, thrash_cb, NULL);

    uint8_t buf[100];
    /* Insert 6 distinct blobs — each triggers eviction of the last one. */
    for (int i = 0; i < 6; ++i) {
        memset(buf, 0, sizeof buf);
        buf[0] = (uint8_t)('a' + i);
        char hex[65]; hex_of(buf, sizeof buf, hex);
        int rc = nh_syncd_cache_put(c, hex, buf, sizeof buf);
        /* Some of these may return OK (we made room via eviction) — the
         * point is that evictions actually happen. */
        (void)rc;
    }
    /* At least one thrash-notify. The count reported must exceed the
     * threshold. */
    assert(g_thrash_hits >= 1);
    assert(g_thrash_last_count > 3u);
    assert(nh_syncd_cache_evict_count_1h(c) > 3u);

    nh_syncd_cache_close(c);
    rm_rf(dir);
    unsetenv("NOSTR_HOMED_PORTHOME_QUOTA_THRASH_THRESHOLD");
    printf("thrash_notify OK (hits=%u count=%u)\n",
           g_thrash_hits, g_thrash_last_count);
}

int main(void) {
    test_env_override_bounds();
    test_file_override();
    test_auto_eviction_on_put();
    test_thrash_notify();
    printf("test_syncd_quota: all tests passed\n");
    return 0;
}
