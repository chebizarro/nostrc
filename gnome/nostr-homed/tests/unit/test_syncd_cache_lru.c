/*
 * test_syncd_cache_lru.c — LRU eviction + pin protection.
 *
 * SPDX-License-Identifier: MIT
 *
 * Sets a tiny quota (256 bytes), inserts 4×100-byte blobs at
 * staggered atimes, pins one of the "old" ones, and asserts:
 *   1. Sweep drops total usage <= quota.
 *   2. The pinned blob is preserved.
 *   3. The oldest unpinned blob(s) go first.
 *   4. `used_bytes` reflects the eviction.
 */

#include "nh_syncd_cache.h"
#include "nh_porthome_crypto.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static void rm_rf(const char *p) { char c[512]; snprintf(c,sizeof c,"rm -rf '%s'",p); (void)system(c); }

static void hex_of(const uint8_t *buf, size_t len, char out[65]) {
    uint8_t h[32];
    assert(nh_porthome_sha256(buf, len, h) == 0);
    nh_porthome_hex64(h, out);
}

/* Backdate a cached blob's atime + mtime to `age_secs` in the past.
 * We need a real timestamp gap so lru_cmp orders them deterministically
 * — the underlying FS has 1-second atime granularity by default. */
static void backdate(nh_syncd_cache *c, const char *hex, time_t age_secs) {
    char *path = nh_syncd_cache_get_path(c, hex);
    assert(path);
    struct timespec t[2] = {
        { .tv_sec = time(NULL) - age_secs, .tv_nsec = 0 },
        { .tv_sec = time(NULL) - age_secs, .tv_nsec = 0 },
    };
    assert(utimensat(AT_FDCWD, path, t, 0) == 0);
    free(path);
}

int main(void) {
    char dir[128]; snprintf(dir, sizeof dir, "/tmp/nh_syncd_lru_%d", (int)getpid());
    rm_rf(dir);

    /* Quota = 256 bytes: three 100-byte blobs saturate it, forcing at
     * least one eviction on the fourth. */
    nh_syncd_cache *c = NULL;
    assert(nh_syncd_cache_open(dir, 256, &c) == NH_SYNCD_CACHE_OK);

    uint8_t buf[100];
    memset(buf, 0, sizeof buf);
    char hexes[4][65];

    for (int i = 0; i < 4; ++i) {
        buf[0] = (uint8_t)('a' + i);
        hex_of(buf, sizeof buf, hexes[i]);
        assert(nh_syncd_cache_put(c, hexes[i], buf, sizeof buf) == NH_SYNCD_CACHE_OK);
    }
    /* All four written, all should be on disk (put path doesn't sweep). */
    for (int i = 0; i < 4; ++i) assert(nh_syncd_cache_has(c, hexes[i]));
    assert(nh_syncd_cache_used_bytes(c) == 400);

    /* Force a deterministic age ordering:
     *   0 -> 400 s old (oldest)  <-- PINNED
     *   1 -> 300 s old            (oldest UNPINNED)
     *   2 -> 200 s old
     *   3 -> 100 s old (newest)
     * The pinned blob is old enough to be a candidate for eviction —
     * proving the pin overrides age. */
    backdate(c, hexes[0], 400);
    backdate(c, hexes[1], 300);
    backdate(c, hexes[2], 200);
    backdate(c, hexes[3], 100);

    assert(nh_syncd_cache_pin(c, hexes[0]) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_cache_is_pinned(c, hexes[0]));

    size_t evicted = 0; uint64_t reclaimed = 0;
    assert(nh_syncd_cache_sweep(c, &evicted, &reclaimed) == NH_SYNCD_CACHE_OK);
    /* Need to shed at least 400 - 256 = 144 bytes → at least 2 blobs
     * (each is 100). */
    assert(reclaimed >= 144);
    assert(evicted >= 2);

    /* Post-conditions:
     *   - pinned blob (0) survives
     *   - the oldest unpinned (1) is gone
     *   - the newest (3) survives */
    assert(nh_syncd_cache_has(c, hexes[0]));
    assert(!nh_syncd_cache_has(c, hexes[1]));
    assert(nh_syncd_cache_has(c, hexes[3]));

    /* Total usage now <= quota. */
    assert(nh_syncd_cache_used_bytes(c) <= 256);

    /* Pin refcounting: two pins survive one unpin, third unpin removes. */
    assert(nh_syncd_cache_pin(c, hexes[3]) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_cache_pin(c, hexes[3]) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_cache_is_pinned(c, hexes[3]));
    assert(nh_syncd_cache_unpin(c, hexes[3]) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_cache_is_pinned(c, hexes[3]));
    assert(nh_syncd_cache_unpin(c, hexes[3]) == NH_SYNCD_CACHE_OK);
    assert(!nh_syncd_cache_is_pinned(c, hexes[3]));
    assert(nh_syncd_cache_unpin(c, hexes[3]) == NH_SYNCD_CACHE_ERR_NOT_FOUND);

    nh_syncd_cache_close(c);
    rm_rf(dir);
    printf("ok\n");
    return 0;
}
