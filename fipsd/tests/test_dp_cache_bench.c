#ifdef NDEBUG
#undef NDEBUG /* The setup asserts run in Release too; only the timed calls matter. */
#endif
#define _POSIX_C_SOURCE 200809L
/* Dataplane coordinate-cache costs (nostrc-x345n), through the real entry
 * points of a node's handlers with --cache resident entries:
 *   lookup        fipsd_dp_forward to a transit destination that needs cached
 *                 coordinates (a resident hit, and a miss), ns per packet;
 *   publish       fipsd_dp_publish after one entry changed (a touch moves its
 *                 expiry; an insert of a new key, which also evicts at capacity);
 *   idle          fipsd_dp_publish forced with nothing changed.
 * Medians over --rounds rounds, dispatcher-thread CPU time. Built always, run
 * only by hand (fipsd/docs/perf.md); not part of the upstream-parity micro set.
 *   fipsd_dp_cache_bench [--cache N] [--lookups N] [--publishes N] [--rounds N] */
#include "node/handlers/internal.h"
#include "fakes/host.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static fips_test_clock clk;
static uint64_t rng_state = UINT64_C(0x2545f4914f6cdd1d);
static uint64_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return rng_state; }
static uint64_t cpu_ns(void)
{
    struct timespec t; assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t));
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static double median(double *v, size_t n) { qsort(v, n, sizeof *v, cmp); return n ? v[n / 2] : 0; }
static fips_node_addr key_of(uint64_t seq)
{
    fips_node_addr a; memcpy(a.bytes, &seq, 8); memset(a.bytes + 8, 0xc5, 8);
    return a;
}
static void insert(handlers *h, uint64_t seq)
{
    uint8_t path[6][16]; fips_node_addr a = key_of(seq);
    memcpy(path[0], a.bytes, 16);
    for (int j = 1; j < 5; ++j) for (int k = 0; k < 16; ++k) path[j][k] = (uint8_t)rnd();
    memcpy(path[5], fips_tree_state_root(h->tree)->bytes, 16);
    fips_cached_coord c; fips_hint_outcome out;
    assert(fips_cached_coord_from_addrs(&c, (const uint8_t (*)[16])path, 6) == FIPS_OK);
    (void)fips_coord_cache_insert(&h->cache, a.bytes, &c, hs_now(h), &out);
}
static double publish_ns(handlers *h)
{
    h->dataplane->needs_publish = true;
    uint64_t t0 = cpu_ns();
    assert(fipsd_dp_publish(h));
    return (double)(cpu_ns() - t0);
}
/* ns per forwarded packet over n datagrams to dests[]. */
static double forward_ns(handlers *h, const fips_session_datagram *dgs, size_t n, size_t *routed_or_none)
{
    fipsd_dataplane *d = h->dataplane;
    const fipsd_dp_snapshot *s = d->current.ptr;
    uint8_t buf[512]; fips_writer w; fips_route_outcome out;
    fips_instant now = hs_now(h);
    size_t none = 0;
    uint64_t t0 = cpu_ns();
    for (size_t i = 0; i < n; ++i) {
        fips_writer_init(&w, buf, sizeof buf);
        assert(fipsd_dp_forward(s, &h->router, &dgs[i], NULL, false, now, d->coord_scratch, d->route_peers, &w, &out) == FIPS_OK);
        none += out.kind == FIPS_ROUTE_NoRoute;
    }
    double ns = (double)(cpu_ns() - t0) / (double)n;
    *routed_or_none = none;
    return ns;
}
int main(int argc, char **argv)
{
    size_t cache = 50000, lookups = 20000, publishes = 200, rounds = 5;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--lookups") && i + 1 < argc) lookups = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--publishes") && i + 1 < argc) publishes = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--rounds") && i + 1 < argc) rounds = strtoull(argv[++i], NULL, 10);
        else { fprintf(stderr, "usage: %s [--cache N] [--lookups N] [--publishes N] [--rounds N]\n", argv[0]); return 2; }
    }
    if (cache < 16 || lookups < 1 || publishes < 3 || rounds < 1) { fprintf(stderr, "bad arguments\n"); return 2; }
    fipsd_node_handlers_config cfg; fipsd_node_handlers_config_default(&cfg);
    cfg.peer_capacity = 16; cfg.tree_peer_capacity = cfg.session_capacity = 4; cfg.cache_entries = cache;
    cfg.clock = fips_test_clock_adapter(&clk);
    handlers *h = fipsd_node_handlers_create(&cfg); assert(h);
    const char *yaml = "node: {identity: {persistent: false}}";
    fipsd_config *config = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(config);
    GoSwap *configs = go_swap_create(config, fipsd_config_free); assert(configs);
    fipsd_node_options o = {0}; fipsd_node_handlers_install(h, &o);
    fipsd_node *node = fipsd_node_create(configs, &o); assert(node);
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.handle_signals = false; rc.workers = 2;
    fipsd_runtime *rt = fipsd_runtime_create(&rc); assert(rt);
    assert(!fipsd_runtime_register(rt, fipsd_node_subsystem(node)) && !fipsd_runtime_start(rt));
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    h->running = true;

    uint64_t seq = 0;
    for (; seq < cache; ++seq) insert(h, seq);
    clk.millis += 1;
    double first = publish_ns(h);
    printf("dp cache bench: %zu entries resident, first publish %.2f ms\n", fips_coord_cache_len(&h->cache), first / 1e6);

    fips_session_datagram *hit = calloc(lookups, sizeof *hit), *miss = calloc(lookups, sizeof *miss);
    assert(hit && miss);
    static const uint8_t payload[] = {0x11};
    for (size_t i = 0; i < lookups; ++i) {
        fips_node_addr src = key_of(UINT64_C(1) << 62), dst;
        do dst = key_of(rnd() % seq); while (!fips_coord_cache_get(&h->cache, dst.bytes, hs_now(h)));
        fips_session_datagram_init(&hit[i], src, dst, payload, sizeof payload);
        fips_session_datagram_init(&miss[i], src, key_of((UINT64_C(1) << 61) + i), payload, sizeof payload);
    }
    double *v[6]; for (int k = 0; k < 6; ++k) { v[k] = calloc(rounds * (publishes + 1), sizeof *v[k]); assert(v[k]); }
    size_t nv[6] = {0};
    for (size_t r = 0; r < rounds; ++r) {
        size_t none;
        v[0][nv[0]++] = forward_ns(h, hit, lookups, &none);
        assert(none == lookups); /* no peers: found coordinates, still no next hop */
        v[1][nv[1]++] = forward_ns(h, miss, lookups, &none);
        for (size_t p = 0; p < publishes; ++p) {
            clk.millis += 1;
            fips_node_addr a = hit[rnd() % lookups].dest_addr;
            assert(fips_coord_cache_get_and_touch(&h->cache, a.bytes, hs_now(h)));
            v[2][nv[2]++] = publish_ns(h); /* one entry's expiry moved */
            v[3][nv[3]++] = publish_ns(h); /* nothing changed since */
        }
        for (size_t p = 0; p < publishes / 4; ++p) {
            clk.millis += 1;
            insert(h, seq++);
            v[4][nv[4]++] = publish_ns(h); /* a new key (evicting one at capacity) */
        }
    }
    static const char *const name[] = {
        "lookup hit (fipsd_dp_forward)   ns/packet", "lookup miss (fipsd_dp_forward)  ns/packet",
        "publish after one touch         us", "idle republish                  us",
        "publish after one insert        us"};
    static const double scale[] = {1, 1, 1e3, 1e3, 1e3};
    for (int k = 0; k < 5; ++k) printf("  %-42s median %10.3f  (n=%zu)\n", name[k], median(v[k], nv[k]) / scale[k], nv[k]);
    for (int k = 0; k < 6; ++k) free(v[k]);
    free(hit); free(miss);
    h->running = false;
    fipsd_runtime_destroy(rt); fipsd_node_destroy(node); fipsd_node_handlers_destroy(h); go_swap_free(configs);
    return 0;
}
