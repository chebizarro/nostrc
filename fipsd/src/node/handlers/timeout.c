/* Timer wheel (upstream handlers/timeout.rs + the rx_loop maintenance tick):
 * one nearest deadline over pending legs, session/path machines and the
 * maintenance tick; the dispatcher's select wakes exactly then. Machines with
 * their own next_wake are polled at it; stateless duties ride the tick. The
 * per-subsystem nearest deadlines are cached (hs_timers), not rescanned per
 * wake: see FIPSD_TIMER_VERIFY_MS. */
#include "node/handlers/internal.h"
#include <stdlib.h>

/* A control snapshot build is expensive (arena, JSON rows, stats rings), so
 * dirty marks that arrive in a burst are coalesced into one build at most this
 * often; the first mark after a quiet period still publishes at once, and the
 * dispatcher wakes exactly when the next build is allowed. State-change
 * notices keep their own synchronous publish (hs_notice_full). */
#define FIPSD_PUBLISH_MIN_INTERVAL_MS 50u

/* Deadline cache (nostrc-w3tfz). The dispatcher asks for the nearest deadline
 * on every wake and polls after every batch; rescanning every leg, peer and
 * session slot each time was the moderate-load dispatcher's largest cost. The
 * cache (hs_timers in internal.h) is never later than the true earliest
 * deadline: hot paths fold the entry they changed (it only moves earlier, so a
 * deadline that moved later costs one early wake and a rescan), everything
 * structural marks it stale, and every poll pass that fires and every tick
 * rescans exactly. A fold a future change forgets is caught two ways: Debug
 * builds audit the cache against an exact scan on every wait and abort, and in
 * Release the cache is re-derived exactly at the first wait at least this long
 * after an event, and on every tick, so a missed fold makes a timer late by at
 * most one tick, never lost. Rescans ride existing wakes: none is scheduled
 * for verification alone (that cost an idle pair ~2.5 dispatcher wakes/s). */
#define FIPSD_TIMER_VERIFY_MS 4u

static void timers_rescan(handlers *h, fips_instant now)
{
    h->timers.due[HS_TIMER_LEGS] = hs_legs_next(h);
    h->timers.due[HS_TIMER_SESSIONS] = hs_session_next(h);
    h->timers.due[HS_TIMER_PATHS] = hs_path_next(h);
    h->timers.verified = now;
    h->timers.stale = h->timers.touched = false;
}
#ifndef NDEBUG
static void timers_audit(handlers *h, fips_instant now)
{
    static const char *const names[HS_TIMER_COUNT] = {"legs", "sessions", "paths"};
    fips_instant exact[HS_TIMER_COUNT] = {hs_legs_next(h), hs_session_next(h), hs_path_next(h)};
    for (size_t k = 0; k < HS_TIMER_COUNT; ++k) {
        uint64_t cached = hs_until_ms(h->timers.due[k], now), truth = hs_until_ms(exact[k], now);
        if (cached <= truth) continue;
        FIPSD_ERROR(HMOD, "timer cache missed an invalidation: %s due in %llu ms, cached %llu ms",
            names[k], (unsigned long long)truth, (unsigned long long)cached);
        abort();
    }
}
#endif
void hs_timers_fold_session(handlers *h, const fips_session_entry *entry)
{
    if (entry->occupied && entry->machine.live)
        hs_timers_fold(h, HS_TIMER_SESSIONS, fips_rekey_next_wake(&entry->machine));
}
void hs_timers_fold_path(handlers *h, const peer_rec *rec)
{
    if (rec->used && rec->path_live) hs_timers_fold(h, HS_TIMER_PATHS, fips_path_bundle_next_wake(rec->path));
}
bool hs_timers_due(handlers *h, hs_timer_kind k, fips_instant now)
{
    if (h->timers.stale) timers_rescan(h, now);
    fips_instant at = h->timers.due[k];
    return at.ops && now.ops->compare(now, at) >= 0;
}
uint64_t hs_wait_ms(handlers *h, fips_instant now)
{
    if (!h->running) return UINT64_MAX;
    uint64_t since = hs_elapsed_ms(now, h->timers.verified);
    if (h->timers.stale || (h->timers.touched && since >= FIPSD_TIMER_VERIFY_MS)) timers_rescan(h, now);
#ifndef NDEBUG
    else timers_audit(h, now);
#endif
    uint64_t wait = hs_until_ms(h->next_tick, now);
    if (h->dirty) wait = hs_min_wait(wait, h->publish_allowed, now);
    for (size_t k = 0; k < HS_TIMER_COUNT; ++k) wait = hs_min_wait(wait, h->timers.due[k], now);
    return wait;
}
bool hs_poll(handlers *h, fips_instant now)
{
    if (!h->running) return true;
    /* Each pass that may fire runs on a fresh exact view (a stale cache
     * rescans in hs_timers_due) and leaves the cache stale behind it, so a
     * pass sees what the one before it armed, as the unconditional scans did. */
    if (hs_timers_due(h, HS_TIMER_LEGS, now)) {
        hs_timers_invalidate(h);
        if (!hs_legs_poll(h, now)) return false;
    }
    if (hs_timers_due(h, HS_TIMER_SESSIONS, now)) { hs_timers_invalidate(h); hs_session_poll(h, now); }
    if (hs_timers_due(h, HS_TIMER_PATHS, now)) { hs_timers_invalidate(h); hs_path_poll(h, now); }
    if (h->next_tick.ops && now.ops->compare(now, h->next_tick) >= 0) {
        hs_timers_invalidate(h); /* the tick is also the cache's periodic exact rescan */
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
