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

static size_t peer_position(const fips_stats_history *h, const uint8_t addr[16])
{
    size_t lo = 0, hi = h->peer_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (memcmp(h->peers[mid].addr, addr, 16) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

const fips_peer_stats *fips_stats_history_peer(const fips_stats_history *h, const uint8_t addr[16])
{
    size_t i = peer_position(h, addr);
    return i < h->peer_count && memcmp(h->peers[i].addr, addr, 16) == 0 ? &h->peers[i] : NULL;
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

void fips_stats_history_cleanup(fips_stats_history *h, fips_instant now)
{
    size_t n = 0;
    for (size_t i = 0; i < h->peer_count; ++i) {
        if (now.ops->saturating_duration_since(now, h->peers[i].last_contact).secs < FIPS_PEER_EVICTION_SECS) {
            if (n != i) h->peers[n] = h->peers[i];
            ++n;
        }
    }
    h->peer_count = n;
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
        if (pos == h->peer_count || memcmp(h->peers[pos].addr, ps->node_addr, 16) != 0) {
            memmove(h->peers + pos + 1, h->peers + pos, (h->peer_count - pos) * sizeof(h->peers[0]));
            ++h->peer_count;
            peer_init(&h->peers[pos], ps->node_addr, now, h->fast_pushes);
        }
        fips_peer_stats *p = &h->peers[pos];
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
