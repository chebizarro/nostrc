#include <fips/node/stats_history.h>
#include <math.h>
#include <string.h>

const fips_metric_info fips_metrics[FIPS_METRIC_COUNT] = {
    {"mesh_size", "nodes", FIPS_AGG_LAST, false},
    {"tree_depth", "hops", FIPS_AGG_LAST, false},
    {"peer_count", "peers", FIPS_AGG_LAST, false},
    {"parent_switches", "events/s", FIPS_AGG_SUM, true},
    {"bytes_in", "bytes/s", FIPS_AGG_MEAN, true},
    {"bytes_out", "bytes/s", FIPS_AGG_MEAN, true},
    {"packets_in", "packets/s", FIPS_AGG_MEAN, true},
    {"packets_out", "packets/s", FIPS_AGG_MEAN, true},
    {"loss_rate", "fraction", FIPS_AGG_MEAN, false},
    {"active_sessions", "sessions", FIPS_AGG_LAST, false}
};
const fips_metric_info fips_peer_metrics[FIPS_PEER_METRIC_COUNT] = {
    {"srtt_ms", "ms", FIPS_AGG_MEAN, false},
    {"loss_rate", "fraction", FIPS_AGG_MEAN, false},
    {"bytes_in", "bytes/s", FIPS_AGG_MEAN, true},
    {"bytes_out", "bytes/s", FIPS_AGG_MEAN, true},
    {"packets_in", "packets/s", FIPS_AGG_MEAN, true},
    {"packets_out", "packets/s", FIPS_AGG_MEAN, true},
    {"ecn_ce", "events/s", FIPS_AGG_SUM, true}
};

fips_status fips_metric_parse(const char *name, fips_metric *out)
{
    for (int i = 0; i < FIPS_METRIC_COUNT; ++i)
        if (strcmp(name, fips_metrics[i].name) == 0) { *out = (fips_metric)i; return FIPS_OK; }
    return FIPS_ERR_RANGE;
}

fips_status fips_peer_metric_parse(const char *name, fips_peer_metric *out)
{
    for (int i = 0; i < FIPS_PEER_METRIC_COUNT; ++i)
        if (strcmp(name, fips_peer_metrics[i].name) == 0) { *out = (fips_peer_metric)i; return FIPS_OK; }
    return FIPS_ERR_RANGE;
}

fips_status fips_granularity_parse(const char *name, fips_granularity *out)
{
    if (strcmp(name, "1s") == 0) *out = FIPS_GRANULARITY_FAST;
    else if (strcmp(name, "1m") == 0) *out = FIPS_GRANULARITY_SLOW;
    else return FIPS_ERR_RANGE;
    return FIPS_OK;
}

static void ring_init(fips_stats_ring *r, fips_aggregation agg)
{
    memset(r, 0, sizeof(*r));
    r->last = NAN;
    r->aggregation = agg;
}

static void push(double *values, size_t cap, size_t *start, size_t *len, double v)
{
    if (*len == cap) {
        values[*start] = v;
        *start = (*start + 1) % cap;
    } else {
        values[(*start + *len) % cap] = v;
        ++*len;
    }
}

static void push_fast(fips_stats_ring *r, double v)
{
    push(r->fast, FIPS_FAST_RING_CAPACITY, &r->fast_start, &r->fast_len, v);
    ++r->total;
    if (!isnan(v)) { r->sum += v; ++r->count; r->last = v; }
}

static void flush_slow(fips_stats_ring *r)
{
    if (r->total != 0) {
        double v = NAN;
        if (r->count != 0) {
            switch (r->aggregation) {
            case FIPS_AGG_LAST: v = r->last; break;
            case FIPS_AGG_SUM: v = r->sum; break;
            case FIPS_AGG_MEAN: v = r->sum / (double)r->count; break;
            }
        }
        push(r->slow, FIPS_SLOW_RING_CAPACITY, &r->slow_start, &r->slow_len, v);
    }
    r->sum = 0;
    r->count = r->total = 0;
    r->last = NAN;
}

static double delta(fips_stats_ring *r, uint64_t total, bool peer)
{
    double v = peer ? NAN : 0.0;
    if (r->has_prev && total >= r->prev_total) v = (double)(total - r->prev_total);
    r->prev_total = total;
    r->has_prev = true;
    return v;
}

/* Peers live in stable slots; peers[r].by_addr names the slot (and address)
 * of the r-th by address. The index travels with the slots it is stored in,
 * so a peer's payload is only ever copied with copy_payload, which leaves it. */
static fips_peer_stats *ranked(const fips_stats_history *h, size_t rank)
{
    return &h->peers[h->peers[rank].by_addr.slot];
}

static void set_rank(fips_stats_history *h, size_t rank, size_t slot)
{
    h->peers[rank].by_addr.slot = slot;
    memcpy(h->peers[rank].by_addr.addr, h->peers[slot].addr, 16);
}

static void copy_payload(fips_peer_stats *to, const fips_peer_stats *from)
{
    fips_peer_rank by_addr = to->by_addr;
    *to = *from;
    to->by_addr = by_addr;
}

/* The rank addr has or would take among the first n. */
static size_t peer_position_upto(const fips_stats_history *h, size_t n, const uint8_t addr[16])
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (memcmp(h->peers[mid].by_addr.addr, addr, 16) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static size_t peer_position(const fips_stats_history *h, const uint8_t addr[16])
{
    return peer_position_upto(h, h->peer_count, addr);
}

const fips_peer_stats *fips_stats_history_peer(const fips_stats_history *h, const uint8_t addr[16])
{
    size_t i = peer_position(h, addr);
    return i < h->peer_count && memcmp(h->peers[i].by_addr.addr, addr, 16) == 0 ? ranked(h, i) : NULL;
}

const fips_peer_stats *fips_stats_history_peer_at(const fips_stats_history *h, size_t rank)
{
    return rank < h->peer_count ? ranked(h, rank) : NULL;
}

/* Drop the peers gone() names: the address order closes up, then each hole
 * below the new count takes one surviving peer from above it. Cost: one pass
 * over the index plus one peer copy per hole, not a shift of every later peer. */
typedef bool (*gone_fn)(const fips_peer_stats *p, const void *ctx);

static void remove_peers(fips_stats_history *h, gone_fn gone, const void *ctx)
{
    size_t kept = 0;
    for (size_t r = 0; r < h->peer_count; ++r) {
        if (gone(&h->peers[h->peers[r].by_addr.slot], ctx)) continue;
        if (kept != r) h->peers[kept].by_addr = h->peers[r].by_addr; /* untouched until one is gone */
        ++kept;
    }
    for (size_t hole = 0, from = kept; from < h->peer_count; ++from) {
        if (gone(&h->peers[from], ctx)) continue;
        while (!gone(&h->peers[hole], ctx)) ++hole; /* one hole below kept per survivor above */
        size_t rank = peer_position_upto(h, kept, h->peers[from].addr);
        copy_payload(&h->peers[hole], &h->peers[from]);
        h->peers[rank].by_addr.slot = hole;
        ++hole;
    }
    h->peer_count = kept;
}

void fips_stats_history_init(fips_stats_history *h, fips_peer_stats *storage, size_t capacity)
{
    for (int i = 0; i < FIPS_METRIC_COUNT; ++i) ring_init(&h->rings[i], fips_metrics[i].aggregation);
    h->peers = storage;
    h->peer_count = 0;
    h->peer_capacity = capacity;
    h->fast_pushes = 0;
    h->last_tick = (fips_instant){0};
    h->has_data = false;
}

static void peer_init(fips_peer_stats *p, const uint8_t addr[16], fips_instant now, uint64_t pushes)
{
    memcpy(p->addr, addr, 16);
    p->first_seen = p->last_contact = now;
    p->seen_tick = false;
    p->born = pushes;
    for (int i = 0; i < FIPS_PEER_METRIC_COUNT; ++i) {
        fips_stats_ring *r = &p->rings[i];
        ring_init(r, fips_peer_metrics[i].aggregation);
        r->fast_len = pushes < FIPS_FAST_RING_CAPACITY ? (size_t)pushes : FIPS_FAST_RING_CAPACITY;
        uint64_t slow = pushes / FIPS_DOWNSAMPLE_FACTOR;
        r->slow_len = slow < FIPS_SLOW_RING_CAPACITY ? (size_t)slow : FIPS_SLOW_RING_CAPACITY;
        for (size_t j = 0; j < r->fast_len; ++j) r->fast[j] = NAN;
        for (size_t j = 0; j < r->slow_len; ++j) r->slow[j] = NAN;
        /* Backfill does not enter the current downsample accumulator. */
    }
}

static bool silent_too_long(const fips_peer_stats *p, const void *ctx)
{
    const fips_instant *now = ctx;
    return now->ops->saturating_duration_since(*now, p->last_contact).secs >= FIPS_PEER_EVICTION_SECS;
}

void fips_stats_history_cleanup(fips_stats_history *h, fips_instant now)
{
    remove_peers(h, silent_too_long, &now);
}

fips_status fips_stats_history_tick(fips_stats_history *h, fips_instant now,
    const fips_stats_snapshot *s, const fips_peer_snapshot *peers, size_t count)
{
    size_t needed = h->peer_count;
    for (size_t i = 0; i < count; ++i) {
        if (fips_stats_history_peer(h, peers[i].node_addr) != NULL) continue;
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j)
            if (memcmp(peers[i].node_addr, peers[j].node_addr, 16) == 0) { duplicate = true; break; }
        if (!duplicate) {
            if (needed == h->peer_capacity) return FIPS_ERR_RANGE;
            ++needed;
        }
    }
    double values[FIPS_METRIC_COUNT] = {
        s->has_mesh_size ? (double)s->mesh_size : 0.0, (double)s->tree_depth, (double)s->peer_count,
        delta(&h->rings[FIPS_METRIC_PARENT_SWITCHES], s->parent_switches_total, false),
        delta(&h->rings[FIPS_METRIC_BYTES_IN], s->bytes_in_total, false),
        delta(&h->rings[FIPS_METRIC_BYTES_OUT], s->bytes_out_total, false),
        delta(&h->rings[FIPS_METRIC_PACKETS_IN], s->packets_in_total, false),
        delta(&h->rings[FIPS_METRIC_PACKETS_OUT], s->packets_out_total, false),
        s->loss_rate, (double)s->active_sessions
    };
    for (int i = 0; i < FIPS_METRIC_COUNT; ++i) push_fast(&h->rings[i], values[i]);
    for (size_t i = 0; i < h->peer_count; ++i) h->peers[i].seen_tick = false;
    for (size_t i = 0; i < count; ++i) {
        const fips_peer_snapshot *ps = &peers[i];
        size_t pos = peer_position(h, ps->node_addr);
        if (pos == h->peer_count || memcmp(h->peers[pos].by_addr.addr, ps->node_addr, 16) != 0) {
            /* A new peer takes the next free slot; only the index shifts. */
            size_t slot = h->peer_count;
            for (size_t r = h->peer_count; r > pos; --r) h->peers[r].by_addr = h->peers[r - 1].by_addr;
            peer_init(&h->peers[slot], ps->node_addr, now, h->fast_pushes);
            set_rank(h, pos, slot);
            ++h->peer_count;
        }
        fips_peer_stats *p = ranked(h, pos);
        p->seen_tick = true;
        p->last_contact = now;
        double pv[FIPS_PEER_METRIC_COUNT] = {
            ps->has_srtt_ms ? ps->srtt_ms : NAN, ps->has_loss_rate ? ps->loss_rate : NAN,
            delta(&p->rings[FIPS_PEER_BYTES_IN], ps->bytes_in_total, true),
            delta(&p->rings[FIPS_PEER_BYTES_OUT], ps->bytes_out_total, true),
            delta(&p->rings[FIPS_PEER_PACKETS_IN], ps->packets_in_total, true),
            delta(&p->rings[FIPS_PEER_PACKETS_OUT], ps->packets_out_total, true),
            delta(&p->rings[FIPS_PEER_ECN_CE], ps->ecn_ce_total, true)
        };
        for (int j = 0; j < FIPS_PEER_METRIC_COUNT; ++j) push_fast(&p->rings[j], pv[j]);
    }
    for (size_t i = 0; i < h->peer_count; ++i) {
        if (h->peers[i].seen_tick) continue;
        for (int j = 0; j < FIPS_PEER_METRIC_COUNT; ++j) {
            fips_stats_ring *r = &h->peers[i].rings[j];
            push_fast(r, NAN);
            if (fips_peer_metrics[j].is_counter) r->has_prev = false;
        }
    }
    /* Modulo cadence remains defined even at the u64 counter boundary. */
    if (h->fast_pushes == UINT64_MAX)
        h->fast_pushes = FIPS_SLOW_RING_CAPACITY * FIPS_DOWNSAMPLE_FACTOR +
                         UINT64_MAX % FIPS_DOWNSAMPLE_FACTOR;
    ++h->fast_pushes;
    if (h->fast_pushes % FIPS_DOWNSAMPLE_FACTOR == 0) {
        for (int i = 0; i < FIPS_METRIC_COUNT; ++i) flush_slow(&h->rings[i]);
        for (size_t i = 0; i < h->peer_count; ++i)
            for (int j = 0; j < FIPS_PEER_METRIC_COUNT; ++j) flush_slow(&h->peers[i].rings[j]);
    }
    fips_stats_history_cleanup(h, now);
    h->last_tick = now;
    h->has_data = true;
    return FIPS_OK;
}

/* Copy the last n samples written to a ring of capacity cap, wherever they
 * sit, or the whole array when n reaches the capacity or the ring cannot hold n. */
static void copy_tail(double *dst, const double *src, size_t cap, size_t start, size_t len, uint64_t n)
{
    if (n >= cap || n > len) { memcpy(dst, src, cap * sizeof *dst); return; }
    /* The last n samples are contiguous from `at`, wrapping once at most. */
    size_t at = (start + len - (size_t)n) % cap, first = cap - at;
    if (first > n) first = (size_t)n;
    memcpy(dst + at, src + at, first * sizeof *dst);
    if (n > first) memcpy(dst, src, ((size_t)n - first) * sizeof *dst);
}

/* dst is the ring as it was kf fast pushes (ks slow pushes) ago. */
static void sync_ring(fips_stats_ring *dst, const fips_stats_ring *src, bool delta, uint64_t kf, uint64_t ks)
{
    if (!delta) { *dst = *src; return; }
    if (kf) copy_tail(dst->fast, src->fast, FIPS_FAST_RING_CAPACITY, src->fast_start, src->fast_len, kf);
    if (ks) copy_tail(dst->slow, src->slow, FIPS_SLOW_RING_CAPACITY, src->slow_start, src->slow_len, ks);
    dst->fast_start = src->fast_start; dst->fast_len = src->fast_len;
    dst->slow_start = src->slow_start; dst->slow_len = src->slow_len;
    dst->sum = src->sum; dst->last = src->last;
    dst->count = src->count; dst->total = src->total;
    dst->prev_total = src->prev_total; dst->has_prev = src->has_prev;
    dst->aggregation = src->aggregation;
}

static bool same_peer(const fips_peer_stats *a, const fips_peer_stats *b)
{
    return a->born == b->born && memcmp(a->addr, b->addr, 16) == 0;
}

static bool not_in(const fips_peer_stats *p, const void *ctx)
{
    const fips_peer_stats *match = fips_stats_history_peer(ctx, p->addr);
    return !match || !same_peer(match, p);
}

fips_status fips_stats_history_sync(fips_stats_history *dst, const fips_stats_history *src)
{
    if (src->peer_count > dst->peer_capacity) return FIPS_ERR_RANGE;
    /* Every ring of a history advances once per tick, so one elapsed count
     * serves them all; a dst ahead of src is no earlier state: copy rings whole. */
    bool delta = dst->fast_pushes <= src->fast_pushes;
    uint64_t kf = delta ? src->fast_pushes - dst->fast_pushes : 0;
    uint64_t ks = delta ? src->fast_pushes / FIPS_DOWNSAMPLE_FACTOR - dst->fast_pushes / FIPS_DOWNSAMPLE_FACTOR : 0;
    for (int i = 0; i < FIPS_METRIC_COUNT; ++i) sync_ring(&dst->rings[i], &src->rings[i], delta, kf, ks);

    /* Peers dst holds that src no longer has, or holds as another peer of the
     * same address, go first; the rest keep their slots and only advance. */
    remove_peers(dst, not_in, src);
    /* dst's survivors are a subset of src's peers in the same order. Rebuild
     * dst's order from the back (rank r is written only once the ranks below
     * it that are still to be read are all below r); a peer src has and dst
     * lacks is copied whole into the next free slot. */
    size_t held = dst->peer_count, fresh = held;
    for (size_t r = src->peer_count; r-- > 0;) {
        const fips_peer_stats *sp = ranked(src, r);
        size_t slot;
        if (held > 0 && same_peer(ranked(dst, held - 1), sp)) {
            slot = dst->peers[--held].by_addr.slot;
            fips_peer_stats *dp = &dst->peers[slot];
            for (int j = 0; j < FIPS_PEER_METRIC_COUNT; ++j) sync_ring(&dp->rings[j], &sp->rings[j], delta, kf, ks);
            dp->first_seen = sp->first_seen; dp->last_contact = sp->last_contact; dp->seen_tick = sp->seen_tick;
        } else {
            slot = fresh++;
            copy_payload(&dst->peers[slot], sp);
        }
        set_rank(dst, r, slot);
    }
    dst->peer_count = src->peer_count;
    dst->fast_pushes = src->fast_pushes;
    dst->last_tick = src->last_tick;
    dst->has_data = src->has_data;
    return FIPS_OK;
}

static fips_status build_series(const fips_stats_ring *r, const fips_metric_info *info,
    fips_duration window, fips_granularity granularity, double *buffer, size_t capacity,
    fips_stats_series *out)
{
    bool fast = granularity == FIPS_GRANULARITY_FAST;
    size_t cap = fast ? FIPS_FAST_RING_CAPACITY : FIPS_SLOW_RING_CAPACITY;
    uint64_t seconds = fast ? 1 : 60;
    uint64_t requested = window.secs / seconds;
    size_t want = requested < cap ? (size_t)requested : cap;
    if (capacity < want) return FIPS_ERR_RANGE;
    size_t len = fast ? r->fast_len : r->slow_len;
    size_t start = fast ? r->fast_start : r->slow_start;
    const double *source = fast ? r->fast : r->slow;
    size_t take = len < want ? len : want;
    for (size_t i = 0; i < want - take; ++i) buffer[i] = NAN;
    for (size_t i = 0; i < take; ++i)
        buffer[want - take + i] = source[(start + len - take + i) % cap];
    *out = (fips_stats_series){info->name, info->unit, seconds, buffer, want};
    return FIPS_OK;
}

fips_status fips_stats_history_query(const fips_stats_history *h, fips_metric metric,
    fips_duration window, fips_granularity granularity, double *buffer, size_t capacity,
    fips_stats_series *out)
{
    return build_series(&h->rings[metric], &fips_metrics[metric], window, granularity, buffer, capacity, out);
}

fips_status fips_stats_history_peer_query(const fips_stats_history *h, const uint8_t addr[16],
    fips_peer_metric metric, fips_duration window, fips_granularity granularity,
    double *buffer, size_t capacity, fips_stats_series *out)
{
    const fips_peer_stats *p = fips_stats_history_peer(h, addr);
    if (p == NULL) return FIPS_ERR_RANGE;
    return build_series(&p->rings[metric], &fips_peer_metrics[metric], window, granularity, buffer, capacity, out);
}

bool fips_stats_history_latest(const fips_stats_history *h, fips_metric metric, double *out)
{
    const fips_stats_ring *r = &h->rings[metric];
    if (r->fast_len == 0) return false;
    *out = r->fast[(r->fast_start + r->fast_len - 1) % FIPS_FAST_RING_CAPACITY];
    return true;
}

fips_status fips_stats_history_recent(const fips_stats_history *h, fips_metric metric,
    size_t n, double *buffer, size_t capacity, size_t *out_len)
{
    const fips_stats_ring *r = &h->rings[metric];
    if (n > r->fast_len) n = r->fast_len;
    if (n > capacity) return FIPS_ERR_RANGE;
    for (size_t i = 0; i < n; ++i)
        buffer[i] = r->fast[(r->fast_start + r->fast_len - n + i) % FIPS_FAST_RING_CAPACITY];
    *out_len = n;
    return FIPS_OK;
}

void fips_stats_series_walk(const fips_stats_series *series, fips_series_visitor visit, void *ctx)
{
    for (size_t i = 0; i < series->len; ++i) {
        bool is_null = !isfinite(series->values[i]);
        visit(ctx, i, is_null, is_null ? 0.0 : series->values[i]);
    }
}
