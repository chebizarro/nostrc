/* Timer wheel (upstream handlers/timeout.rs + the rx_loop maintenance tick):
 * one nearest deadline over pending legs, session/path machines and the
 * maintenance tick; the dispatcher's select wakes exactly then. Machines with
 * their own next_wake are polled at it; stateless duties ride the tick. */
#include "node/handlers/internal.h"

uint64_t hs_wait_ms(handlers *h, fips_instant now)
{
    if (!h->running) return UINT64_MAX;
    uint64_t wait = hs_until_ms(h->next_tick, now);
    uint64_t w = hs_legs_wait_ms(h, now); if (w < wait) wait = w;
    w = hs_session_wait_ms(h, now); if (w < wait) wait = w;
    w = hs_path_wait_ms(h, now); if (w < wait) wait = w;
    return wait;
}
bool hs_poll(handlers *h, fips_instant now)
{
    if (!h->running) return true;
    if (!hs_legs_poll(h, now)) return false;
    hs_session_poll(h, now);
    hs_path_poll(h, now);
    if (h->next_tick.ops && now.ops->compare(now, h->next_tick) >= 0) {
        hs_tick(h, now);
        do h->next_tick = fips_deadline(h->next_tick, fips_duration_from_millis(h->cfg.tick_ms));
        while (now.ops->compare(now, h->next_tick) >= 0);
    }
    if (h->dirty) hs_publish(h, now);
    return true;
}
static void history_tick(handlers *h, fips_instant now)
{
    fips_stats_snapshot s = {.tree_depth = (uint32_t)fips_tree_coordinate_depth(fips_tree_state_my_coords(h->tree)),
        .parent_switches_total = h->counters.tree_switches, .active_sessions = h->counters.sess_established - h->counters.sess_closed};
    fips_peer_snapshot peers[64]; size_t n = 0;
    double loss_sum = 0; size_t loss_n = 0;
    for (size_t i = 0; i < h->cfg.peer_capacity && n < 64; ++i) {
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
        s.bytes_in_total += p->bytes_in_total; s.bytes_out_total += p->bytes_out_total;
        s.packets_in_total += p->packets_in_total; s.packets_out_total += p->packets_out_total;
        ++s.peer_count;
    }
    s.loss_rate = loss_n ? loss_sum / (double)loss_n : 0.0;
    if (fips_stats_history_tick(h->history, now, &s, peers, n) != FIPS_OK) {
        fips_stats_history_cleanup(h->history, now);
        (void)fips_stats_history_tick(h->history, now, &s, peers, n);
    }
}
void hs_tick(handlers *h, fips_instant now)
{
    hs_mmp_tick(h, now);
    hs_tree_tick(h, now);
    hs_bloom_refresh(h, now, false);
    hs_lookup_tick(h, now);
    hs_session_tick(h, now);
    hs_mmp_session_tick(h, now);
    hs_probe_tick(h, now);
    (void)fips_coord_cache_purge_expired(&h->cache, now);
    fips_session_setup_cleanup(&h->setup_limiter, now);
    fips_lookup_sign_cleanup(&h->sign_limiter, now);
    fips_routing_error_rate_limiter_cleanup(&h->router.error_limiter, now);
    history_tick(h, now);
    hs_mark_dirty(h);
}
