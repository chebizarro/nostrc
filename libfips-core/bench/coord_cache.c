/* Port of upstream benches/coord_cache.rs: CoordCache::get hits and misses at
 * a small cache (1,000 entries) and at the default capacity (50,000), the
 * forwarding path's per-packet lookup. C addition (no upstream counterpart):
 * purge_expired, the dispatcher's per-tick call, at the same two sizes with
 * nothing expired and with 16 entries due, to show its cost follows what it
 * removes, not the capacity. Host program: it reads a monotonic clock and
 * allocates; the core under test does neither. Not a ctest. */
#include "../tests/fakes/host.h"
#include <fips/cache/coord_cache.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bench.h"

#define SAMPLES 4096

static uint64_t seed = UINT64_C(0x123456789abcdef1);

static void addr(uint8_t out[16])
{
    for (int chunk = 0; chunk < 2; ++chunk) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        for (int i = 0; i < 8; ++i) out[chunk * 8 + i] = (uint8_t)(seed >> (8 * i));
    }
}

static void time_gets(const char *name, const fips_coord_cache *cache, const uint8_t (*keys)[16])
{
    fips_instant at = fips_test_instant(2);
    size_t i = 0;
    BENCH(name, {
        i = (i + 1) % SAMPLES;
        const fips_cached_coord *result = fips_coord_cache_get(cache, keys[i], at);
        bench_observe(result);
    });
}

static int run(size_t n)
{
    seed = UINT64_C(0x123456789abcdef1);
    size_t slot_count = FIPS_COORD_CACHE_SLOTS(n);
    fips_coord_slot *slots = calloc(slot_count, sizeof(*slots));
    uint32_t *index = calloc(FIPS_COORD_CACHE_INDEX_WORDS(slot_count), sizeof(*index));
    uint8_t (*keys)[16] = calloc(n, 16);
    uint8_t (*hits)[16] = calloc(SAMPLES, 16);
    uint8_t (*misses)[16] = calloc(SAMPLES, 16);
    fips_coord_cache cache;
    fips_seeded_entropy entropy = {1};
    if (!slots || !index || !keys || !hits || !misses) return 1;
    if (fips_coord_cache_init(&cache, slots, slot_count, index,
                              FIPS_COORD_CACHE_INDEX_WORDS(slot_count), n, UINT64_MAX / 2,
                              fips_test_instant(0), fips_seeded_entropy_adapter(&entropy)) != FIPS_OK)
        return 1;
    for (size_t i = 0; i < n; ++i) addr(keys[i]);
    for (size_t i = 0; i < n; ++i) {
        uint8_t path[6][16];
        memcpy(path[0], keys[i], 16);
        for (int j = 1; j < 6; ++j) addr(path[j]);
        fips_cached_coord coords;
        fips_hint_outcome outcome;
        if (fips_cached_coord_from_addrs(&coords, path, 6) != FIPS_OK ||
            fips_coord_cache_insert(&cache, keys[i], &coords, fips_test_instant(1), &outcome) != FIPS_OK)
            return 1;
    }
    for (size_t i = 0; i < SAMPLES; ++i) memcpy(hits[i], keys[(i * 7919) % n], 16);
    for (size_t i = 0; i < SAMPLES; ++i) addr(misses[i]);
    for (size_t i = 0; i < SAMPLES; ++i) {
        REQUIRE(fips_coord_cache_get(&cache, hits[i], fips_test_instant(2)) != NULL);
        REQUIRE(fips_coord_cache_get(&cache, misses[i], fips_test_instant(2)) == NULL);
    }
    char name[80];
    snprintf(name, sizeof(name), "coord_cache/get_hit/%zu", n);
    time_gets(name, &cache, hits);
    snprintf(name, sizeof(name), "coord_cache/get_miss/%zu", n);
    time_gets(name, &cache, misses);
    free(slots);
    free(index);
    free(keys);
    free(hits);
    free(misses);
    return 0;
}

/* n long-lived entries. With nothing due, purges are batched like the get
 * benches. With entries due, each round first inserts DUE short-lived entries
 * (untimed) and then times the one purge that removes exactly them; a sample
 * averages ROUNDS such purges, so a coarse (1 us) host clock still resolves. */
static int run_purge(size_t n)
{
    enum { DUE = 16, ROUNDS = 64 };
    seed = UINT64_C(0x0fedcba987654321);
    size_t max = n + DUE, slot_count = FIPS_COORD_CACHE_SLOTS(max);
    fips_coord_slot *slots = calloc(slot_count, sizeof(*slots));
    uint32_t *index = calloc(FIPS_COORD_CACHE_INDEX_WORDS(slot_count), sizeof(*index));
    fips_coord_cache cache;
    fips_seeded_entropy entropy = {2};
    if (!slots || !index) return 1;
    if (fips_coord_cache_init(&cache, slots, slot_count, index, FIPS_COORD_CACHE_INDEX_WORDS(slot_count),
                              max, UINT64_MAX / 2, fips_test_instant(0),
                              fips_seeded_entropy_adapter(&entropy)) != FIPS_OK)
        return 1;
    uint8_t path[6][16];
    fips_cached_coord coords;
    fips_hint_outcome outcome;
    for (size_t i = 0; i < n; ++i) {
        for (int j = 0; j < 6; ++j) addr(path[j]);
        if (fips_cached_coord_from_addrs(&coords, path, 6) != FIPS_OK ||
            fips_coord_cache_insert(&cache, path[0], &coords, fips_test_instant(1), &outcome) != FIPS_OK)
            return 1;
    }
    char name[80];
    uint64_t now = 10;
    snprintf(name, sizeof(name), "coord_cache/purge_expired/%zu/0_due", n);
    BENCH(name, {
        size_t removed = fips_coord_cache_purge_expired(&cache, fips_test_instant(++now));
        bench_observe(&removed);
    });
    REQUIRE(fips_coord_cache_len(&cache) == n);
    double samples[BENCH_SAMPLES];
    for (unsigned s = 0; s < BENCH_SAMPLES + 10; ++s) {
        uint64_t total = 0;
        for (unsigned round = 0; round < ROUNDS; ++round) {
            for (size_t i = 0; i < DUE; ++i) {
                for (int j = 0; j < 6; ++j) addr(path[j]);
                REQUIRE(fips_cached_coord_from_addrs(&coords, path, 6) == FIPS_OK);
                REQUIRE(fips_coord_cache_insert_with_ttl(&cache, path[0], &coords, fips_test_instant(now), 1,
                                                         &outcome) == FIPS_OK && outcome == FIPS_HINT_INSERTED);
            }
            now += 2;
            uint64_t start = bench_now();
            size_t removed = fips_coord_cache_purge_expired(&cache, fips_test_instant(now));
            total += bench_now() - start;
            REQUIRE(removed == DUE && fips_coord_cache_len(&cache) == n);
        }
        if (s >= 10) samples[s - 10] = total > 0 ? (double)total / ROUNDS : 1.0;
    }
    snprintf(name, sizeof(name), "coord_cache/purge_expired/%zu/%d_due", n, DUE);
    bench_report(name, samples);
    free(slots);
    free(index);
    return 0;
}

int main(void)
{
    size_t sizes[] = {1000, 50000};
    for (size_t i = 0; i < 2; ++i)
        if (run(sizes[i]) != 0) {
            fprintf(stderr, "coord_cache bench setup failed at %zu entries\n", sizes[i]);
            return 1;
        }
    for (size_t i = 0; i < 2; ++i)
        if (run_purge(sizes[i]) != 0) {
            fprintf(stderr, "coord_cache purge bench setup failed at %zu entries\n", sizes[i]);
            return 1;
        }
    return 0;
}
