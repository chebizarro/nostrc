#ifdef NDEBUG
#undef NDEBUG /* The setup asserts run in Release too; only the timed publish matters. */
#endif
#define _POSIX_C_SOURCE 200809L
/* Control-snapshot publish latency with many retained churned peers
 * (nostrc-igg4w). Drives the real dispatcher handlers: the stats history holds
 * --peers retained peers (24-hour retention; most are absent on any tick, and
 * new ones arrive at random sorted positions), the coordinate cache holds
 * --cache resident entries under touch/insert churn, and a slow control client
 * periodically holds snapshots across publishes. Every hs_publish() (the real
 * `publish_snapshot` stage) is timed and the p50/p99/max reported per kind.
 *
 * Each publish is timed on the wall clock and on the dispatcher thread's CPU
 * clock: the CPU figure is the cost of the work and survives a loaded host, the
 * wall figure adds whatever preemption the host inflicts.
 *
 * Opt-in like fipsd_dataplane_throughput: always built, never run by ordinary
 * ctest. A Release build is the measurement; --max-p99-us makes it a gate on
 * the clock named by --clock (wall, the default, or cpu).
 *   fipsd_snapshot_bench [--peers N] [--cache N] [--publishes N]
 *                        [--cache-churn PCT] [--slow-clients N]
 *                        [--max-p99-us N] [--clock wall|cpu]
 * --cache-churn is the share of publishes preceded by cache traffic (touches,
 * occasionally an insert); a forwarding node's cache is touched constantly, an
 * idle one never. Default 25.
 * --slow-clients N adds N control clients that each keep a snapshot for a few
 * history ticks (20, 31, 42... publishes) and take the next as soon as they
 * let go, so the stats-history views are pinned while the history advances:
 * the case that used to force a copy of the whole retained history. */
#include "node/handlers/internal.h"
#include "fakes/host.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum kind { K_STATE, K_TICK, K_HELD, K_CACHE, K_COUNT };
static const char *const kind_name[K_COUNT] = {
    "state change (history+cache unchanged)", "history tick", "slow reader held a snapshot", "cache churn"};
static uint64_t *samples[2][K_COUNT + 1]; /* [wall, cpu] */
static size_t counts[K_COUNT + 1];
static uint64_t rng_state = UINT64_C(0x9e3779b97f4a7c15);
static uint64_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return rng_state; }
static uint64_t clock_ns(clockid_t id)
{
    struct timespec t; assert(!clock_gettime(id, &t));
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static int cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static uint64_t pct(uint64_t *v, size_t n, double p)
{
    if (!n) return 0;
    size_t i = (size_t)((double)(n - 1) * p / 100.0 + 0.999999);
    return v[i < n ? i : n - 1];
}
static void report(const char *name, uint64_t *v, size_t n)
{
    qsort(v, n, sizeof *v, cmp);
    uint64_t sum = 0; for (size_t i = 0; i < n; ++i) sum += v[i];
    printf("  %-40s n=%-5zu avg=%7.1f us  p50=%7.1f  p99=%8.1f  max=%8.1f us\n", name, n,
        n ? (double)sum / (double)n / 1e3 : 0, (double)pct(v, n, 50) / 1e3, (double)pct(v, n, 99) / 1e3,
        n ? (double)v[n - 1] / 1e3 : 0);
}

static uint8_t (*pool)[16];
static size_t pool_len;
static fips_test_clock clk;
static uint64_t totals[1024];

/* What hs_stats_tick does with this tick's live peers, minus the live reads. */
static void history_tick(handlers *h, size_t present, size_t known)
{
    fips_peer_snapshot *peers = h->history_samples; size_t n = 0;
    fips_stats_snapshot s = {.tree_depth = 2, .bytes_in_total = rnd() >> 40, .has_mesh_size = true, .mesh_size = 40};
    for (size_t i = 0; i < present && n < h->cfg.peer_capacity; ++i) {
        size_t id = i == 0 ? known - 1 : (size_t)(rnd() % known); /* the newest peer is always present */
        bool dup = false;
        for (size_t j = 0; j < n; ++j) if (!memcmp(peers[j].node_addr, pool[id], 16)) dup = true;
        if (dup) continue;
        fips_peer_snapshot *p = &peers[n++]; memset(p, 0, sizeof *p);
        memcpy(p->node_addr, pool[id], 16); p->last_seen = hs_now(h);
        p->has_srtt_ms = true; p->srtt_ms = (double)(rnd() % 90); p->has_loss_rate = true;
        totals[id] += rnd() % 5000; p->bytes_in_total = totals[id]; p->packets_in_total = totals[id] / 7;
    }
    s.peer_count = n;
    fipsd_stats_outcome o = fipsd_stats_history_update(h->history, &h->history_peers, hs_now(h), &s, peers, n);
    assert(o == FIPSD_STATS_APPLIED);
    fipsd_stats_views_record(&h->stats_views, true, hs_now(h), &s, peers, n);
}
static void cache_insert(handlers *h, uint64_t seq)
{
    fips_cached_coord c; uint8_t path[6][16];
    for (int j = 0; j < 6; ++j) { for (int k = 0; k < 16; ++k) path[j][k] = (uint8_t)rnd(); }
    memcpy(path[0], &seq, sizeof seq); memset(path[0] + 8, 0xc5, 8);
    assert(fips_cached_coord_from_addrs(&c, (const uint8_t (*)[16])path, 6) == FIPS_OK);
    fips_hint_outcome out; (void)fips_coord_cache_insert(&h->cache, path[0], &c, hs_now(h), &out);
}
static void cache_touch(handlers *h, uint64_t seq_max)
{
    uint8_t a[16]; uint64_t seq = rnd() % seq_max;
    memcpy(a, &seq, sizeof seq); memset(a + 8, 0xc5, 8);
    (void)fips_coord_cache_get_and_touch(&h->cache, a, hs_now(h));
}
static void timed_publish(handlers *h, enum kind kind, size_t cap)
{
    uint64_t c0 = clock_ns(CLOCK_THREAD_CPUTIME_ID), w0 = clock_ns(CLOCK_MONOTONIC);
    hs_publish(h, hs_now(h));
    uint64_t wall = clock_ns(CLOCK_MONOTONIC) - w0, cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID) - c0;
    if (counts[kind] < cap) { samples[0][kind][counts[kind]] = wall; samples[1][kind][counts[kind]++] = cpu; }
    if (counts[K_COUNT] < cap) { samples[0][K_COUNT][counts[K_COUNT]] = wall; samples[1][K_COUNT][counts[K_COUNT]++] = cpu; }
}

int main(int argc, char **argv)
{
    size_t peers = 256, cache = 50000, publishes = 2000, churn_pct = 25, slow_clients = 0; uint64_t max_p99_us = 0; int gate_cpu = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--peers") && i + 1 < argc) peers = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--publishes") && i + 1 < argc) publishes = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--cache-churn") && i + 1 < argc) churn_pct = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--slow-clients") && i + 1 < argc) slow_clients = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--max-p99-us") && i + 1 < argc) max_p99_us = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--clock") && i + 1 < argc) { const char *c = argv[++i]; gate_cpu = !strcmp(c, "cpu"); if (!gate_cpu && strcmp(c, "wall")) return 2; }
        else { fprintf(stderr, "usage: %s [--peers N] [--cache N] [--publishes N] [--cache-churn PCT] [--slow-clients N] [--max-p99-us N] [--clock wall|cpu]\n", argv[0]); return 2; }
    }
    if (peers < 2 || peers > 1024 || publishes < 100 || churn_pct > 100 || slow_clients > 4) { fprintf(stderr, "bad arguments\n"); return 2; }
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k <= K_COUNT; ++k) { samples[c][k] = calloc(publishes + 1, sizeof **samples[c]); assert(samples[c][k]); }
    pool_len = peers; pool = calloc(pool_len, 16); assert(pool);
    for (size_t i = 0; i < pool_len; ++i) for (int k = 0; k < 16; ++k) pool[i][k] = (uint8_t)rnd();

    fipsd_node_handlers_config cfg; fipsd_node_handlers_config_default(&cfg);
    cfg.peer_capacity = 256; cfg.tree_peer_capacity = cfg.session_capacity = 4; cfg.cache_entries = cache;
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

    /* Retain `peers` peers: one new peer per simulated second, inserted at a
     * random sorted position; the rest of each tick's presence is a random
     * subset of those already known, so most retained peers are churned out. */
    for (size_t known = 1; known <= peers; ++known) {
        history_tick(h, 16, known); clk.millis += 1000;
        if (known % 8 == 0) hs_publish(h, hs_now(h));
    }
    uint64_t seq = 0;
    for (; seq < cache; ++seq) cache_insert(h, seq);
    hs_publish(h, hs_now(h));
    printf("snapshot bench: %zu retained peers, %zu cache entries resident, %zu publishes, cache traffic before %zu%% of them\n",
           h->history->peer_count, fips_coord_cache_len(&h->cache), publishes, churn_pct);

    GoRefPtr held[4] = {{0}, {0}, {0}, {0}}; size_t hold_left[4] = {0, 0, 0, 0};
    uint64_t replays0 = h->stats_views.replays, copies0 = h->stats_views.copies;
    for (size_t i = 0; i < publishes; ++i) {
        enum kind kind = K_STATE;
        if (i % 10 == 0) { clk.millis += 1000; history_tick(h, 16, peers); kind = K_TICK; }
        else clk.millis += 20;
        if (cache > 0 && rnd() % 100 < churn_pct) {
            for (int t = 0; t < 8; ++t) cache_touch(h, seq);
            if (rnd() % 4 == 0) cache_insert(h, seq++);
            kind = kind == K_STATE ? K_CACHE : kind;
        }
        for (size_t r = 0; r < 4; ++r) if (hold_left[r] && !--hold_left[r]) go_refptr_release(&held[r]);
        if (slow_clients) {
            /* Clients rendering a large response: they hold what they took for
             * several ticks and take the next snapshot the moment they let go. */
            for (size_t c = 0; c < slow_clients; ++c)
                if (!held[c].ptr) { held[c] = go_swap_load(h->snapshot); hold_left[c] = 20 + 11 * c; }
        } else if (i % 25 == 9) { /* one client holding across a few publishes */
            size_t r = !held[0].ptr ? 0 : 1;
            if (!held[r].ptr) { held[r] = go_swap_load(h->snapshot); hold_left[r] = 4 + (size_t)(rnd() % 4); }
            if (kind == K_TICK || kind == K_STATE) kind = K_HELD;
        }
        timed_publish(h, kind, publishes);
    }
    for (int r = 0; r < 4; ++r) go_refptr_release(&held[r]);

    printf("history views while measuring: %" PRIu64 " replays, %" PRIu64 " full copies\n",
           h->stats_views.replays - replays0, h->stats_views.copies - copies0);
    static const char *const clock_name[2] = {"wall clock", "dispatcher thread CPU time"};
    for (int c = 0; c < 2; ++c) {
        printf("%s:\n", clock_name[c]);
        for (int k = 0; k < K_COUNT; ++k) if (counts[k]) report(kind_name[k], samples[c][k], counts[k]);
        report("all publishes", samples[c][K_COUNT], counts[K_COUNT]);
    }
    for (int c = 0; c < 2; ++c) {
        size_t over = 0;
        for (size_t i = 0; i < counts[K_COUNT]; ++i) over += samples[c][K_COUNT][i] > 1000000;
        printf("publishes over 1 ms (%s): %zu of %zu\n", clock_name[c], over, counts[K_COUNT]);
    }
    uint64_t p99 = pct(samples[gate_cpu][K_COUNT], counts[K_COUNT], 99);
    printf("publish_snapshot p99 = %.1f us (%s)%s\n", (double)p99 / 1e3, clock_name[gate_cpu],
           max_p99_us ? (p99 / 1000 <= max_p99_us ? "  within gate" : "  OVER GATE") : "");

    h->running = false;
    fipsd_runtime_destroy(rt); fipsd_node_destroy(node); fipsd_node_handlers_destroy(h); go_swap_free(configs);
    for (int c = 0; c < 2; ++c) for (int k = 0; k <= K_COUNT; ++k) free(samples[c][k]);
    free(pool);
    return max_p99_us && p99 / 1000 > max_p99_us ? 1 : 0;
}
