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
    hs_stats_tick(h, now);
    hs_mark_dirty(h);
}
