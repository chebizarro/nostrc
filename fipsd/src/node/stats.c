#include "node/handlers/internal.h"
#include <math.h>

/* Upstream compute_mesh_size: union every contributing inbound filter,
 * insert self idempotently, then estimate once. No contributors => unknown. */
bool hs_stats_mesh(handlers *h, uint64_t *out)
{
    FIPSD_INSTR_GATE(timer_on);
    FIPSD_INSTR_BEGIN(timer_on, timer_start);
    bool has = false;
    for (size_t i = 0; i < h->bloom.peer_count; ++i) {
        const fips_bloom_peer *p = &h->bloom.peers[i];
        if (!p->has_inbound) continue;
        if (!has) { h->filter_scratch = p->inbound; has = true; }
        else (void)fips_bloom_merge(&h->filter_scratch, &p->inbound, NULL);
    }
    if (!has) FIPSD_INSTR_RETURN(timer_on, timer_start, COMPUTE_MESH_SIZE, false);
    fips_bloom_insert(&h->filter_scratch, &h->me);
    double estimate;
    if (!fips_bloom_estimated_count(&h->filter_scratch, FIPS_BLOOM_ACCEPT_FPR_CAP, &estimate)) FIPSD_INSTR_RETURN(timer_on, timer_start, COMPUTE_MESH_SIZE, false);
    *out = (uint64_t)round(estimate); FIPSD_INSTR_RETURN(timer_on, timer_start, COMPUTE_MESH_SIZE, true);
}

void hs_stats_tick(handlers *h, fips_instant now)
{
    fips_stats_snapshot s = {.tree_depth = (uint32_t)fips_tree_coordinate_depth(fips_tree_state_my_coords(h->tree)),
        .parent_switches_total = h->counters.tree_switches, .active_sessions = 0};
    if (h->history->has_data && hs_elapsed_ms(now, h->history->last_tick) < 1000) return;
    fips_peer_snapshot *peers = h->history_samples; size_t n = 0;
    double loss_sum = 0; size_t loss_n = 0;
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
        peer_rec *rec = &h->recs[i];
        if (!rec->used) continue;
        const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
        fips_peer_snapshot *p = &peers[n++];
        memset(p, 0, sizeof *p);
        memcpy(p->node_addr, rec->addr.bytes, 16);
        p->last_seen = rec->last_heard.ops ? rec->last_heard : now;
        p->bytes_in_total = info->data.link_stats.bytes_recv; p->bytes_out_total = info->data.link_stats.bytes_sent;
        p->packets_in_total = info->data.link_stats.packets_recv; p->packets_out_total = info->data.link_stats.packets_sent;
        double srtt;
        if (hs_mmp_srtt(h, rec, &srtt)) { p->has_srtt_ms = true; p->srtt_ms = srtt; }
        fips_mmp_peer_state *mmp = fipsd_active_peer_mmp(rec->peer);
        if (mmp) {
            p->has_loss_rate = true; p->loss_rate = fips_mmp_metrics_loss_rate(&mmp->metrics);
            p->ecn_ce_total = mmp->receiver.ecn_ce_count;
            loss_sum += p->loss_rate; ++loss_n;
        }
        ++s.peer_count;
    }
    /* Upstream record_stats_history samples forwarding traffic, not FMP
     * overhead or a sum of live peers that falls when a link disappears. */
    s.bytes_in_total = h->counters.fwd_received_bytes;
    s.bytes_out_total = h->counters.fwd_forwarded_bytes + h->counters.fwd_originated_bytes;
    s.packets_in_total = h->counters.fwd_received_packets;
    s.packets_out_total = h->counters.fwd_forwarded_packets + h->counters.fwd_originated_packets;
    s.has_mesh_size = hs_stats_mesh(h, &s.mesh_size);
    for (size_t i = 0; i < h->cfg.session_capacity; ++i)
        if (h->session_entries[i].occupied) ++s.active_sessions;
    s.loss_rate = loss_n ? loss_sum / (double)loss_n : 0.0;
    /* Grow only on the dispatcher; control views replay the same update. */
    fipsd_stats_outcome outcome = fipsd_stats_history_update(h->history, &h->history_peers, now, &s, peers, n);
    if (outcome == FIPSD_STATS_NO_MEMORY) FIPSD_WARN("stats", "cannot grow retained peer history");
    else if (outcome == FIPSD_STATS_EXHAUSTED) FIPSD_WARN("stats", "peer history retention capacity exhausted");
    fipsd_stats_views_record(&h->stats_views, outcome == FIPSD_STATS_APPLIED, now, &s, peers, n);
}
