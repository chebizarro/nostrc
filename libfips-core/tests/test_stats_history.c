#include "fakes/host.h"
#include <fips/node/stats_history.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); return 1; } } while (0)
#define FAST FIPS_GRANULARITY_FAST
#define SLOW FIPS_GRANULARITY_SLOW
static fips_stats_history h;
static fips_peer_stats storage[3];
static double buffer[FIPS_FAST_RING_CAPACITY];
static fips_stats_series series;
static fips_instant secs(uint64_t s)
{
    fips_test_clock state = {0};
    fips_clock clock = fips_test_clock_adapter(&state);
    (void)fips_test_clock_advance(&state, s * 1000);
    return clock.now(clock.ctx);
}
static fips_stats_snapshot snap(uint64_t i)
{
    return (fips_stats_snapshot){true, 10 + i, 2, 3, i, 100 * i, 200 * i,
                                 i, 2 * i, .01 * (double)i, i};
}
static fips_peer_snapshot peer(unsigned char tag, uint64_t s, uint64_t i)
{
    fips_peer_snapshot p = {0};
    memset(p.node_addr, tag, 16);
    p.last_seen = secs(s);
    p.has_srtt_ms = p.has_loss_rate = true;
    p.srtt_ms = 10 + (double)i; p.loss_rate = .01 * (double)i;
    p.bytes_in_total = 50 * i; p.bytes_out_total = 75 * i;
    p.packets_in_total = i; p.packets_out_total = 2 * i;
    return p;
}
static void init(void) { fips_stats_history_init(&h, storage, 3); }
static bool all_nan(size_t start, size_t end)
{
    for (size_t i = start; i < end; ++i) if (!isnan(buffer[i])) return false;
    return true;
}
#define TICK(time, index) do { fips_stats_snapshot s_ = snap(index); \
    CHECK(fips_stats_history_tick(&h, secs(time), &s_, NULL, 0) == FIPS_OK); } while (0)
#define PTICK(time, index, tag) do { fips_stats_snapshot s_ = snap(index); \
    fips_peer_snapshot p_ = peer(tag, time, index); \
    CHECK(fips_stats_history_tick(&h, secs(time), &s_, &p_, 1) == FIPS_OK); } while (0)
#define Q(metric, window, gran) CHECK(fips_stats_history_query(&h, FIPS_METRIC_##metric, \
    (fips_duration){window, 0}, gran, buffer, FIPS_FAST_RING_CAPACITY, &series) == FIPS_OK)
#define PQ(tag, metric, window, gran) do { uint8_t a_[16]; memset(a_, tag, 16); \
    CHECK(fips_stats_history_peer_query(&h, a_, FIPS_PEER_##metric, (fips_duration){window, 0}, \
    gran, buffer, FIPS_FAST_RING_CAPACITY, &series) == FIPS_OK); } while (0)
/* node/tests/stats_history.rs: 22 upstream cases, same names. */
static int push_and_query_fast_ring(void)
{
    init(); for (uint64_t i = 0; i < 10; ++i) TICK(i, i);
    Q(MESH_SIZE, 5, FAST);
    CHECK(series.len == 5 && series.granularity_seconds == 1);
    for (size_t i = 0; i < 5; ++i) CHECK(buffer[i] == 15 + (double)i);
    return 0;
}
static int fast_ring_wraps_at_capacity(void)
{
    init(); for (uint64_t i = 0; i < 3610; ++i) TICK(i, i);
    Q(MESH_SIZE, 7200, FAST);
    CHECK(series.len == 3600 && buffer[0] == 20 && buffer[3599] == 3619);
    return 0;
}
static int delta_for_counter_metric(void)
{
    const uint64_t totals[] = {0, 0, 2, 5}; init();
    for (size_t i = 0; i < 4; ++i) {
        fips_stats_snapshot s = snap(i); s.parent_switches_total = totals[i];
        CHECK(fips_stats_history_tick(&h, secs(i), &s, NULL, 0) == FIPS_OK);
    }
    Q(PARENT_SWITCHES, 10, FAST);
    CHECK(series.len == 10 && all_nan(0, 6));
    CHECK(buffer[6] == 0 && buffer[7] == 0 && buffer[8] == 2 && buffer[9] == 3);
    return 0;
}
static int downsample_last_aggregation(void)
{
    init(); for (uint64_t i = 0; i < 60; ++i) TICK(i, i);
    Q(MESH_SIZE, 300, SLOW);
    CHECK(series.len == 5 && all_nan(0, 4) && buffer[4] == 69 && series.granularity_seconds == 60);
    return 0;
}
static int downsample_mean_aggregation(void)
{
    init(); for (uint64_t i = 0; i < 60; ++i) TICK(i, i);
    Q(LOSS_RATE, 60, SLOW); CHECK(series.len == 1 && fabs(buffer[0] - .295) < 1e-9);
    return 0;
}
static int downsample_sum_aggregation(void)
{
    init(); for (uint64_t i = 0; i < 60; ++i) {
        fips_stats_snapshot s = snap(0); s.parent_switches_total = i;
        CHECK(fips_stats_history_tick(&h, secs(i), &s, NULL, 0) == FIPS_OK);
    }
    Q(PARENT_SWITCHES, 60, SLOW); CHECK(series.len == 1 && buffer[0] == 59);
    return 0;
}
static int query_pads_front_with_nan_when_ring_is_short(void)
{
    init(); for (uint64_t i = 0; i < 3; ++i) TICK(i, i);
    Q(MESH_SIZE, 10, FAST); CHECK(series.len == 10 && all_nan(0, 7));
    CHECK(buffer[7] == 10 && buffer[8] == 11 && buffer[9] == 12);
    return 0;
}
static int fast_query_young_ring_returns_full_hour_with_leading_nan(void)
{
    init(); for (uint64_t i = 0; i < 300; ++i) TICK(i, i);
    Q(MESH_SIZE, 3600, FAST);
    CHECK(series.len == 3600 && all_nan(0, 3300) && buffer[3300] == 10 && buffer[3599] == 309);
    return 0;
}
static int slow_query_young_ring_returns_full_day_with_leading_nan(void)
{
    init(); for (uint64_t i = 0; i < 1800; ++i) TICK(i, i);
    Q(MESH_SIZE, 86400, SLOW); CHECK(series.len == 1440 && all_nan(0, 1410));
    for (size_t i = 1410; i < 1440; ++i) CHECK(!isnan(buffer[i]));
    return 0;
}
static int metric_parse_roundtrip(void)
{
    for (int i = 0; i < FIPS_METRIC_COUNT; ++i) {
        fips_metric m;
        CHECK(fips_metric_parse(fips_metrics[i].name, &m) == FIPS_OK && m == (fips_metric)i);
    }
    fips_metric m = FIPS_METRIC_MESH_SIZE;
    CHECK(fips_metric_parse("bogus", &m) == FIPS_ERR_RANGE && m == FIPS_METRIC_MESH_SIZE);
    return 0;
}
static int peer_metric_parse_roundtrip(void)
{
    for (int i = 0; i < FIPS_PEER_METRIC_COUNT; ++i) {
        fips_peer_metric m;
        CHECK(fips_peer_metric_parse(fips_peer_metrics[i].name, &m) == FIPS_OK && m == (fips_peer_metric)i);
    }
    fips_peer_metric m = FIPS_PEER_SRTT_MS;
    CHECK(fips_peer_metric_parse("bogus", &m) == FIPS_ERR_RANGE && m == FIPS_PEER_SRTT_MS);
    return 0;
}
static int granularity_parse(void)
{
    fips_granularity g;
    CHECK(fips_granularity_parse("1s", &g) == FIPS_OK && g == FAST);
    CHECK(fips_granularity_parse("1m", &g) == FIPS_OK && g == SLOW);
    CHECK(fips_granularity_parse("1h", &g) == FIPS_ERR_RANGE && g == SLOW);
    return 0;
}
static int latest_and_recent(void)
{
    init(); for (uint64_t i = 0; i < 5; ++i) TICK(i, i);
    double v; size_t n;
    CHECK(fips_stats_history_latest(&h, FIPS_METRIC_MESH_SIZE, &v) && v == 14);
    CHECK(fips_stats_history_recent(&h, FIPS_METRIC_MESH_SIZE, 3, buffer, 3600, &n) == FIPS_OK);
    CHECK(n == 3 && buffer[0] == 12 && buffer[1] == 13 && buffer[2] == 14);
    CHECK(fips_stats_history_recent(&h, FIPS_METRIC_MESH_SIZE, 100, buffer, 3600, &n) == FIPS_OK && n == 5);
    return 0;
}
static int active_sessions_is_sampled_as_gauge(void)
{
    init(); for (uint64_t i = 0; i < 3; ++i) {
        fips_stats_snapshot s = snap(i); s.active_sessions = 10 + i;
        CHECK(fips_stats_history_tick(&h, secs(i), &s, NULL, 0) == FIPS_OK);
    }
    Q(ACTIVE_SESSIONS, 5, FAST); CHECK(series.len == 5 && all_nan(0, 2));
    CHECK(buffer[2] == 10 && buffer[3] == 11 && buffer[4] == 12);
    return 0;
}
static int new_peer_backfills_nan_to_align_with_node_rings(void)
{
    init(); for (uint64_t i = 0; i < 5; ++i) TICK(i, i);
    PTICK(5, 5, 1);
    PQ(1, SRTT_MS, 60, FAST); CHECK(series.len == 60 && all_nan(0, 59) && buffer[59] == 15);
    return 0;
}
static int absent_peer_gets_nan_sample(void)
{
    init(); for (uint64_t i = 0; i < 3; ++i) PTICK(i, i, 1);
    for (uint64_t i = 3; i < 5; ++i) TICK(i, i);
    PQ(1, SRTT_MS, 60, FAST);
    CHECK(series.len == 60 && all_nan(0, 55) && buffer[55] == 10 && buffer[57] == 12 && all_nan(58, 60));
    return 0;
}
static int counter_decrease_emits_nan_and_rebaselines(void)
{
    const uint64_t totals[] = {100, 200, 300, 50, 80}; init();
    for (size_t i = 0; i < 5; ++i) {
        fips_stats_snapshot s = snap(i); fips_peer_snapshot p = peer(1, i, i);
        p.bytes_in_total = totals[i];
        CHECK(fips_stats_history_tick(&h, secs(i), &s, &p, 1) == FIPS_OK);
    }
    PQ(1, BYTES_IN, 60, FAST);
    CHECK(series.len == 60 && all_nan(0, 56));
    CHECK(buffer[56] == 100 && buffer[57] == 100 && isnan(buffer[58]) && buffer[59] == 30);
    return 0;
}
static int peer_eviction_fires_after_24h_of_silence(void)
{
    init(); PTICK(0, 0, 1);
    uint8_t a[16]; memset(a, 1, 16); CHECK(fips_stats_history_peer(&h, a) != NULL);
    for (uint64_t i = 1; i <= FIPS_PEER_EVICTION_SECS / 60; ++i) TICK(i * 60, i);
    CHECK(fips_stats_history_peer(&h, a) == NULL && h.peer_count == 0);
    return 0;
}
static int a_peer_silent_for_just_under_the_eviction_window_is_kept(void)
{
    init(); PTICK(0, 0, 1); TICK(FIPS_PEER_EVICTION_SECS - 1, 1);
    uint8_t a[16]; memset(a, 1, 16); CHECK(fips_stats_history_peer(&h, a) != NULL);
    return 0;
}
static int nan_mean_downsample_skips_nan_samples(void)
{
    init(); for (uint64_t i = 0; i < 60; ++i) {
        if (i % 2 == 0) { PTICK(i, i, 1); } else { TICK(i, i); }
    }
    PQ(1, SRTT_MS, 60, SLOW); CHECK(series.len == 1 && fabs(buffer[0] - 39) < 1e-9);
    return 0;
}
static int all_nan_window_downsamples_to_nan(void)
{
    init(); PTICK(0, 0, 1); for (uint64_t i = 1; i <= 60; ++i) TICK(i, i);
    PQ(1, SRTT_MS, 300, SLOW); CHECK(series.len == 5 && all_nan(0, 4) && buffer[4] == 10);
    for (uint64_t i = 61; i <= 120; ++i) TICK(i, i);
    PQ(1, SRTT_MS, 300, SLOW);
    CHECK(series.len == 5 && all_nan(0, 3) && buffer[3] == 10 && isnan(buffer[4]));
    return 0;
}
/* A host-like serialization consumer: no JSON dependency in the core. */
typedef struct json_values { char text[256]; size_t used; } json_values;
static void emit_value(void *ctx, size_t i, bool is_null, double value)
{
    json_values *j = ctx;
    int n = snprintf(j->text + j->used, sizeof(j->text) - j->used,
                     is_null ? "%snull" : "%s%.1f", i ? "," : "", value);
    if (n > 0 && (size_t)n < sizeof(j->text) - j->used) j->used += (size_t)n;
}
static int nan_serializes_to_json_null(void)
{
    const double values[] = {1, NAN, 3};
    fips_stats_series s = {"srtt_ms", "ms", 1, values, 3};
    json_values j = {{0}, 0}; fips_stats_series_walk(&s, emit_value, &j);
    CHECK(strcmp(j.text, "1.0,null,3.0") == 0);
    CHECK(strcmp(s.metric, "srtt_ms") == 0 && strcmp(s.unit, "ms") == 0 && s.granularity_seconds == 1);
    const double infinities[] = {INFINITY, -INFINITY, NAN, 0};
    s.values = infinities; s.len = 4; j = (json_values){{0}, 0};
    fips_stats_series_walk(&s, emit_value, &j);
    CHECK(strcmp(j.text, "null,null,null,0.0") == 0);
    return 0;
}
/* Additional bounded-storage and lifecycle cases. */
static int slow_ring_wraps_at_capacity(void)
{
    init();
    for (uint64_t i = 0; i < (FIPS_SLOW_RING_CAPACITY + 2) * 60; ++i) TICK(i, i);
    Q(MESH_SIZE, UINT64_MAX, SLOW);
    CHECK(series.len == 1440 && buffer[0] == 189 && buffer[1439] == 86529);
    CHECK(h.rings[FIPS_METRIC_MESH_SIZE].slow_len == 1440);
    return 0;
}
static int empty_zero_window_and_failure_outputs(void)
{
    init(); double value = 42; size_t len = 42;
    CHECK(!h.has_data && h.peer_count == 0);
    CHECK(!fips_stats_history_latest(&h, FIPS_METRIC_MESH_SIZE, &value) && value == 42);
    Q(MESH_SIZE, 5, FAST); CHECK(all_nan(0, 5));
    CHECK(fips_stats_history_query(&h, FIPS_METRIC_MESH_SIZE, (fips_duration){0, 999999999},
          FAST, NULL, 0, &series) == FIPS_OK && series.len == 0);
    CHECK(fips_stats_history_recent(&h, FIPS_METRIC_MESH_SIZE, 0, NULL, 0, &len) == FIPS_OK && len == 0);
    buffer[0] = 42; series.metric = "sentinel"; series.len = 123;
    CHECK(fips_stats_history_query(&h, FIPS_METRIC_MESH_SIZE, (fips_duration){5, 0},
          FAST, buffer, 4, &series) == FIPS_ERR_RANGE);
    CHECK(buffer[0] == 42 && series.len == 123 && strcmp(series.metric, "sentinel") == 0);
    uint8_t absent[16] = {0};
    CHECK(fips_stats_history_peer_query(&h, absent, FIPS_PEER_SRTT_MS, (fips_duration){1, 0},
          FAST, buffer, 3600, &series) == FIPS_ERR_RANGE && buffer[0] == 42 && series.len == 123);
    TICK(0, 0); len = 42;
    CHECK(fips_stats_history_recent(&h, FIPS_METRIC_MESH_SIZE, 1, buffer, 0, &len) == FIPS_ERR_RANGE);
    CHECK(len == 42 && buffer[0] == 42 && h.has_data);
    Q(MESH_SIZE, 59, SLOW); CHECK(series.len == 0);
    return 0;
}
static int capacity_preflight_is_transactional_and_sorted(void)
{
    fips_stats_history_init(&h, storage, 2);
    fips_stats_snapshot s = snap(0);
    fips_peer_snapshot ps[3] = {peer(2, 0, 0), peer(1, 0, 0), peer(3, 0, 0)};
    CHECK(fips_stats_history_tick(&h, secs(0), &s, ps, 3) == FIPS_ERR_RANGE);
    CHECK(!h.has_data && h.fast_pushes == 0 && h.peer_count == 0 && h.rings[0].fast_len == 0);
    CHECK(fips_stats_history_tick(&h, secs(0), &s, ps, 2) == FIPS_OK);
    CHECK(h.peer_count == 2 && h.peers[0].addr[0] == 1 && h.peers[1].addr[0] == 2);
    s = snap(1);
    fips_peer_snapshot mixed[2] = {peer(2, 1, 1000), peer(3, 1, 1000)};
    CHECK(fips_stats_history_tick(&h, secs(1), &s, mixed, 2) == FIPS_ERR_RANGE);
    CHECK(h.peers[1].rings[FIPS_PEER_BYTES_IN].prev_total == 0);
    CHECK(h.peers[1].last_contact.ops->compare(h.peers[1].last_contact, secs(0)) == 0);
    CHECK(h.fast_pushes == 1 && h.last_tick.ops->compare(h.last_tick, secs(0)) == 0);
    CHECK(h.peers[0].rings[0].fast_len == 1 && h.rings[0].fast_len == 1);
    fips_stats_history_cleanup(&h, secs(86400)); CHECK(h.peer_count == 0);
    CHECK(fips_stats_history_tick(&h, secs(86400), &s, ps + 2, 1) == FIPS_OK && h.peer_count == 1);
    fips_stats_history_init(&h, NULL, 0);
    CHECK(fips_stats_history_tick(&h, secs(0), &s, ps, 1) == FIPS_ERR_RANGE && !h.has_data);
    TICK(0, 0);
    return 0;
}
static int gaps_rebaseline_every_counter_and_node_saturates(void)
{
    init(); PTICK(0, 0, 1); PTICK(1, 1, 1); TICK(2, 2); PTICK(3, 1000, 1); PTICK(4, 1001, 1);
    const fips_peer_metric counters[] = {FIPS_PEER_BYTES_IN, FIPS_PEER_BYTES_OUT,
        FIPS_PEER_PACKETS_IN, FIPS_PEER_PACKETS_OUT, FIPS_PEER_ECN_CE};
    const double deltas[] = {50, 75, 1, 2, 0}; uint8_t a[16]; memset(a, 1, 16);
    for (size_t i = 0; i < 5; ++i) {
        CHECK(fips_stats_history_peer_query(&h, a, counters[i], (fips_duration){5, 0},
              FAST, buffer, 3600, &series) == FIPS_OK);
        CHECK(isnan(buffer[0]) && buffer[1] == deltas[i] && isnan(buffer[2]) &&
              isnan(buffer[3]) && buffer[4] == deltas[i]);
    }
    TICK(5, 0); Q(BYTES_IN, 1, FAST); CHECK(buffer[0] == 0);
    fips_stats_snapshot s = snap(0); s.has_mesh_size = false;
    CHECK(fips_stats_history_tick(&h, secs(6), &s, NULL, 0) == FIPS_OK);
    Q(MESH_SIZE, 1, FAST); CHECK(buffer[0] == 0);
    return 0;
}
static int peer_metadata_optional_gauges_and_backfill(void)
{
    init(); for (uint64_t i = 0; i < 125; ++i) TICK(i, i);
    fips_stats_snapshot s = snap(125); fips_peer_snapshot p = peer(1, 0, 125);
    p.has_srtt_ms = p.has_loss_rate = false;
    CHECK(fips_stats_history_tick(&h, secs(125), &s, &p, 1) == FIPS_OK);
    const fips_peer_stats *tracked = fips_stats_history_peer(&h, p.node_addr);
    CHECK(tracked && tracked->first_seen.ops->compare(tracked->first_seen, secs(125)) == 0);
    CHECK(tracked->last_contact.ops->compare(tracked->last_contact, secs(125)) == 0);
    CHECK(tracked->rings[0].fast_len == 126 && tracked->rings[0].slow_len == 2);
    PQ(1, SRTT_MS, 126, FAST); CHECK(all_nan(0, 126));
    PQ(1, LOSS_RATE, 180, SLOW); CHECK(all_nan(0, 3));
    for (uint64_t i = 126; i < 180; ++i) PTICK(i, i, 1);
    PQ(1, SRTT_MS, 180, SLOW);
    CHECK(all_nan(0, 2) && fabs(buffer[2] - 162.5) < 1e-9);
    tracked = fips_stats_history_peer(&h, p.node_addr);
    CHECK(tracked->first_seen.ops->compare(tracked->first_seen, secs(125)) == 0);
    CHECK(tracked->last_contact.ops->compare(tracked->last_contact, secs(179)) == 0);
    TICK(178, 180); CHECK(h.peer_count == 1); /* elapsed time saturates */
    TICK(179 + 86400, 181); CHECK(h.peer_count == 0);
    PTICK(179 + 86401, 182, 1);
    tracked = fips_stats_history_peer(&h, p.node_addr);
    CHECK(tracked->first_seen.ops->compare(tracked->first_seen, secs(179 + 86401)) == 0);
    return 0;
}
static int peer_sum_last_nan_and_duplicate_inputs(void)
{
    init();
    for (uint64_t i = 0; i < 60; ++i) {
        fips_stats_snapshot s = snap(i); fips_peer_snapshot p = peer(1, i, i);
        p.ecn_ce_total = i * 2;
        if (i == 59) { s.loss_rate = NAN; s.has_mesh_size = false; }
        CHECK(fips_stats_history_tick(&h, secs(i), &s, &p, 1) == FIPS_OK);
    }
    PQ(1, ECN_CE, 60, SLOW); CHECK(buffer[0] == 118);
    Q(MESH_SIZE, 60, SLOW); CHECK(buffer[0] == 0);
    Q(LOSS_RATE, 60, SLOW); CHECK(fabs(buffer[0] - .29) < 1e-9);
    init(); fips_stats_snapshot s = snap(0);
    fips_peer_snapshot ps[2] = {peer(1, 0, 1), peer(1, 0, 2)};
    CHECK(fips_stats_history_tick(&h, secs(0), &s, ps, 2) == FIPS_OK);
    CHECK(h.peer_count == 1 && h.fast_pushes == 1 && h.peers[0].rings[0].fast_len == 2);
    PQ(1, SRTT_MS, 2, FAST); CHECK(buffer[0] == 11 && buffer[1] == 12);
    return 0;
}
static int minute_boundary_joins_and_duplicate_flush(void)
{
    const uint64_t joins[] = {59, 60, 61};
    for (size_t j = 0; j < 3; ++j) {
        init();
        for (uint64_t i = 0; i < joins[j]; ++i) TICK(i, i);
        for (uint64_t i = joins[j]; i < 120; ++i) PTICK(i, i, 1);
        PQ(1, SRTT_MS, 120, SLOW);
        if (joins[j] == 59) CHECK(buffer[0] == 69);
        else CHECK(isnan(buffer[0]));
        double expected = 10.0 + (double)(joins[j] > 60 ? joins[j] + 119 : 60 + 119) / 2;
        CHECK(buffer[1] == expected);
    }
    init(); for (uint64_t i = 0; i < 59; ++i) TICK(i, i);
    fips_stats_snapshot s = snap(59);
    fips_peer_snapshot ps[2] = {peer(1, 59, 1), peer(1, 59, 3)};
    CHECK(fips_stats_history_tick(&h, secs(59), &s, ps, 2) == FIPS_OK);
    PQ(1, SRTT_MS, 60, SLOW); CHECK(buffer[0] == 12);
    return 0;
}
static int counters_subtract_before_double_conversion(void)
{
    const uint64_t starts[] = {UINT64_C(9007199254740992), UINT64_MAX - 1};
    for (size_t k = 0; k < 2; ++k) {
        init();
        for (uint64_t i = 0; i < 2; ++i) {
            fips_stats_snapshot s = snap(i); fips_peer_snapshot p = peer(1, i, i);
            s.bytes_in_total = p.bytes_in_total = starts[k] + i;
            CHECK(fips_stats_history_tick(&h, secs(i), &s, &p, 1) == FIPS_OK);
        }
        Q(BYTES_IN, 2, FAST); CHECK(buffer[0] == 0 && buffer[1] == 1);
        PQ(1, BYTES_IN, 2, FAST); CHECK(isnan(buffer[0]) && buffer[1] == 1);
    }
    return 0;
}
static int overflow_rebase_preserves_cadence_and_backfill(void)
{
    init(); for (uint64_t i = 0; i < 14; ++i) TICK(i, i);
    h.fast_pushes = UINT64_MAX - 1; /* Same modulo-60 phase as 14. */
    for (uint64_t i = 0; i < 46; ++i) {
        if (i == 0) { TICK(i, i); } else { PTICK(i, i, 1); }
        if (i == 1) {
            CHECK(h.fast_pushes == 86416 && h.peer_count == 1);
            CHECK(h.peers[0].rings[0].fast_len == 3600 && h.peers[0].rings[0].slow_len == 1440);
        }
        if (i < 45) CHECK(h.rings[0].slow_len == 0);
    }
    CHECK(h.fast_pushes == 86460 && h.rings[0].slow_len == 1);
    Q(MESH_SIZE, 60, SLOW); CHECK(buffer[0] == 55);
    PQ(1, SRTT_MS, 86400, SLOW); CHECK(all_nan(0, 1439) && buffer[1439] == 33);
    return 0;
}

/* fips_stats_history_sync: an earlier copy of a history brought current by
 * copying only what changed. Compared field by field (rings carry padding). */
#define SYNC_POOL 24
static bool ring_equal(const fips_stats_ring *a, const fips_stats_ring *b)
{
    return !memcmp(a->fast, b->fast, sizeof a->fast) && !memcmp(a->slow, b->slow, sizeof a->slow) &&
        a->fast_start == b->fast_start && a->fast_len == b->fast_len &&
        a->slow_start == b->slow_start && a->slow_len == b->slow_len &&
        !memcmp(&a->sum, &b->sum, sizeof a->sum) && !memcmp(&a->last, &b->last, sizeof a->last) &&
        a->count == b->count && a->total == b->total && a->prev_total == b->prev_total &&
        a->has_prev == b->has_prev && a->aggregation == b->aggregation;
}
static bool history_equal(const fips_stats_history *a, const fips_stats_history *b)
{
    if (a->peer_count != b->peer_count || a->fast_pushes != b->fast_pushes || a->has_data != b->has_data ||
        a->last_tick.ops != b->last_tick.ops || memcmp(&a->last_tick, &b->last_tick, sizeof a->last_tick))
        return false;
    for (int i = 0; i < FIPS_METRIC_COUNT; ++i) if (!ring_equal(&a->rings[i], &b->rings[i])) return false;
    for (size_t i = 0; i < a->peer_count; ++i) {
        const fips_peer_stats *p = &a->peers[i], *q = &b->peers[i];
        if (memcmp(p->addr, q->addr, 16) || p->born != q->born || p->seen_tick != q->seen_tick ||
            memcmp(&p->first_seen, &q->first_seen, sizeof p->first_seen) ||
            memcmp(&p->last_contact, &q->last_contact, sizeof p->last_contact)) return false;
        for (int j = 0; j < FIPS_PEER_METRIC_COUNT; ++j) if (!ring_equal(&p->rings[j], &q->rings[j])) return false;
    }
    return true;
}
typedef struct hist { fips_stats_history h; fips_peer_stats *peers; } hist;
static bool hist_new(hist *x, size_t cap)
{
    x->peers = calloc(cap, sizeof *x->peers);
    if (x->peers) fips_stats_history_init(&x->h, x->peers, cap);
    return x->peers != NULL;
}
static bool hist_clone(hist *to, const hist *from)
{
    if (!hist_new(to, from->h.peer_capacity)) return false;
    to->h = from->h; to->h.peers = to->peers;
    memcpy(to->peers, from->h.peers, from->h.peer_count * sizeof *to->peers);
    return true;
}
static uint32_t sync_rng = 20260618;
static uint32_t sync_next(void) { sync_rng = sync_rng * 1103515245u + 12345u; return sync_rng >> 8; }
static uint64_t sync_secs;
static uint64_t sync_totals[SYNC_POOL];
static int sync_tick(hist *x, bool jump)
{
    fips_peer_snapshot ps[SYNC_POOL]; size_t n = 0;
    sync_secs += jump ? 90000 : 1;
    for (unsigned id = 0; id < SYNC_POOL; ++id) {
        if (sync_next() % 4) continue;
        sync_totals[id] += sync_next() % 5000;
        if (sync_next() % 50 == 0) sync_totals[id] /= 2; /* a counter reset */
        ps[n++] = peer((unsigned char)(id + 1), sync_secs, sync_totals[id]);
        if (sync_next() % 3 == 0) ps[n - 1].has_srtt_ms = false;
    }
    fips_stats_snapshot sn = snap(sync_next() % 500);
    CHECK(fips_stats_history_tick(&x->h, secs(sync_secs), &sn, ps, n) == FIPS_OK);
    return 0;
}
static int sync_brings_an_earlier_copy_current_under_churn(void)
{
    enum { CLONES = 4, TICKS = 14000 };
    static hist live, clone[CLONES]; size_t due[CLONES] = {0};
    CHECK(hist_new(&live, SYNC_POOL));
    sync_secs = 0; memset(sync_totals, 0, sizeof sync_totals);
    for (int i = 0; i < 70; ++i) CHECK(!sync_tick(&live, false));
    for (int i = 0; i < CLONES; ++i) CHECK(hist_clone(&clone[i], &live));
    size_t syncs = 0, evicting = 0, long_gaps = 0;
    for (size_t t = 0; t < TICKS; ++t) {
        bool jump = sync_next() % 400 == 0;
        evicting += jump;
        CHECK(!sync_tick(&live, jump));
        for (int i = 0; i < CLONES; ++i) {
            if (t < due[i]) continue;
            CHECK(fips_stats_history_sync(&clone[i].h, &live.h) == FIPS_OK);
            CHECK(history_equal(&clone[i].h, &live.h));
            uint32_t r = sync_next() % 20;
            size_t gap = r < 14 ? 1 + r % 7 : r < 18 ? 58 + sync_next() % 5 : r < 19 ? 100 + sync_next() % 400 : 3590 + sync_next() % 40;
            long_gaps += gap > 3000;
            due[i] = t + gap; ++syncs;
        }
    }
    if (!(syncs > 200 && evicting > 10 && long_gaps > 8)) fprintf(stderr, "syncs=%zu evicting=%zu long_gaps=%zu\n", syncs, evicting, long_gaps);
    CHECK(syncs > 200 && evicting > 10 && long_gaps > 8);
    for (int i = 0; i < CLONES; ++i) free(clone[i].peers);
    free(live.peers);
    return 0;
}
static int sync_copies_only_what_changed(void)
{
    static hist live, copy;
    CHECK(hist_new(&live, SYNC_POOL)); sync_secs = 0; memset(sync_totals, 0, sizeof sync_totals);
    for (int i = 0; i < 200; ++i) CHECK(!sync_tick(&live, false));
    CHECK(live.h.peer_count > 3 && hist_clone(&copy, &live));
    /* An old sample in the copy that the live history still has is not rewritten. */
    double keep_node = copy.h.rings[FIPS_METRIC_MESH_SIZE].fast[0], keep_peer = copy.peers[0].rings[0].fast[0];
    copy.h.rings[FIPS_METRIC_MESH_SIZE].fast[0] = -1; copy.peers[0].rings[0].fast[0] = -2;
    for (int i = 0; i < 3; ++i) CHECK(!sync_tick(&live, false));
    CHECK(fips_stats_history_sync(&copy.h, &live.h) == FIPS_OK);
    CHECK(copy.h.rings[FIPS_METRIC_MESH_SIZE].fast[0] == -1 && copy.peers[0].rings[0].fast[0] == -2);
    copy.h.rings[FIPS_METRIC_MESH_SIZE].fast[0] = keep_node; copy.peers[0].rings[0].fast[0] = keep_peer;
    CHECK(history_equal(&copy.h, &live.h));
    /* Nothing elapsed: nothing to do and still equal. */
    CHECK(fips_stats_history_sync(&copy.h, &live.h) == FIPS_OK && history_equal(&copy.h, &live.h));
    free(copy.peers); free(live.peers);
    return 0;
}
static int sync_of_a_copy_ahead_of_its_source_copies_whole(void)
{
    static hist live, past, ahead;
    CHECK(hist_new(&live, SYNC_POOL)); sync_secs = 0; memset(sync_totals, 0, sizeof sync_totals);
    for (int i = 0; i < 150; ++i) CHECK(!sync_tick(&live, false));
    CHECK(hist_clone(&past, &live));
    for (int i = 0; i < 90; ++i) CHECK(!sync_tick(&live, false));
    CHECK(hist_clone(&ahead, &live));
    CHECK(fips_stats_history_sync(&ahead.h, &past.h) == FIPS_OK && history_equal(&ahead.h, &past.h));
    free(ahead.peers); free(past.peers); free(live.peers);
    return 0;
}
static int sync_refuses_a_destination_too_small_for_the_peers(void)
{
    static hist live, small;
    CHECK(hist_new(&live, SYNC_POOL)); sync_secs = 0; memset(sync_totals, 0, sizeof sync_totals);
    for (int i = 0; i < 40; ++i) CHECK(!sync_tick(&live, false));
    CHECK(live.h.peer_count > 2 && hist_new(&small, 2));
    fips_stats_history_init(&small.h, small.peers, 2);
    fips_stats_history before = small.h;
    CHECK(fips_stats_history_sync(&small.h, &live.h) == FIPS_ERR_RANGE);
    CHECK(!memcmp(&before, &small.h, sizeof before) && small.h.peer_count == 0);
    free(small.peers); free(live.peers);
    return 0;
}
int main(void)
{
    int (*tests[])(void) = {
        push_and_query_fast_ring, fast_ring_wraps_at_capacity, delta_for_counter_metric,
        downsample_last_aggregation, downsample_mean_aggregation, downsample_sum_aggregation,
        query_pads_front_with_nan_when_ring_is_short,
        fast_query_young_ring_returns_full_hour_with_leading_nan,
        slow_query_young_ring_returns_full_day_with_leading_nan,
        metric_parse_roundtrip, peer_metric_parse_roundtrip, granularity_parse,
        latest_and_recent, active_sessions_is_sampled_as_gauge,
        new_peer_backfills_nan_to_align_with_node_rings, absent_peer_gets_nan_sample,
        counter_decrease_emits_nan_and_rebaselines, peer_eviction_fires_after_24h_of_silence,
        a_peer_silent_for_just_under_the_eviction_window_is_kept,
        nan_mean_downsample_skips_nan_samples, all_nan_window_downsamples_to_nan,
        nan_serializes_to_json_null,
        slow_ring_wraps_at_capacity, empty_zero_window_and_failure_outputs,
        capacity_preflight_is_transactional_and_sorted,
        gaps_rebaseline_every_counter_and_node_saturates,
        peer_metadata_optional_gauges_and_backfill, peer_sum_last_nan_and_duplicate_inputs,
        minute_boundary_joins_and_duplicate_flush, counters_subtract_before_double_conversion,
        overflow_rebase_preserves_cadence_and_backfill,
        sync_brings_an_earlier_copy_current_under_churn, sync_copies_only_what_changed,
        sync_of_a_copy_ahead_of_its_source_copies_whole, sync_refuses_a_destination_too_small_for_the_peers
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) if (tests[i]()) return 1;
    puts("stats history: 22 upstream tests + 9 edge tests + 4 sync tests passed");
    return 0;
}
