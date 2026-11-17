/* Timer wheel (upstream handlers/timeout.rs + the rx_loop maintenance tick):
 * one nearest deadline over pending legs, session/path machines and the
 * maintenance tick; the dispatcher's select wakes exactly then. Machines with
 * their own next_wake are polled at it; stateless duties ride the tick. */
#include "node/handlers/internal.h"

/* A control snapshot build is expensive (arena, JSON rows, stats rings), so
 * dirty marks that arrive in a burst are coalesced into one build at most this
 * often; the first mark after a quiet period still publishes at once, and the
 * dispatcher wakes exactly when the next build is allowed. State-change
 * notices keep their own synchronous publish (hs_notice_full). */
#define FIPSD_PUBLISH_MIN_INTERVAL_MS 50u

uint64_t hs_wait_ms(handlers *h, fips_instant now)
{
    if (!h->running) return UINT64_MAX;
    uint64_t wait = hs_until_ms(h->next_tick, now);
    if (h->dirty) { uint64_t p = hs_until_ms(h->publish_allowed, now); if (p < wait) wait = p; }
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
    if (h->dirty && hs_until_ms(h->publish_allowed, now) == 0) {
        hs_publish(h, now);
        h->publish_allowed = fips_deadline(now, fips_duration_from_millis(FIPSD_PUBLISH_MIN_INTERVAL_MS));
    }
    return true;
}
/* Upstream close_unproven_ble_links (FRC-1301 Section 6.5.4): the node holds a
 * channel while a handshake leg or a link uses its address. */
typedef struct unproven_scope { handlers *h; fips_transport_id transport; } unproven_scope;
static bool held(void *arg, const fips_transport_addr *a)
{
    unproven_scope *s = arg; handlers *h = s->h;
    if (h->peers && fipsd_peer_table_endpoint(h->peers->table, s->transport, a)) return true;
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i)
        if (h->legs[i].used && fips_transport_id_equal(h->legs[i].transport, s->transport) &&
            fips_transport_addr_equal(&h->legs[i].addr, a)) return true;
    return false;
}
static void close_unproven(handlers *h)
{
    for (size_t i = 0; h->registry && i < h->registry->count; ++i) {
        fipsd_transport *t = h->registry->items[i];
        if (!t->ops->close_unproven) continue;
        unproven_scope s = {h, t->id};
        (void)t->ops->close_unproven(t, h->cfg.timing.handshake_timeout_ms, held, &s);
    }
}
void hs_tick(handlers *h, fips_instant now)
{
    FIPSD_INSTR_GATE(instr_on);
    FIPSD_INSTR_ENTRY(instr_on, hs_now(h), h->next_tick);
    FIPSD_INSTR_BEGIN(instr_on, whole);
    FIPSD_INSTR_STEP(instr_on, CHECK_MMP_REPORTS, hs_mmp_tick(h, now));
    FIPSD_INSTR_STEP(instr_on, CHECK_TREE_STATE, hs_tree_tick(h, now));
    FIPSD_INSTR_STEP(instr_on, CHECK_BLOOM_STATE, hs_bloom_refresh(h, now, false));
    FIPSD_INSTR_STEP(instr_on, CHECK_PENDING_LOOKUPS, hs_lookup_tick(h, now));
    FIPSD_INSTR_STEP(instr_on, POLL_SESSION_REKEYS, hs_session_tick(h, now));
    FIPSD_INSTR_STEP(instr_on, CHECK_SESSION_MMP_REPORTS, hs_mmp_session_tick(h, now));
    FIPSD_INSTR_STEP(instr_on, POLL_PROBES, hs_probe_tick(h, now));
    close_unproven(h);
    (void)fips_coord_cache_purge_expired(&h->cache, now);
    fips_session_setup_cleanup(&h->setup_limiter, now);
    fips_lookup_sign_cleanup(&h->sign_limiter, now);
    fips_routing_error_rate_limiter_cleanup(&h->router.error_limiter, now);
    FIPSD_INSTR_STEP(instr_on, RECORD_STATS_HISTORY, hs_stats_tick(h, now));
    hs_mark_dirty(h);
    FIPSD_INSTR_END(instr_on, whole, WHOLE_TICK);
    FIPSD_INSTR_PEER_COUNT(instr_on, h->peers ? fipsd_peer_table_count(h->peers->table) : 0);
}
