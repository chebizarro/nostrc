/* Fake-fed host collector: more than 64 peers, retained churn, forwarding
 * totals, half-open session count, one-second sampling and mesh union. */
#include "node/handlers/internal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void)
{
    handlers *h = calloc(1, sizeof *h); assert(h);
    h->cfg.peer_capacity = 65; h->cfg.session_capacity = 1;
    h->tree = calloc(1, sizeof *h->tree); h->recs = calloc(65, sizeof *h->recs);
    h->session_entries = calloc(1, sizeof *h->session_entries);
    h->history = calloc(1, sizeof *h->history); h->history_peers = calloc(1, sizeof *h->history_peers);
    h->history_samples = calloc(65, sizeof *h->history_samples);
    assert(h->tree && h->recs && h->session_entries && h->history && h->history_peers && h->history_samples);
    h->session_entries[0].occupied = true; /* upstream active_sessions=sessions.len(), even half-open */
    fips_stats_history_init(h->history, h->history_peers, 1);
    fips_clock clock = fipsd_host_os_clock(); fips_instant now = clock.now(clock.ctx);
    fips_peer_identity id = {0}; fips_transport_addr addr = {0};
    for (size_t i = 0; i < 65; ++i) {
        id.node_addr.bytes[15] = (uint8_t)(i + 1);
        peer_rec *p = &h->recs[i]; p->used = true; p->addr = id.node_addr;
        p->peer = fipsd_active_peer_create(&id, (fips_transport_id){1}, &addr, (fips_link_id){i+1},
            FIPS_LINK_OUTBOUND, 0, now, fipsd_peer_timing_default()); assert(p->peer);
    }
    h->counters.fwd_received_bytes = 100; h->counters.fwd_originated_bytes = 70;
    hs_stats_tick(h, now); assert(h->history->peer_count == 65);
    assert(h->history->peer_capacity >= 65 && h->history->fast_pushes == 1);
    double v; assert(fips_stats_history_latest(h->history, FIPS_METRIC_ACTIVE_SESSIONS, &v) && v == 1.0);
    uint64_t unavailable = 99; assert(!hs_stats_mesh(h, &unavailable) && unavailable == 99);
    /* Core's node-ring contract maps absent mesh size to zero; absent peer
     * samples, tested below, use NaN. Do not reimplement either in the host. */
    assert(fips_stats_history_latest(h->history, FIPS_METRIC_MESH_SIZE, &v) && v == 0.0);
    hs_stats_tick(h, fips_deadline(now, fips_duration_from_millis(200))); assert(h->history->fast_pushes == 1);
    for (size_t i = 0; i < 65; ++i) h->recs[i].used = false;
    assert(fipsd_active_peer_destroy(h->recs[0].peer)); id.node_addr.bytes[15] = 66;
    h->recs[0].peer = fipsd_active_peer_create(&id, (fips_transport_id){1}, &addr, (fips_link_id){66},
        FIPS_LINK_OUTBOUND, 0, now, fipsd_peer_timing_default()); assert(h->recs[0].peer);
    h->recs[0].addr = id.node_addr; h->recs[0].used = true;
    h->counters.fwd_received_bytes += 7; h->counters.fwd_originated_bytes += 9;
    hs_stats_tick(h, fips_deadline(now, (fips_duration){1,0}));
    assert(h->history->peer_count == 66 && h->history->fast_pushes == 2);
    assert(fips_stats_history_latest(h->history, FIPS_METRIC_BYTES_IN, &v) && v == 7.0);
    assert(fips_stats_history_latest(h->history, FIPS_METRIC_BYTES_OUT, &v) && v == 9.0);
    assert(fips_stats_history_latest(h->history, FIPS_METRIC_PEER_COUNT, &v) && v == 1.0);
    uint8_t old_addr[16] = {0}; old_addr[15] = 1;
    double buffer[1]; fips_stats_series series;
    assert(fips_stats_history_peer_query(h->history, old_addr, FIPS_PEER_BYTES_IN, (fips_duration){1,0},
        FIPS_GRANULARITY_FAST, buffer, 1, &series) == FIPS_OK && isnan(buffer[0]));
    h->bloom.peers = calloc(2, sizeof *h->bloom.peers); assert(h->bloom.peers); h->bloom.peer_count = 2;
    h->me.bytes[0] = 10;
    for (size_t i = 0; i < 2; ++i) {
        fips_bloom_peer *p = &h->bloom.peers[i]; p->has_inbound = true;
        fips_bloom_init(&p->inbound); fips_bloom_insert(&p->inbound, &id.node_addr);
    }
    uint64_t mesh = 0; assert(hs_stats_mesh(h, &mesh) && mesh == 2); /* overlapping peers dedupe */
    for (size_t i = 0; i < 65; ++i) assert(fipsd_active_peer_destroy(h->recs[i].peer));
    free(h->bloom.peers); free(h->recs); free(h->tree); free(h->session_entries);
    free(h->history); free(h->history_peers); free(h->history_samples); free(h);
    puts("host history: 65 peers, 66 retained after churn, no 200ms oversampling, forwarding deltas, half-open count, NaN, mesh union: ok");
    return 0;
}
