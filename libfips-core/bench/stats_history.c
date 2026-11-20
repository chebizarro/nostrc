/* Stats-history tick cost with n retained peers (no upstream bench): a tick at
 * which one new peer arrives at the lowest address, the dispatcher's worst
 * case for a sorted peer table, against a tick with the same peers and no
 * arrival. Every retained peer is present each tick (a tick samples every
 * peer, so it is O(n) either way); the arrival is evicted again by an untimed
 * tick 24 hours later, so n stays fixed. Host program: it reads a monotonic
 * clock and allocates; the core under test does neither. Not a ctest. */
#include "../tests/fakes/host.h"
#include <fips/node/stats_history.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bench.h"

enum { ROUNDS = 4 };

static void set_addr(uint8_t out[16], uint32_t value)
{
    memset(out, 0xa5, 12);
    for (int i = 0; i < 4; ++i) out[12 + i] = (uint8_t)(value >> (24 - 8 * i)); /* big-endian: memcmp order */
}

static void tick(fips_stats_history *h, uint64_t *secs, fips_peer_snapshot *peers, size_t count)
{
    fips_stats_snapshot s = {.tree_depth = 2, .has_mesh_size = true, .mesh_size = 40};
    for (size_t i = 0; i < count; ++i) {
        peers[i].last_seen = fips_test_instant(*secs * 1000);
        peers[i].bytes_in_total += 1000;
    }
    REQUIRE(fips_stats_history_tick(h, fips_test_instant(*secs * 1000), &s, peers, count) == FIPS_OK);
}

static int run(size_t n)
{
    fips_peer_stats *storage = calloc(n + 1, sizeof(*storage));
    fips_peer_snapshot *peers = calloc(n + 1, sizeof(*peers));
    fips_stats_history *h = calloc(1, sizeof(*h));
    if (!storage || !peers || !h) return 1;
    fips_stats_history_init(h, storage, n + 1);
    for (size_t i = 0; i < n; ++i) {
        set_addr(peers[i + 1].node_addr, (uint32_t)(1000000 + i));
        peers[i + 1].has_srtt_ms = true;
        peers[i + 1].srtt_ms = 20;
    }
    uint64_t secs = 1;
    uint32_t arrival = 999999;
    tick(h, &secs, peers + 1, n);
    REQUIRE(h->peer_count == n);
    for (int with_arrival = 0; with_arrival < 2; ++with_arrival) {
        double samples[BENCH_SAMPLES];
        for (unsigned s = 0; s < BENCH_SAMPLES + 10; ++s) {
            uint64_t total = 0;
            for (unsigned round = 0; round < ROUNDS; ++round) {
                ++secs;
                if (with_arrival) set_addr(peers[0].node_addr, arrival--);
                uint64_t start = bench_now();
                tick(h, &secs, with_arrival ? peers : peers + 1, with_arrival ? n + 1 : n);
                total += bench_now() - start;
                REQUIRE(h->peer_count == n + (size_t)with_arrival);
                if (with_arrival) {
                    secs += FIPS_PEER_EVICTION_SECS; /* the arrival falls silent and is evicted */
                    tick(h, &secs, peers + 1, n);
                    REQUIRE(h->peer_count == n);
                }
            }
            if (s >= 10) samples[s - 10] = total > 0 ? (double)total / ROUNDS : 1.0;
        }
        char name[80];
        snprintf(name, sizeof(name), "stats_history/tick/%zu_peers/%s", n, with_arrival ? "one_arrival" : "no_arrival");
        bench_report(name, samples);
    }
    free(h);
    free(peers);
    free(storage);
    return 0;
}

int main(void)
{
    size_t sizes[] = {64, 512};
    for (size_t i = 0; i < 2; ++i)
        if (run(sizes[i]) != 0) {
            fprintf(stderr, "stats_history bench setup failed at %zu peers\n", sizes[i]);
            return 1;
        }
    return 0;
}
