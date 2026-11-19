/* Ports all 26 core + 4 path tests from proto/probe/tests at the upstream pin.
 * The diagnostic has no codec: no path::wire cases belong in this test. */
#include <fips/proto/probe/mod.h>
#include "fakes/host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)
#define T0 UINT64_C(1000000)
static fips_probe p, saved;
static fips_probe_snapshot s;
static fips_probe_path_facts good, facts;
static fips_tree_coordinate coords[2];
static fips_probe_next_hop_facts hop;
static fips_test_clock clock_state;
static fips_probe_actions actions;
static fips_node_addr addr(uint8_t n) { fips_node_addr a = {{0}}; a.bytes[0] = n; return a; }
static fips_probe_budgets budgets(void)
{
    const uint64_t ladder[] = {1,2,4,8};
    return fips_probe_budgets_derive(1000, ladder, 4);
}
static fips_probe_preflight preflight(void) { return (fips_probe_preflight){false,true,true,false}; }
static void start_with(fips_probe_budgets b, fips_probe_preflight pre)
{
    clock_state.millis = 0;
    fips_probe_new(&p, T0, b, pre);
}
static void start(void) { start_with(budgets(), preflight()); }
static fips_probe_observation obs(uint64_t ms)
{
    CHECK(ms >= clock_state.millis);
    CHECK(fips_test_clock_advance(&clock_state, ms - clock_state.millis) == FIPS_OK);
    fips_clock c = fips_test_clock_adapter(&clock_state);
    fips_instant now = c.now(c.ctx);
    fips_duration d = now.ops->saturating_duration_since(now, fips_test_instant(0));
    return (fips_probe_observation){.now_ms = d.secs * 1000 + d.nanos / 1000000,
        .coords_cached = true, .path = &good, .next_hop = &hop};
}
static fips_probe_observation established(uint64_t ms)
{
    fips_probe_observation o = obs(ms);
    o.session_present = o.session_established = o.session_is_ours = true;
    return o;
}
static void step(fips_probe_observation o)
{
    CHECK(fips_probe_step(&p, &o, &actions) == FIPS_OK);
    CHECK(actions.len <= FIPS_PROBE_ACTION_CAPACITY);
    fips_probe_snapshot_get(&p, &s);
}
static void cancel(uint64_t ms)
{
    fips_probe_cancel(&p, ms, &actions);
    fips_probe_snapshot_get(&p, &s);
}
static bool has(fips_probe_action a)
{
    for (size_t i = 0; i < actions.len; ++i) if (actions.items[i] == a) return true;
    return false;
}
static void one(fips_probe_action a) { CHECK(actions.len == 1 && actions.items[0] == a); }
static void reason(fips_probe_stage_record r, fips_probe_fail_kind k) { CHECK(r.has_reason && r.reason == k); }
static uint64_t probe_at_rtt(void)
{
    start(); step(obs(T0)); one(FIPS_PROBE_OpenSession);
    step(established(T0+1000)); one(FIPS_PROBE_SendWarmup);
    return T0+1000;
}
static void resolving(void)
{
    fips_probe_preflight pre = preflight(); pre.coords_cached = false;
    start_with(budgets(), pre);
    fips_probe_observation o = obs(T0); o.coords_cached = false;
    step(o); one(FIPS_PROBE_InitiateLookup);
}
static void sent(fips_probe_lookup_outcome_kind kind)
{
    fips_probe_observation o = obs(T0+1000);
    o.coords_cached = false; o.lookup_pending = true;
    o.has_lookup_outcome = true; o.lookup_outcome = kind;
    step(o);
}
static void assert_teardown_then_finish(uint64_t from, uint64_t ticks, bool complete)
{
    unsigned td = 0, fin = 0;
    for (uint64_t tick = 0; tick < ticks; ++tick) {
        fips_probe_observation o = established(from + tick*1000);
        o.session_established = complete;
        step(o);
        for (size_t i = 0; i < actions.len; ++i) {
            if (actions.items[i] == FIPS_PROBE_TeardownSession) { CHECK(fin == 0); ++td; }
            if (actions.items[i] == FIPS_PROBE_Finish) { CHECK(td == 1); ++fin; }
        }
    }
    CHECK(td == 1 && fin == 1 && s.torn_down && fips_probe_is_finished(&p));
}
static void budgets_derived_from_a_ladder_with_a_huge_entry_saturate_instead_of_overflowing(void)
{
    const uint64_t ladder[] = {UINT64_MAX,1};
    fips_probe_budgets b = fips_probe_budgets_derive(1000, ladder, 2);
    CHECK(b.discovery_ms == UINT64_MAX && fips_probe_budgets_total_ms(b) == UINT64_MAX);
}
static void a_probe_with_a_saturated_budget_starts_and_runs_instead_of_overflowing_its_deadline(void)
{
    const uint64_t ladder[] = {UINT64_MAX,1};
    start_with(fips_probe_budgets_derive(1000, ladder, 2), preflight());
    step(obs(T0)); one(FIPS_PROBE_OpenSession);
}
static void both_lookup_stages_are_skipped_when_coords_are_cached(void)
{
    start(); step(obs(T0)); CHECK(!has(FIPS_PROBE_InitiateLookup));
    CHECK(s.bloom.verdict == FIPS_PROBE_VERDICT_Skipped && s.discovery.verdict == FIPS_PROBE_VERDICT_Skipped);
    reason(s.bloom,FIPS_PROBE_Cached); reason(s.discovery,FIPS_PROBE_Cached);
    CHECK(s.has_resolve_source && s.resolve_source == FIPS_PROBE_RESOLVE_Cache);
}
static void both_lookup_stages_are_skipped_for_a_direct_peer(void)
{
    fips_probe_preflight pre = preflight(); pre.coords_cached = false; start_with(budgets(),pre);
    fips_probe_observation o = obs(T0); o.coords_cached = false; o.target_is_direct_peer = true;
    step(o); one(FIPS_PROBE_OpenSession);
    CHECK(s.bloom.verdict == FIPS_PROBE_VERDICT_Skipped);
    reason(s.bloom,FIPS_PROBE_DirectPeer); reason(s.discovery,FIPS_PROBE_DirectPeer);
}
static void discovery_times_out_at_its_own_budget_measured_from_the_request(void)
{
    resolving(); sent(FIPS_PROBE_LOOKUP_Sent); CHECK(actions.len == 0);
    CHECK(s.bloom.verdict == FIPS_PROBE_VERDICT_Ok && s.discovery.verdict == FIPS_PROBE_VERDICT_Running);
    fips_probe_observation o = obs(T0+1000+budgets().discovery_ms-1);
    o.coords_cached = false; o.lookup_pending = true; step(o);
    CHECK(actions.len == 0 && s.discovery.verdict == FIPS_PROBE_VERDICT_Running);
    o = obs(T0+1000+budgets().discovery_ms); o.coords_cached = false; o.lookup_pending = true; step(o);
    one(FIPS_PROBE_Finish); reason(s.discovery,FIPS_PROBE_BloomUnconfirmed);
    CHECK(s.discovery.verdict == FIPS_PROBE_VERDICT_Failed && s.bloom.verdict == FIPS_PROBE_VERDICT_Ok);
}
static void discovery_fails_early_when_the_pending_entry_clears(void)
{
    resolving(); sent(FIPS_PROBE_LOOKUP_Sent); CHECK(actions.len == 0);
    fips_probe_observation o = obs(T0+2000); o.coords_cached = false; step(o);
    one(FIPS_PROBE_Finish); reason(s.discovery,FIPS_PROBE_BloomUnconfirmed);
    CHECK(2000 < budgets().discovery_ms);
}
static void each_gate_decision_lands_on_the_stage_that_owns_it(void)
{
    const fips_probe_lookup_outcome_kind kinds[] = {FIPS_PROBE_LOOKUP_BloomMiss,FIPS_PROBE_LOOKUP_Suppressed,
        FIPS_PROBE_LOOKUP_ZeroFanout,FIPS_PROBE_LOOKUP_Deduplicated};
    const fips_probe_fail_kind expected[] = {FIPS_PROBE_BloomMiss,FIPS_PROBE_BackoffSuppressed,
        FIPS_PROBE_NoTreePeers,FIPS_PROBE_AlreadyPending};
    for (size_t i = 0; i < 4; ++i) {
        resolving(); sent(kinds[i]);
        if (i == 3) { fips_probe_observation o = obs(T0+2000); o.coords_cached = false; step(o); }
        reason(i == 3 ? s.discovery : s.bloom, expected[i]);
    }
}
static void absent_key_probe(fips_probe_lookup_outcome_kind gate)
{
    fips_probe_preflight pre = preflight(); pre.coords_cached = false; start_with(budgets(),pre);
    for (uint64_t tick = 0; tick < 40; ++tick) {
        fips_probe_observation o = obs(T0+tick*1000); o.coords_cached = false;
        o.has_lookup_outcome = tick > 0; o.lookup_outcome = gate;
        o.lookup_pending = gate == FIPS_PROBE_LOOKUP_Sent && tick < 16;
        step(o); if (has(FIPS_PROBE_Finish)) break;
    }
}
static void absent_key_names_the_filter_claim_instead_of_collapsing_into_no_response(void)
{
    absent_key_probe(FIPS_PROBE_LOOKUP_BloomMiss);
    CHECK(s.bloom.verdict == FIPS_PROBE_VERDICT_Failed && s.overall == FIPS_PROBE_OVERALL_Failed);
    reason(s.bloom,FIPS_PROBE_BloomMiss);
    absent_key_probe(FIPS_PROBE_LOOKUP_Sent);
    CHECK(s.bloom.verdict == FIPS_PROBE_VERDICT_Ok && s.overall == FIPS_PROBE_OVERALL_Failed);
    reason(s.discovery,FIPS_PROBE_BloomUnconfirmed);
    CHECK(s.discovery.reason != FIPS_PROBE_BloomMiss && s.discovery.reason != FIPS_PROBE_NoResponse &&
        s.discovery.reason != FIPS_PROBE_BackoffSuppressed && s.discovery.reason != FIPS_PROBE_NoTreePeers);
}
static void cancelling_mid_discovery_still_names_the_filter_claim(void)
{
    resolving(); sent(FIPS_PROBE_LOOKUP_Sent); CHECK(s.discovery.verdict == FIPS_PROBE_VERDICT_Running);
    cancel(T0+2000); reason(s.discovery,FIPS_PROBE_BloomUnconfirmed);
}
static void never_owned(uint64_t first, bool complete)
{
    for (uint64_t tick = first; tick < 40; ++tick) {
        fips_probe_observation o = obs(T0+tick*1000); o.session_present = true; o.session_established = complete;
        step(o); CHECK(!has(FIPS_PROBE_OpenSession) && !has(FIPS_PROBE_TeardownSession));
    }
    CHECK(!fips_probe_owns_session(&p));
}
static void session_skipped_when_entry_present_at_preflight(void)
{
    fips_probe_preflight pre = preflight(); pre.session_present = true; start_with(budgets(),pre);
    never_owned(0,true); reason(s.session,FIPS_PROBE_Preexisting);
}
static void session_present_but_not_established_is_still_not_owned(void)
{
    fips_probe_preflight pre = preflight(); pre.session_present = true; start_with(budgets(),pre);
    never_owned(0,false);
}
static void session_appearing_mid_probe_is_never_owned(void) { resolving(); never_owned(1,true); }
static void ownership_is_dropped_when_our_entry_is_replaced(void)
{
    start(); step(obs(T0)); one(FIPS_PROBE_OpenSession);
    step(established(T0+1000)); CHECK(fips_probe_owns_session(&p));
    never_owned(2,true); CHECK(s.has_left_intact && s.left_intact == FIPS_PROBE_LEFT_Replaced);
}
static void owned_session_torn_down_on_every_terminal_path(void)
{
    uint64_t t = probe_at_rtt(); fips_probe_observation o = established(t+1000);
    o.counters = (fips_probe_rtt_counters){1,1,0,0}; o.has_last_rtt_ms = true; o.last_rtt_ms = 18;
    step(o); assert_teardown_then_finish(t+2000,40,true);
    t = probe_at_rtt(); assert_teardown_then_finish(t+1000,40,true);
    start(); step(obs(T0)); one(FIPS_PROBE_OpenSession);
    assert_teardown_then_finish(T0+1000,39,false); reason(s.session,FIPS_PROBE_HandshakeTimeout);
    start(); step(obs(T0)); one(FIPS_PROBE_OpenSession); step(established(T0+1000));
    cancel(T0+2000); CHECK(actions.len == 2 && actions.items[0] == FIPS_PROBE_TeardownSession && actions.items[1] == FIPS_PROBE_Finish);
    cancel(T0+3000); CHECK(actions.len == 0);
}
static void a_session_the_probe_opened_is_never_reported_preexisting(void)
{
    start(); step(obs(T0)); one(FIPS_PROBE_OpenSession);
    fips_probe_observation o = established(T0+1000); o.session_established = false; step(o);
    CHECK(fips_probe_owns_session(&p) && !s.session_preexisting);
    step(established(T0+2000)); CHECK(!s.session_preexisting);
}
static void ownership_lost_before_confirmation_reads_as_replaced(void)
{
    start(); step(obs(T0)); one(FIPS_PROBE_OpenSession);
    fips_probe_observation o = established(T0+1000); o.session_is_ours = false; step(o);
    CHECK(!fips_probe_owns_session(&p) && s.has_left_intact && s.left_intact == FIPS_PROBE_LEFT_Replaced);
}
static void session_send_error_drops_ownership_and_carries_the_message(void)
{
    start(); step(obs(T0)); one(FIPS_PROBE_OpenSession); CHECK(fips_probe_owns_session(&p));
    fips_probe_observation o = obs(T0+1000); o.session_error = "no route to destination";
    o.session_error_len = strlen(o.session_error); step(o); one(FIPS_PROBE_Finish);
    CHECK(!fips_probe_owns_session(&p)); reason(s.session,FIPS_PROBE_SendError);
    CHECK(s.session.has_detail && !strcmp(s.session.detail,o.session_error));
    CHECK(!strcmp(fips_probe_overall_name(s.overall),"failed"));
}
static void direct_peer_with_no_coords_names_a_hop_and_no_missing_coords_reason(void)
{
    fips_probe_preflight pre = preflight(); pre.coords_cached = false; start_with(budgets(),pre);
    fips_probe_observation o = obs(T0); o.coords_cached = false; o.target_is_direct_peer = true; o.path = NULL;
    step(o); CHECK(s.has_next_hop && !s.has_no_hop_reason);
}
static void rtt_requires_a_new_sample_not_a_present_srtt(void)
{
    start(); step(obs(T0)); one(FIPS_PROBE_OpenSession);
    for (uint64_t tick = 1; tick < 4; ++tick) {
        fips_probe_observation o = established(T0+tick*1000);
        o.counters = (fips_probe_rtt_counters){5,5,0,0}; o.has_srtt_ms = true; o.srtt_ms = 12.0;
        step(o); if (tick == 1) one(FIPS_PROBE_SendWarmup);
        CHECK(s.rtt.verdict == FIPS_PROBE_VERDICT_Running);
    }
    fips_probe_observation o = established(T0+4000); o.counters = (fips_probe_rtt_counters){5,6,0,0};
    o.has_last_rtt_ms = true; o.last_rtt_ms = 18; step(o);
    CHECK(s.rtt.verdict == FIPS_PROBE_VERDICT_Ok && s.has_rtt_ms && s.rtt_ms == 18);
}
static void rtt_four_failure_modes_are_distinct(void)
{
    /* The last vector is upstream's precedence, kept for parity: on a sub-millisecond path the two truncated
     * clocks can yield -1 ms as well as 0 ms, and a single -1 (arith_fail) outranks any number of 0 ms samples
     * (zero), so a loopback probe may report bad_timestamp_echo. See libfips-core/UPSTREAM.md, I-5. */
    const fips_probe_rtt_counters counters[] = {{0,0,0,0},{2,0,0,0},{2,0,2,0},{2,0,0,2},{10,0,9,1}};
    const fips_probe_fail_kind expected[] = {FIPS_PROBE_NoReport,FIPS_PROBE_NoEcho,FIPS_PROBE_SubMillisecond,FIPS_PROBE_BadTimestampEcho,
        FIPS_PROBE_BadTimestampEcho};
    for (size_t i = 0; i < 5; ++i) {
        uint64_t t = probe_at_rtt();
        for (uint64_t tick = 1; tick < 12; ++tick) {
            fips_probe_observation o = established(t+tick*1000); o.counters = counters[i]; step(o);
        }
        reason(s.rtt,expected[i]);
    }
}
static void warmup_sent_at_most_twice(void)
{
    for (unsigned reports = 0; reports < 2; ++reports) {
        uint64_t t = probe_at_rtt(); unsigned warmups = 1;
        for (uint64_t tick = 1; tick < 11; ++tick) {
            fips_probe_observation o = established(t+tick*1000); o.counters.reports_seen = reports; step(o);
            if (has(FIPS_PROBE_SendWarmup)) ++warmups;
        }
        CHECK(warmups == (reports ? 1u : 2u));
    }
}
static void overall_ok_when_all_stages_pass(void)
{
    uint64_t t = probe_at_rtt(); fips_probe_observation o = established(t+1000);
    o.counters = (fips_probe_rtt_counters){1,1,0,0}; o.has_last_rtt_ms = true; o.last_rtt_ms = 18;
    step(o); assert_teardown_then_finish(t+2000,10,true);
    CHECK(!strcmp(fips_probe_overall_name(s.overall),"ok"));
}
static void overall_partial_when_session_ok_and_rtt_failed(void)
{
    uint64_t t = probe_at_rtt(); assert_teardown_then_finish(t+1000,40,true);
    CHECK(!strcmp(fips_probe_overall_name(s.overall),"partial"));
}
static void overall_failed_when_session_failed(void)
{
    start(); step(obs(T0)); one(FIPS_PROBE_OpenSession);
    assert_teardown_then_finish(T0+1000,39,false); reason(s.session,FIPS_PROBE_HandshakeTimeout);
    CHECK(!strcmp(fips_probe_overall_name(s.overall),"failed"));
}
static void total_deadline_forces_terminal(void)
{
    resolving(); fips_probe_observation o = obs(T0+fips_probe_budgets_total_ms(budgets())+1);
    o.coords_cached = false; o.lookup_pending = true; step(o); one(FIPS_PROBE_Finish);
    CHECK(fips_probe_is_finished(&p));
}
static void budgets_scale_with_tick(void)
{
    const uint64_t ladder[] = {1,2,4,8}; fips_probe_budgets b = fips_probe_budgets_derive(10000,ladder,4);
    CHECK(b.session_ms >= 30000 && b.rtt_ms >= 40000 && b.discovery_ms == 35000 && b.bloom_ms == 20000);
}
static void coordinate(size_t slot, const uint8_t *ids, size_t count)
{
    fips_node_addr addrs[16]; CHECK(count <= 16);
    for (size_t i = 0; i < count; ++i) addrs[i] = addr(ids[i]);
    CHECK(fips_tree_coordinate_from_addrs(&coords[slot],addrs,count,NULL) == FIPS_OK);
}
#define COORD(slot, ...) do { const uint8_t ids[] = {__VA_ARGS__}; coordinate(slot,ids,sizeof(ids)); } while (0)
static void path_facts_same_tree(void)
{
    COORD(0,0x4d,0x91,0x7c); COORD(1,0x4a,0xe5,0x91,0x7c);
    fips_probe_describe_path(&coords[0],&coords[1],&facts);
    CHECK(facts.same_root && facts.our_depth == 2 && facts.their_depth == 3);
    CHECK(facts.has_lca && facts.lca.bytes[0] == 0x91 && facts.has_lca_depth && facts.lca_depth == 1);
    CHECK(facts.has_tree_hops_up && facts.tree_hops_up == 1 && facts.has_tree_hops_down && facts.tree_hops_down == 2);
    CHECK(facts.has_tree_distance && facts.tree_distance == 3);
}
static void path_facts_different_roots_is_disjoint_not_depth_zero(void)
{
    COORD(0,0x11,0x01); COORD(1,0x22,0x02); fips_probe_describe_path(&coords[0],&coords[1],&facts);
    CHECK(!facts.same_root && !facts.has_lca && !facts.has_lca_depth && !facts.has_tree_hops_up &&
        !facts.has_tree_hops_down && !facts.has_tree_distance);
}
static void path_facts_ancestor_pair(void)
{
    COORD(0,0x4d,0x91,0x7c); COORD(1,0x91,0x7c); fips_probe_describe_path(&coords[0],&coords[1],&facts);
    CHECK(facts.has_lca && facts.lca.bytes[0] == 0x91 && facts.has_lca_depth && facts.lca_depth == 1);
    CHECK(facts.has_tree_hops_up && facts.tree_hops_up == 1 && facts.has_tree_hops_down && facts.tree_hops_down == 0);
    CHECK(facts.has_tree_distance && facts.tree_distance == 1);
}
static void depth_matches_entry_count_minus_one(void)
{
    path_facts_same_tree(); CHECK(facts.our_depth == facts.our_coords_len-1 && facts.their_depth == facts.their_coords_len-1);
}

/* C storage, total-deadline, snapshot-latching and boundary contracts. */
static void c_boundaries(void)
{
    fips_probe_budgets b = fips_probe_budgets_derive(UINT64_MAX,NULL,0);
    CHECK(b.bloom_ms == UINT64_MAX && b.session_ms == UINT64_MAX && b.rtt_ms == UINT64_MAX);
    start(); step(obs(T0)); saved = p;
    char text[FIPS_PROBE_DETAIL_CAPACITY]; memset(text,'x',sizeof(text));
    fips_probe_observation o = obs(T0+1); o.session_error = text; o.session_error_len = sizeof(text);
    fips_probe_actions before = actions;
    CHECK(fips_probe_step(&p,&o,&actions) == FIPS_ERR_RANGE);
    CHECK(!memcmp(&saved,&p,sizeof(p)) && !memcmp(&before,&actions,sizeof(actions)));
    o.session_error_len = sizeof(text)-1; text[10] = '\0';
    CHECK(fips_probe_step(&p,&o,&actions) == FIPS_ERR_RANGE && !memcmp(&saved,&p,sizeof(p)));
    text[10] = 'x'; step(o); CHECK(s.session.has_detail && strlen(s.session.detail) == sizeof(text)-1);
    memset(text,'y',sizeof(text)); CHECK(s.session.detail[0] == 'x');
    CHECK(fips_probe_elapsed_ms(&p,T0+99999) == 1); cancel(T0+2); CHECK(actions.len == 0);
    step(obs(T0+2)); CHECK(actions.len == 0);
    start(); CHECK(fips_probe_elapsed_ms(&p,T0-1) == 0 && fips_probe_elapsed_ms(&p,T0+5) == 5);
    cancel(T0+7); CHECK(s.overall == FIPS_PROBE_OVERALL_Cancelled && fips_probe_elapsed_ms(&p,T0+20) == 7);
    fips_probe_rtt_counters d = fips_probe_rtt_counters_delta((fips_probe_rtt_counters){1,2,3,4},
        (fips_probe_rtt_counters){2,2,1,5});
    CHECK(d.reports_seen == 0 && d.samples == 0 && d.zero == 2 && d.arith_fail == 0);
    fips_probe_preflight pre = preflight(); pre.target_claimed = true; start_with(budgets(),pre);
    step(obs(T0)); CHECK(!has(FIPS_PROBE_OpenSession)); step(obs(T0+budgets().session_ms));
    one(FIPS_PROBE_Finish); reason(s.session,FIPS_PROBE_Preexisting);
    resolving(); step(obs(T0+1999)); CHECK(actions.len == 0); step(obs(T0+2000));
    one(FIPS_PROBE_Finish); reason(s.bloom,FIPS_PROBE_NoResponse);
    /* Path failure never halts session/RTT. */
    start(); o = obs(T0); o.path = NULL; o.next_hop = NULL; step(o);
    one(FIPS_PROBE_OpenSession); reason(s.path,FIPS_PROBE_NoNextHop);
    CHECK(s.has_no_hop_reason && s.no_hop_reason == FIPS_PROBE_HOP_NoCoords);
    o = established(T0+1000); o.has_path_mtu = true; o.path_mtu = 1280; step(o);
    o = established(T0+2000); o.path = NULL; o.next_hop = NULL; o.counters.samples = 1; step(o);
    CHECK(s.overall == FIPS_PROBE_OVERALL_Ok && s.has_path_facts && s.has_next_hop && s.path_mtu == 1280);
    CHECK(actions.len == 0); o = established(T0+2999); step(o); CHECK(actions.len == 0);
    o = established(T0+3000); step(o); CHECK(actions.len == 2);
    /* Lookup success, latched fanout, per-stage elapsed time. */
    resolving(); o = obs(T0+1000); o.has_lookup_outcome = true; o.lookup_outcome = FIPS_PROBE_LOOKUP_Sent;
    o.has_lookup_fanout = true; o.lookup_fanout = 3; step(o); one(FIPS_PROBE_OpenSession);
    CHECK(s.resolve_source == FIPS_PROBE_RESOLVE_Lookup && s.bloom.elapsed_ms == 1000 && s.discovery.elapsed_ms == 0);
    step(established(T0+2000)); CHECK(s.has_lookup_fanout && s.lookup_fanout == 3);
    /* Zero-tick grace is still one ms; saturate rather than wrap at MAX. */
    b = (fips_probe_budgets){0,0,0,0,0}; start_with(b,preflight()); step(obs(T0)); one(FIPS_PROBE_Finish);
    fips_probe_new(&p,UINT64_MAX-1,(fips_probe_budgets){1,1,1,1,UINT64_MAX},preflight());
    o = (fips_probe_observation){.now_ms=UINT64_MAX-1,.coords_cached=true}; step(o); one(FIPS_PROBE_OpenSession);
    o.now_ms = UINT64_MAX; o.session_is_ours = true; step(o);
    CHECK(actions.len == 2 && actions.items[0] == FIPS_PROBE_TeardownSession && actions.items[1] == FIPS_PROBE_Finish);
}
static void snapshot_copy_and_max_path(void)
{
    static fips_node_addr addrs[FIPS_COORD_MAX_ENTRIES];
    for (size_t i = 0; i < FIPS_COORD_MAX_ENTRIES; ++i) {
        addrs[i] = addr((uint8_t)i);
        addrs[i].bytes[1] = (uint8_t)(i >> 8);
    }
    CHECK(fips_tree_coordinate_from_addrs(&coords[0],addrs,FIPS_COORD_MAX_ENTRIES,NULL) == FIPS_OK);
    CHECK(fips_tree_coordinate_from_addrs(&coords[1],addrs,FIPS_COORD_MAX_ENTRIES,NULL) == FIPS_OK);
    fips_probe_describe_path(&coords[0],&coords[1],&facts);
    CHECK(facts.our_coords_len == FIPS_COORD_MAX_ENTRIES && facts.their_coords_len == FIPS_COORD_MAX_ENTRIES);
    CHECK(facts.our_depth == FIPS_COORD_MAX_DEPTH && facts.lca_depth == FIPS_COORD_MAX_DEPTH);
    CHECK(facts.has_tree_distance && facts.tree_distance == 0);
    CHECK(!memcmp(facts.our_coords[FIPS_COORD_MAX_ENTRIES-1].bytes,addrs[FIPS_COORD_MAX_ENTRIES-1].bytes,16));
    start(); fips_probe_observation o = obs(T0); o.path = &facts; step(o);
    memset(&facts,0,sizeof(facts));
    CHECK(s.path_facts.our_coords_len == FIPS_COORD_MAX_ENTRIES);
    CHECK(s.path_facts.our_depth == FIPS_COORD_MAX_DEPTH);
    /* Mutating either the input or a projected report cannot mutate the machine. */
    s.path_facts.our_coords_len = 0;
    fips_probe_snapshot_get(&p,&s);
    CHECK(s.path_facts.our_coords_len == FIPS_COORD_MAX_ENTRIES);
}

static void names(void)
{
    const char *verdicts[] = {"pending","running","ok","skipped","failed"};
    const char *failures[] = {"cached","direct_peer","bloom_miss","backoff_suppressed","already_pending","no_tree_peers",
        "no_response","bloom_unconfirmed","disjoint_trees","no_next_hop","preexisting","send_error","handshake_timeout",
        "no_report","no_echo","sub_millisecond","bad_timestamp_echo","not_reached"};
    const char *sources[] = {"cache","lookup","direct_peer"};
    const char *overall[] = {"running","ok","partial","failed","cancelled"};
    const char *hops[] = {"local","no_coords","disjoint_trees","no_closer_peer","hop_not_send_ready"};
    const char *left[] = {"preexisting","adopted_by_traffic","replaced"};
    for (size_t i=0;i<5;++i) CHECK(!strcmp(fips_probe_stage_verdict_name((fips_probe_stage_verdict)i),verdicts[i]));
    for (size_t i=0;i<18;++i) CHECK(!strcmp(fips_probe_fail_kind_name((fips_probe_fail_kind)i),failures[i]));
    for (size_t i=0;i<3;++i) CHECK(!strcmp(fips_probe_resolve_source_name((fips_probe_resolve_source)i),sources[i]));
    for (size_t i=0;i<5;++i) CHECK(!strcmp(fips_probe_overall_name((fips_probe_overall)i),overall[i]));
    for (size_t i=0;i<5;++i) CHECK(!strcmp(fips_probe_no_hop_reason_name((fips_probe_no_hop_reason)i),hops[i]));
    for (size_t i=0;i<3;++i) CHECK(!strcmp(fips_probe_left_intact_name((fips_probe_left_intact)i),left[i]));
}

int main(void)
{
    COORD(0,2,1); COORD(1,3,1);
    fips_probe_describe_path(&coords[0],&coords[1],&good);
    hop = (fips_probe_next_hop_facts){addr(3),FIPS_ROUTE_DirectPeer,true,false};
    budgets_derived_from_a_ladder_with_a_huge_entry_saturate_instead_of_overflowing();
    a_probe_with_a_saturated_budget_starts_and_runs_instead_of_overflowing_its_deadline();
    both_lookup_stages_are_skipped_when_coords_are_cached();
    both_lookup_stages_are_skipped_for_a_direct_peer();
    discovery_times_out_at_its_own_budget_measured_from_the_request();
    discovery_fails_early_when_the_pending_entry_clears();
    each_gate_decision_lands_on_the_stage_that_owns_it();
    absent_key_names_the_filter_claim_instead_of_collapsing_into_no_response();
    cancelling_mid_discovery_still_names_the_filter_claim();
    session_skipped_when_entry_present_at_preflight();
    session_present_but_not_established_is_still_not_owned();
    session_appearing_mid_probe_is_never_owned();
    ownership_is_dropped_when_our_entry_is_replaced();
    owned_session_torn_down_on_every_terminal_path();
    a_session_the_probe_opened_is_never_reported_preexisting();
    ownership_lost_before_confirmation_reads_as_replaced();
    session_send_error_drops_ownership_and_carries_the_message();
    direct_peer_with_no_coords_names_a_hop_and_no_missing_coords_reason();
    rtt_requires_a_new_sample_not_a_present_srtt();
    rtt_four_failure_modes_are_distinct();
    warmup_sent_at_most_twice();
    overall_ok_when_all_stages_pass();
    overall_partial_when_session_ok_and_rtt_failed();
    overall_failed_when_session_failed();
    total_deadline_forces_terminal();
    budgets_scale_with_tick();
    path_facts_same_tree();
    path_facts_different_roots_is_disjoint_not_depth_zero();
    path_facts_ancestor_pair();
    depth_matches_entry_count_minus_one();
    c_boundaries(); snapshot_copy_and_max_path(); names();
    puts("probe: 30 upstream tests + C boundary/name tests passed");
    return 0;
}
