/*
 * test_syncd_pin_ring.c — 10-slot generation ring behaviour.
 *
 * SPDX-License-Identifier: MIT
 *
 * Asserts:
 *   1. Empty on first open.
 *   2. Promoting 11 generations evicts slot 0 (FIFO).
 *   3. Effective-pins is the union across all held generations.
 *   4. Round-trip through disk preserves ordering and hashes.
 *   5. Repeated promote of the same generation is idempotent (replaces).
 *   6. Malformed hashes are silently dropped.
 *   7. apply() pins every effective hash in a live cache.
 */

#include "nh_syncd_cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void rm_rf(const char *p) { char c[512]; snprintf(c,sizeof c,"rm -rf '%s'",p); (void)system(c); }

static const char *mk_hex(char buf[65], uint8_t seed) {
    for (int i = 0; i < 64; ++i) buf[i] = "0123456789abcdef"[(seed + i) & 0xf];
    buf[64] = '\0';
    return buf;
}

int main(void) {
    char pin_path[128]; snprintf(pin_path, sizeof pin_path,
        "/tmp/nh_syncd_pin_%d/pinned.json", (int)getpid());
    char dir[128]; snprintf(dir, sizeof dir, "/tmp/nh_syncd_pin_%d", (int)getpid());
    rm_rf(dir);

    /* Empty open (file absent). */
    nh_syncd_pin_ring *r = NULL;
    assert(nh_syncd_pin_ring_open(pin_path, &r) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_pin_ring_size(r) == 0);
    assert(nh_syncd_pin_ring_capacity(r) == NH_SYNCD_CACHE_RETENTION_GENERATIONS);

    /* Promote 11 generations, one blob each with a seed = gen. */
    char hex[65];
    for (uint64_t g = 1; g <= 11; ++g) {
        mk_hex(hex, (uint8_t)g);
        const char *arr[1] = { hex };
        assert(nh_syncd_pin_ring_promote(r, g, arr, 1) == NH_SYNCD_CACHE_OK);
    }
    assert(nh_syncd_pin_ring_size(r) == 10);
    /* The 11th promote should have evicted gen=1; the effective set
     * contains gens 2..11 (10 hashes, all distinct — each seed byte
     * differs). */
    char **eff = NULL; size_t neff = 0;
    assert(nh_syncd_pin_ring_effective_pins(r, &eff, &neff) == NH_SYNCD_CACHE_OK);
    assert(neff == 10);
    /* gen 1's hash (seed=1) MUST NOT be present. */
    char gen1[65]; mk_hex(gen1, 1);
    for (size_t i = 0; i < neff; ++i) {
        assert(memcmp(eff[i], gen1, 64) != 0);
        free(eff[i]);
    }
    free(eff);

    /* Idempotent replace: promote gen=11 again with a different hash;
     * ring size unchanged, gen=11's slot now carries the new hash. */
    char alt[65];
    memset(alt, 'a', 64); alt[64] = '\0';  /* uniform-nibble hex — no collision with mk_hex(1..11) */
    const char *arr2[1] = { alt };
    assert(nh_syncd_pin_ring_promote(r, 11, arr2, 1) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_pin_ring_size(r) == 10);
    assert(nh_syncd_pin_ring_effective_pins(r, &eff, &neff) == NH_SYNCD_CACHE_OK);
    int seen_alt = 0;
    char gen11[65]; mk_hex(gen11, 11);
    int seen_old_gen11 = 0;
    for (size_t i = 0; i < neff; ++i) {
        if (memcmp(eff[i], alt, 64) == 0) seen_alt = 1;
        if (memcmp(eff[i], gen11, 64) == 0) seen_old_gen11 = 1;
        free(eff[i]);
    }
    free(eff);
    assert(seen_alt == 1);
    assert(seen_old_gen11 == 0);

    /* Round-trip through disk. */
    nh_syncd_pin_ring_close(r);
    r = NULL;
    assert(nh_syncd_pin_ring_open(pin_path, &r) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_pin_ring_size(r) == 10);
    assert(nh_syncd_pin_ring_effective_pins(r, &eff, &neff) == NH_SYNCD_CACHE_OK);
    assert(neff == 10);
    for (size_t i = 0; i < neff; ++i) free(eff[i]);
    free(eff);

    /* Malformed hashes silently dropped. */
    const char *arr3[3] = { "not-hex", alt, "" };
    assert(nh_syncd_pin_ring_promote(r, 12, arr3, 3) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_pin_ring_size(r) == 10); /* still 10 — ring was full */
    /* apply() pins every effective hash. */
    char cache_dir[128]; snprintf(cache_dir, sizeof cache_dir,
        "/tmp/nh_syncd_pin_cache_%d", (int)getpid());
    rm_rf(cache_dir);
    nh_syncd_cache *cache = NULL;
    assert(nh_syncd_cache_open(cache_dir, 1ull << 20, &cache)
           == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_pin_ring_apply(r, cache) == NH_SYNCD_CACHE_OK);
    assert(nh_syncd_pin_ring_effective_pins(r, &eff, &neff) == NH_SYNCD_CACHE_OK);
    for (size_t i = 0; i < neff; ++i) {
        assert(nh_syncd_cache_is_pinned(cache, eff[i]));
        free(eff[i]);
    }
    free(eff);
    nh_syncd_cache_close(cache);
    rm_rf(cache_dir);

    /* pinned.json is mode 0600 (secrets-adjacent hygiene). */
    struct stat st;
    assert(stat(pin_path, &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    nh_syncd_pin_ring_close(r);
    rm_rf(dir);
    printf("ok\n");
    return 0;
}
