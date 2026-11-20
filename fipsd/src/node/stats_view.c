#include "node/stats_view.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct fipsd_stats_view {
    atomic_size_t refs; /* the pool's plus one per snapshot */
    uint64_t seq;       /* live updates reflected; meaningful only if valid */
    uint64_t lineage;   /* the live history's lineage when last filled */
    bool valid;
    fips_peer_stats *storage; /* history.peers; history.peer_capacity is its size */
    fips_stats_history history;
};

fipsd_stats_outcome fipsd_stats_history_update(fips_stats_history *h, fips_peer_stats **storage,
    fips_instant now, const fips_stats_snapshot *snapshot, const fips_peer_snapshot *peers, size_t count)
{
    /* Retain churned peers for the core's 24-hour window, not just the
     * currently active peer-capacity budget. */
    fips_stats_history_cleanup(h, now);
    size_t needed = h->peer_count;
    for (size_t i = 0; i < count; ++i) if (!fips_stats_history_peer(h, peers[i].node_addr)) ++needed;
    if (needed > h->peer_capacity) {
        size_t cap = needed > h->peer_capacity * 2 ? needed : h->peer_capacity * 2;
        fips_peer_stats *grown = realloc(*storage, cap * sizeof *grown);
        if (!grown) return FIPSD_STATS_NO_MEMORY;
        *storage = grown; h->peers = grown; h->peer_capacity = cap;
    }
    if (fips_stats_history_tick(h, now, snapshot, peers, count) == FIPS_OK) return FIPSD_STATS_APPLIED;
    fips_stats_history_cleanup(h, now);
    return fips_stats_history_tick(h, now, snapshot, peers, count) == FIPS_OK ? FIPSD_STATS_APPLIED : FIPSD_STATS_EXHAUSTED;
}

bool fipsd_stats_views_init(fipsd_stats_views *v, size_t peer_capacity)
{
    memset(v, 0, sizeof *v);
    for (size_t i = 0; peer_capacity && i < FIPSD_STATS_LOG; ++i)
        if (!(v->log[i].peers = malloc(peer_capacity * sizeof *v->log[i].peers))) { fipsd_stats_views_free(v); return false; }
    v->log_capacity = peer_capacity;
    return true;
}
void fipsd_stats_views_free(fipsd_stats_views *v)
{
    for (size_t i = 0; i < FIPSD_STATS_VIEWS; ++i) fipsd_stats_view_release(v->pool[i]);
    for (size_t i = 0; i < FIPSD_STATS_LOG; ++i) free(v->log[i].peers);
    memset(v, 0, sizeof *v);
}
void fipsd_stats_views_record(fipsd_stats_views *v, bool applied, fips_instant now,
    const fips_stats_snapshot *snapshot, const fips_peer_snapshot *peers, size_t count)
{
    fipsd_stats_update *u = &v->log[++v->seq % FIPSD_STATS_LOG];
    u->seq = v->seq;
    /* A refused live update is not replayed: a view with other capacity could
     * succeed where the live history did not. Views behind it copy instead. */
    u->replayable = applied && count <= v->log_capacity;
    if (!u->replayable) return;
    u->now = now; u->snapshot = *snapshot; u->count = count;
    if (count) memcpy(u->peers, peers, count * sizeof *peers);
}
void fipsd_stats_views_invalidate(fipsd_stats_views *v)
{
    fipsd_stats_update *u = &v->log[++v->seq % FIPSD_STATS_LOG];
    u->seq = v->seq; u->replayable = false;
    ++v->lineage; /* no view is an earlier state of what the history is now */
}

static bool replay(fipsd_stats_views *v, fipsd_stats_view *w)
{
    if (!w->valid || v->seq - w->seq > FIPSD_STATS_LOG) return false;
    while (w->seq != v->seq) {
        const fipsd_stats_update *u = &v->log[(w->seq + 1) % FIPSD_STATS_LOG];
        if (u->seq != w->seq + 1 || !u->replayable) return false;
        if (fipsd_stats_history_update(&w->history, &w->storage, u->now, &u->snapshot, u->count ? u->peers : NULL, u->count)
            != FIPSD_STATS_APPLIED) return false;
        ++w->seq; ++v->replays;
    }
    return true;
}
static bool copy(fipsd_stats_views *v, fipsd_stats_view *w, const fips_stats_history *live)
{
    w->valid = false;
    size_t cap = w->history.peer_capacity;
    if (live->peer_count > cap) {
        fips_peer_stats *grown = realloc(w->storage, live->peer_count * sizeof *grown);
        if (!grown) return false;
        w->storage = grown; cap = live->peer_count;
    }
    w->history = *live; w->history.peers = w->storage; w->history.peer_capacity = cap;
    if (live->peer_count) memcpy(w->storage, live->peers, live->peer_count * sizeof *w->storage);
    w->seq = v->seq; w->lineage = v->lineage; w->valid = true; ++v->copies;
    return true;
}
/* A view the log cannot replay but whose history is an earlier state of the
 * live one: copy what changed since. Its storage keeps whatever it holds (the
 * core reads only peer_count of it), so growing it by realloc is enough. */
static bool sync(fipsd_stats_views *v, fipsd_stats_view *w, const fips_stats_history *live)
{
    if (!w->valid || w->lineage != v->lineage) return false;
    if (live->peer_count > w->history.peer_capacity) {
        fips_peer_stats *grown = realloc(w->storage, live->peer_count * sizeof *grown);
        if (!grown) return false;
        w->storage = grown; w->history.peers = grown; w->history.peer_capacity = live->peer_count;
    }
    if (fips_stats_history_sync(&w->history, live) != FIPS_OK) return false;
    w->seq = v->seq; ++v->syncs;
    return true;
}
/* Least replay first; an invalid view needs a full copy whatever its seq. */
static uint64_t rank(const fipsd_stats_view *w) { return w->valid ? w->seq + 1 : 0; }
fipsd_stats_view *fipsd_stats_views_acquire(fipsd_stats_views *v, const fips_stats_history *live)
{
    fipsd_stats_view *w = v->current;
    if (!w || !w->valid || w->seq != v->seq) {
        /* A view only the pool holds is ours to rewrite: the acquire pairs
         * with the acq_rel decrement of the last snapshot that read it. */
        size_t slot = FIPSD_STATS_VIEWS, empty = FIPSD_STATS_VIEWS, oldest = FIPSD_STATS_VIEWS;
        for (size_t i = 0; i < FIPSD_STATS_VIEWS; ++i) {
            fipsd_stats_view *c = v->pool[i];
            if (!c) { empty = i; continue; }
            if (oldest == FIPSD_STATS_VIEWS || rank(c) < rank(v->pool[oldest])) oldest = i;
            if (atomic_load_explicit(&c->refs, memory_order_acquire) != 1) continue;
            if (slot == FIPSD_STATS_VIEWS || rank(c) > rank(v->pool[slot])) slot = i;
        }
        if (slot == FIPSD_STATS_VIEWS) {
            /* Every view is still read: give up the oldest to its readers. */
            slot = empty != FIPSD_STATS_VIEWS ? empty : oldest;
            fipsd_stats_view_release(v->pool[slot]);
            if (v->current == v->pool[slot]) v->current = NULL;
            v->pool[slot] = NULL;
            fipsd_stats_view *fresh = calloc(1, sizeof *fresh);
            if (!fresh) return NULL;
            atomic_init(&fresh->refs, 1);
            v->pool[slot] = fresh;
        }
        w = v->pool[slot];
        if (!replay(v, w) && !sync(v, w, live) && !copy(v, w, live)) return NULL;
        v->current = w;
    }
    atomic_fetch_add_explicit(&w->refs, 1, memory_order_relaxed);
    return w;
}
const fips_stats_history *fipsd_stats_view_history(const fipsd_stats_view *view) { return &view->history; }
void fipsd_stats_view_release(fipsd_stats_view *view)
{
    if (view && atomic_fetch_sub_explicit(&view->refs, 1, memory_order_acq_rel) == 1) {
        free(view->storage); free(view);
    }
}
