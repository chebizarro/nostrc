/* Snapshot stats-history views: replayed views answer every control query
 * exactly like the live history under peer churn, 24-hour eviction and storage
 * growth; a held view never changes; a view the log cannot replay (older than
 * the log, behind a refused update, or a log-less zeroed pool) is synced
 * incrementally and only a never-filled or re-initialized one is copied whole;
 * a reader thread releasing views while the writer replays them is race-free
 * (run under TSan). */
#include "node/stats_view.h"
#include "host_os.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); abort(); } } while (0)
#define POOL 24
#define CAP 8
#define SERIES (FIPS_FAST_RING_CAPACITY > FIPS_SLOW_RING_CAPACITY ? FIPS_FAST_RING_CAPACITY : FIPS_SLOW_RING_CAPACITY)
typedef struct live { fips_stats_history h; fips_peer_stats *storage; } live;
static uint32_t rng = 7;
static uint32_t next(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static double a[SERIES], b[SERIES];

/* Everything control reads: every series at both granularities, every
 * retained peer's metadata and series, latest and recent. */
static bool same(const fips_stats_history *x, const fips_stats_history *y)
{
    if (x->peer_count != y->peer_count || x->fast_pushes != y->fast_pushes || x->has_data != y->has_data) return false;
    fips_duration window = fips_duration_from_millis(UINT64_C(86400000));
    for (int g = 0; g < 2; ++g)
        for (int m = 0; m < FIPS_METRIC_COUNT; ++m) {
            fips_stats_series sa, sb;
            if (fips_stats_history_query(x, (fips_metric)m, window, (fips_granularity)g, a, SERIES, &sa) != FIPS_OK ||
                fips_stats_history_query(y, (fips_metric)m, window, (fips_granularity)g, b, SERIES, &sb) != FIPS_OK ||
                sa.len != sb.len || memcmp(a, b, sa.len * sizeof *a)) return false;
        }
    for (size_t i = 0; i < x->peer_count; ++i) {
        const fips_peer_stats *p = &x->peers[i], *q = fips_stats_history_peer(y, p->addr);
        if (!q || memcmp(&p->first_seen, &q->first_seen, sizeof p->first_seen) ||
            memcmp(&p->last_contact, &q->last_contact, sizeof p->last_contact)) return false;
        for (int g = 0; g < 2; ++g)
            for (int m = 0; m < FIPS_PEER_METRIC_COUNT; ++m) {
                fips_stats_series sa, sb;
                if (fips_stats_history_peer_query(x, p->addr, (fips_peer_metric)m, window, (fips_granularity)g, a, SERIES, &sa) != FIPS_OK ||
                    fips_stats_history_peer_query(y, p->addr, (fips_peer_metric)m, window, (fips_granularity)g, b, SERIES, &sb) != FIPS_OK ||
                    sa.len != sb.len || memcmp(a, b, sa.len * sizeof *a)) return false;
            }
    }
    size_t na = 0, nb = 0;
    if (fips_stats_history_recent(x, FIPS_METRIC_PEER_COUNT, 30, a, SERIES, &na) != FIPS_OK ||
        fips_stats_history_recent(y, FIPS_METRIC_PEER_COUNT, 30, b, SERIES, &nb) != FIPS_OK) return false;
    return na == nb && !memcmp(a, b, na * sizeof *a);
}
static fips_instant now_at;
static uint64_t totals[POOL];
static fipsd_stats_outcome tick(live *l, fipsd_stats_views *v)
{
    fips_peer_snapshot peers[CAP]; size_t n = 0;
    fips_stats_snapshot s = {.has_mesh_size = next() % 2, .mesh_size = next() % 50, .tree_depth = next() % 4,
        .bytes_in_total = next(), .loss_rate = (double)(next() % 100) / 100.0, .active_sessions = next() % 3};
    for (size_t i = 0; i < POOL && n < CAP; ++i) {
        if (next() % 4) continue;
        fips_peer_snapshot *p = &peers[n++]; memset(p, 0, sizeof *p);
        p->node_addr[0] = (uint8_t)(i + 1); p->last_seen = now_at;
        p->has_srtt_ms = next() % 2; p->srtt_ms = (double)(next() % 900);
        p->has_loss_rate = true; p->loss_rate = (double)(next() % 10) / 10.0;
        totals[i] += next() % 5000; p->bytes_in_total = totals[i]; p->packets_out_total = totals[i] / 3;
    }
    s.peer_count = n;
    fipsd_stats_outcome o = fipsd_stats_history_update(&l->h, &l->storage, now_at, &s, peers, n);
    fipsd_stats_views_record(v, o == FIPSD_STATS_APPLIED, now_at, &s, peers, n);
    uint64_t step = next() % 97 == 0 ? UINT64_C(90000000) : 1000; /* occasionally past the 24 h eviction */
    now_at = fips_deadline(now_at, fips_duration_from_millis(step));
    return o;
}
static void live_init(live *l)
{
    l->storage = calloc(1, sizeof *l->storage); CHECK(l->storage);
    fips_stats_history_init(&l->h, l->storage, 1);
}

static _Atomic(fipsd_stats_view *) handoff;
static atomic_bool done;
static void *reader(void *unused)
{
    (void)unused; double buf[64]; size_t n;
    while (!atomic_load(&done)) {
        fipsd_stats_view *w = atomic_exchange(&handoff, NULL);
        if (!w) continue;
        const fips_stats_history *h = fipsd_stats_view_history(w);
        for (int m = 0; m < FIPS_METRIC_COUNT; ++m) (void)fips_stats_history_recent(h, (fips_metric)m, 64, buf, 64, &n);
        for (size_t i = 0; i < h->peer_count; ++i) (void)fips_stats_history_peer(h, h->peers[i].addr);
        fipsd_stats_view_release(w);
    }
    return NULL;
}

int main(void)
{
    fips_clock clock = fipsd_host_os_clock(); now_at = clock.now(clock.ctx);
    static live l; live_init(&l);
    fipsd_stats_views v; CHECK(fipsd_stats_views_init(&v, CAP));

    /* Replay tracks live; a view held across ticks stays as it was. */
    fipsd_stats_view *held = NULL; static fips_stats_history frozen; fips_peer_stats *frozen_peers = NULL;
    for (int i = 0; i < 400; ++i) {
        CHECK(tick(&l, &v) == FIPSD_STATS_APPLIED);
        if (next() % 3 == 0) continue; /* history may run ahead of publishes */
        fipsd_stats_view *w = fipsd_stats_views_acquire(&v, &l.h); CHECK(w);
        CHECK(same(fipsd_stats_view_history(w), &l.h));
        fipsd_stats_view *again = fipsd_stats_views_acquire(&v, &l.h); CHECK(again == w); /* unchanged: shared */
        fipsd_stats_view_release(again);
        if (held) CHECK(same(fipsd_stats_view_history(held), &frozen));
        if (!held && next() % 5 == 0) {
            held = w; frozen = *fipsd_stats_view_history(w);
            frozen_peers = realloc(frozen_peers, (frozen.peer_count + 1) * sizeof *frozen_peers); CHECK(frozen_peers);
            memcpy(frozen_peers, frozen.peers, frozen.peer_count * sizeof *frozen_peers); frozen.peers = frozen_peers;
            continue;
        }
        if (held && next() % 7 == 0) { fipsd_stats_view_release(held); held = NULL; }
        fipsd_stats_view_release(w);
    }
    fipsd_stats_view_release(held);
    CHECK(v.replays > 4 * v.copies); /* steady state replays; copies only to start */
    /* Every pooled view read at once: the pool gives the oldest to its reader. */
    fipsd_stats_view *r[FIPSD_STATS_VIEWS + 2];
    for (size_t i = 0; i < FIPSD_STATS_VIEWS + 2; ++i) {
        (void)tick(&l, &v); r[i] = fipsd_stats_views_acquire(&v, &l.h); CHECK(r[i] && same(fipsd_stats_view_history(r[i]), &l.h));
    }
    for (size_t i = 0; i < FIPSD_STATS_VIEWS + 2; ++i) fipsd_stats_view_release(r[i]);
    /* A refused update and more than a log of updates leave the views behind
     * the log: they are synced (what changed), not copied, and still equal live. */
    uint64_t copies = v.copies, syncs = v.syncs;
    fipsd_stats_views_record(&v, false, now_at, &(fips_stats_snapshot){0}, NULL, 0);
    fipsd_stats_view *w = fipsd_stats_views_acquire(&v, &l.h); CHECK(w && same(fipsd_stats_view_history(w), &l.h));
    fipsd_stats_view_release(w);
    CHECK(v.copies == copies && v.syncs == syncs + 1);
    for (int round = 0; round < 80; ++round) {
        int gap = FIPSD_STATS_LOG + 1 + (int)(next() % 40);
        for (int i = 0; i < gap; ++i) (void)tick(&l, &v);
        w = fipsd_stats_views_acquire(&v, &l.h); CHECK(w && same(fipsd_stats_view_history(w), &l.h)); fipsd_stats_view_release(w);
    }
    CHECK(v.copies == copies && v.syncs >= syncs + 1 + 80);
    /* A slow reader holds one view across many publishes: with three views the
     * other two keep alternating, so it costs no copy, and what it holds is
     * unchanged however far the live history moves. */
    copies = v.copies;
    fipsd_stats_view *slow = fipsd_stats_views_acquire(&v, &l.h); CHECK(slow);
    static fips_stats_history slow_frozen; fips_peer_stats *slow_peers = NULL;
    slow_frozen = *fipsd_stats_view_history(slow);
    slow_peers = malloc((slow_frozen.peer_count + 1) * sizeof *slow_peers); CHECK(slow_peers);
    memcpy(slow_peers, slow_frozen.peers, slow_frozen.peer_count * sizeof *slow_peers); slow_frozen.peers = slow_peers;
    for (int i = 0; i < 60; ++i) {
        (void)tick(&l, &v);
        w = fipsd_stats_views_acquire(&v, &l.h); CHECK(w && same(fipsd_stats_view_history(w), &l.h)); fipsd_stats_view_release(w);
        CHECK(same(fipsd_stats_view_history(slow), &slow_frozen));
    }
    CHECK(v.copies <= copies + 1); /* at most the third view's first fill */
    /* Every view read at once: the fourth reader takes a fresh view, filled whole. */
    fipsd_stats_view *others[FIPSD_STATS_VIEWS];
    for (size_t i = 0; i < FIPSD_STATS_VIEWS - 1; ++i) {
        (void)tick(&l, &v); others[i] = fipsd_stats_views_acquire(&v, &l.h); CHECK(others[i]);
    }
    copies = v.copies; (void)tick(&l, &v);
    w = fipsd_stats_views_acquire(&v, &l.h); CHECK(w && same(fipsd_stats_view_history(w), &l.h)); fipsd_stats_view_release(w);
    CHECK(v.copies == copies + 1 && same(fipsd_stats_view_history(slow), &slow_frozen));
    for (size_t i = 0; i < FIPSD_STATS_VIEWS - 1; ++i) fipsd_stats_view_release(others[i]);
    fipsd_stats_view_release(slow); free(slow_peers);
    /* A re-initialized history is a new lineage: its views are copied whole. */
    copies = v.copies;
    fips_stats_history_init(&l.h, l.storage, l.h.peer_capacity); fipsd_stats_views_invalidate(&v);
    w = fipsd_stats_views_acquire(&v, &l.h); CHECK(w && fipsd_stats_view_history(w)->peer_count == 0 && same(fipsd_stats_view_history(w), &l.h));
    CHECK(v.copies == copies + 1);
    fipsd_stats_view *kept = w; /* outlives the pool */
    fipsd_stats_views_free(&v);
    CHECK(same(fipsd_stats_view_history(kept), &l.h)); fipsd_stats_view_release(kept);

    /* Zeroed pool, no log: the first fill is a copy and every later change a sync. */
    fipsd_stats_views z; memset(&z, 0, sizeof z);
    for (int i = 0; i < 20; ++i) {
        (void)tick(&l, &z);
        w = fipsd_stats_views_acquire(&z, &l.h); CHECK(w && same(fipsd_stats_view_history(w), &l.h)); fipsd_stats_view_release(w);
    }
    CHECK(z.copies == 1 && z.syncs >= 15 && z.replays == 0);
    fipsd_stats_views_free(&z);

    /* Reader on another thread; the writer only rewrites views it alone holds. */
    CHECK(fipsd_stats_views_init(&v, CAP));
    pthread_t t; CHECK(pthread_create(&t, NULL, reader, NULL) == 0);
    for (int i = 0; i < 3000; ++i) {
        (void)tick(&l, &v);
        w = fipsd_stats_views_acquire(&v, &l.h); CHECK(w);
        fipsd_stats_view_release(atomic_exchange(&handoff, w));
    }
    atomic_store(&done, true); CHECK(pthread_join(t, NULL) == 0);
    fipsd_stats_view_release(atomic_exchange(&handoff, NULL));
    fipsd_stats_views_free(&v);
    free(frozen_peers); free(l.storage);
    puts("stats views: replay parity, immutability, eviction, fallbacks, cross-thread release");
    return 0;
}
