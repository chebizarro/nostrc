#include <fips/proto/probe/core.h>
#include <string.h>

static uint64_t add(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }
static uint64_t mul(uint64_t a, uint64_t b) { return b && a > UINT64_MAX / b ? UINT64_MAX : a * b; }
static uint64_t sub(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }
static uint64_t max(uint64_t a, uint64_t b) { return a > b ? a : b; }

fips_probe_budgets fips_probe_budgets_derive(uint64_t tick, const uint64_t *secs, size_t count)
{
    uint64_t ladder = 0;
    for (size_t i = 0; i < count; ++i) ladder = add(ladder, secs[i]);
    return (fips_probe_budgets){
        mul(FIPS_PROBE_BLOOM_MIN_TICKS, tick),
        add(mul(ladder, 1000), mul(FIPS_PROBE_RESOLVE_SLACK_TICKS, tick)),
        max(FIPS_PROBE_SESSION_FLOOR_MS, mul(FIPS_PROBE_SESSION_MIN_TICKS, tick)),
        max(FIPS_PROBE_RTT_FLOOR_MS, mul(FIPS_PROBE_RTT_MIN_TICKS, tick)), tick};
}
uint64_t fips_probe_budgets_total_ms(fips_probe_budgets b)
{
    return add(add(add(b.bloom_ms, b.discovery_ms), b.session_ms), b.rtt_ms);
}
void fips_probe_new(fips_probe *p, uint64_t now, fips_probe_budgets b, fips_probe_preflight pre)
{
    memset(p, 0, sizeof(*p));
    p->started_ms = p->stage_started_ms = now;
    p->deadline_ms = add(now, fips_probe_budgets_total_ms(b));
    p->budgets = b;
    p->preflight = pre;
    p->stage = FIPS_PROBE_Bloom;
    p->report.tick_ms = b.tick_ms;
    p->report.overall = FIPS_PROBE_OVERALL_Running;
    p->report.bloom = p->report.discovery = p->report.path =
        p->report.session = p->report.rtt = fips_probe_stage_record_pending();
    p->report.session_preexisting = pre.session_present;
    p->report.coords_were_cached = pre.coords_cached;
    p->report.identity_was_cached = pre.identity_cached;
}
bool fips_probe_owns_session(const fips_probe *p) { return p->owns_session; }
bool fips_probe_is_finished(const fips_probe *p) { return p->finished; }
void fips_probe_snapshot_get(const fips_probe *p, fips_probe_snapshot *out) { *out = p->report; }
uint64_t fips_probe_elapsed_ms(const fips_probe *p, uint64_t now)
{
    return sub(p->has_ended_ms ? p->ended_ms : now, p->started_ms);
}

static fips_probe_stage_record record(fips_probe_stage_verdict verdict, fips_probe_fail_kind reason)
{
    return (fips_probe_stage_record){.verdict = verdict,
        .has_reason = verdict != FIPS_PROBE_VERDICT_Ok, .reason = reason};
}
static fips_probe_stage_record *current_record(fips_probe *p)
{
    switch (p->stage) {
    case FIPS_PROBE_Bloom: return &p->report.bloom;
    case FIPS_PROBE_Discovery: return &p->report.discovery;
    case FIPS_PROBE_Path: return &p->report.path;
    case FIPS_PROBE_Session: return &p->report.session;
    case FIPS_PROBE_Rtt: case FIPS_PROBE_Terminal: return &p->report.rtt;
    }
    return &p->report.rtt;
}
static void finish_stage(fips_probe *p, fips_probe_stage_record r, const fips_probe_observation *o)
{
    r.has_elapsed_ms = true;
    r.elapsed_ms = sub(o->now_ms, p->stage_started_ms);
    *current_record(p) = r;
}
static void advance(fips_probe *p, const fips_probe_observation *o)
{
    if (p->stage < FIPS_PROBE_Terminal) p->stage = (fips_probe_stage)(p->stage + 1);
    p->stage_started_ms = o->now_ms;
}
static void skip_rest(fips_probe *p)
{
    fips_probe_stage_record *slots[] = {&p->report.bloom, &p->report.discovery,
        &p->report.path, &p->report.session, &p->report.rtt};
    for (size_t i = (size_t)p->stage + 1; i < 5; ++i)
        *slots[i] = record(FIPS_PROBE_VERDICT_Skipped, FIPS_PROBE_NotReached);
}
static fips_probe_overall compute_overall(const fips_probe *p)
{
    if (p->report.overall == FIPS_PROBE_OVERALL_Cancelled) return FIPS_PROBE_OVERALL_Cancelled;
    if (fips_probe_stage_record_is_failed(&p->report.bloom) ||
        fips_probe_stage_record_is_failed(&p->report.discovery) ||
        fips_probe_stage_record_is_failed(&p->report.session)) return FIPS_PROBE_OVERALL_Failed;
    return p->report.rtt.verdict == FIPS_PROBE_VERDICT_Ok ? FIPS_PROBE_OVERALL_Ok : FIPS_PROBE_OVERALL_Partial;
}
static void enter_terminal(fips_probe *p, const fips_probe_observation *o)
{
    p->stage = FIPS_PROBE_Terminal;
    p->has_teardown_at_ms = true;
    p->teardown_at_ms = add(o->now_ms, mul(FIPS_PROBE_TEARDOWN_GRACE_TICKS, max(p->budgets.tick_ms, 1)));
    p->report.overall = compute_overall(p);
}
static void fail_stage(fips_probe *p, fips_probe_fail_kind kind, const fips_probe_observation *o)
{
    finish_stage(p, record(FIPS_PROBE_VERDICT_Failed, kind), o);
    skip_rest(p);
    enter_terminal(p, o);
}
static fips_probe_fail_kind discovery_fail_kind(const fips_probe *p)
{
    if (p->has_lookup_outcome) {
        if (p->lookup_outcome == FIPS_PROBE_LOOKUP_Deduplicated) return FIPS_PROBE_AlreadyPending;
        if (p->lookup_outcome == FIPS_PROBE_LOOKUP_Sent) return FIPS_PROBE_BloomUnconfirmed;
    }
    return FIPS_PROBE_NoResponse;
}
static fips_probe_fail_kind classify_rtt_failure(fips_probe_rtt_counters d)
{
    if (d.arith_fail) return FIPS_PROBE_BadTimestampEcho;
    if (d.zero) return FIPS_PROBE_SubMillisecond;
    if (d.reports_seen) return FIPS_PROBE_NoEcho;
    return FIPS_PROBE_NoReport;
}
static void expire_running_stage(fips_probe *p)
{
    fips_probe_fail_kind kind;
    switch (p->stage) {
    case FIPS_PROBE_Bloom: kind = FIPS_PROBE_NoResponse; break;
    case FIPS_PROBE_Discovery: kind = discovery_fail_kind(p); break;
    case FIPS_PROBE_Path: kind = FIPS_PROBE_NoNextHop; break;
    case FIPS_PROBE_Session: kind = p->owns_session ? FIPS_PROBE_HandshakeTimeout : FIPS_PROBE_Preexisting; break;
    case FIPS_PROBE_Rtt: kind = classify_rtt_failure(p->report.counters); break;
    case FIPS_PROBE_Terminal: return;
    default: return;
    }
    fips_probe_stage_record r = record(p->stage == FIPS_PROBE_Session && !p->owns_session ?
        FIPS_PROBE_VERDICT_Skipped : FIPS_PROBE_VERDICT_Failed, kind);
    if (current_record(p)->verdict != FIPS_PROBE_VERDICT_Ok) *current_record(p) = r;
    skip_rest(p);
}
static void push(fips_probe_actions *a, fips_probe_action action) { a->items[a->len++] = action; }
static void drive_terminal(fips_probe *p, const fips_probe_observation *o, fips_probe_actions *a)
{
    if (p->owns_session && !p->teardown_emitted) {
        if (p->has_teardown_at_ms && o->now_ms < p->teardown_at_ms) return;
        p->teardown_emitted = p->report.torn_down = true;
        push(a, FIPS_PROBE_TeardownSession);
    }
    p->finished = p->has_ended_ms = true;
    p->ended_ms = o->now_ms;
    p->report.overall = compute_overall(p);
    push(a, FIPS_PROBE_Finish);
}
static void absorb(fips_probe *p, const fips_probe_observation *o)
{
    if (o->has_lookup_outcome) { p->has_lookup_outcome = true; p->lookup_outcome = o->lookup_outcome; }
    if (o->has_lookup_fanout) { p->report.has_lookup_fanout = true; p->report.lookup_fanout = o->lookup_fanout; }
    if (o->lookup_pending) p->lookup_was_pending = true;
    if (o->path) { p->report.has_path_facts = true; p->report.path_facts = *o->path; }
    if (o->next_hop) { p->report.has_next_hop = true; p->report.next_hop = *o->next_hop; }
    if (o->has_no_hop_reason) { p->report.has_no_hop_reason = true; p->report.no_hop_reason = o->no_hop_reason; }
    if (o->has_path_mtu) { p->report.has_path_mtu = true; p->report.path_mtu = o->path_mtu; }
    if (o->has_srtt_ms) { p->report.has_srtt_ms = true; p->report.srtt_ms = o->srtt_ms; }
    if (o->session_established) p->report.session_established = true;
}
static void step_bloom(fips_probe *p, const fips_probe_observation *o, fips_probe_actions *a)
{
    if (!p->report.lookup_issued) {
        if (o->coords_cached || o->target_is_direct_peer) {
            finish_stage(p, record(FIPS_PROBE_VERDICT_Skipped,
                o->coords_cached ? FIPS_PROBE_Cached : FIPS_PROBE_DirectPeer), o);
            p->report.has_resolve_source = true;
            p->report.resolve_source = o->coords_cached ? FIPS_PROBE_RESOLVE_Cache : FIPS_PROBE_RESOLVE_DirectPeer;
            advance(p, o);
            return;
        }
        p->report.bloom.verdict = FIPS_PROBE_VERDICT_Running;
        p->report.lookup_issued = true;
        push(a, FIPS_PROBE_InitiateLookup);
        return;
    }
    if (p->has_lookup_outcome) {
        switch (p->lookup_outcome) {
        case FIPS_PROBE_LOOKUP_BloomMiss: fail_stage(p, FIPS_PROBE_BloomMiss, o); break;
        case FIPS_PROBE_LOOKUP_Suppressed: fail_stage(p, FIPS_PROBE_BackoffSuppressed, o); break;
        case FIPS_PROBE_LOOKUP_ZeroFanout: fail_stage(p, FIPS_PROBE_NoTreePeers, o); break;
        case FIPS_PROBE_LOOKUP_Sent: case FIPS_PROBE_LOOKUP_Deduplicated:
            finish_stage(p, record(FIPS_PROBE_VERDICT_Ok, FIPS_PROBE_Cached), o);
            advance(p, o); break;
        }
    } else if (sub(o->now_ms, p->stage_started_ms) >= p->budgets.bloom_ms) fail_stage(p, FIPS_PROBE_NoResponse, o);
}
static void step_discovery(fips_probe *p, const fips_probe_observation *o)
{
    if (!p->report.lookup_issued) {
        finish_stage(p, record(FIPS_PROBE_VERDICT_Skipped,
            p->report.has_resolve_source && p->report.resolve_source == FIPS_PROBE_RESOLVE_DirectPeer ?
            FIPS_PROBE_DirectPeer : FIPS_PROBE_Cached), o);
        advance(p, o);
        return;
    }
    if (o->coords_cached) {
        finish_stage(p, record(FIPS_PROBE_VERDICT_Ok, FIPS_PROBE_Cached), o);
        p->report.has_resolve_source = true;
        p->report.resolve_source = FIPS_PROBE_RESOLVE_Lookup;
        advance(p, o);
        return;
    }
    p->report.discovery.verdict = FIPS_PROBE_VERDICT_Running;
    if ((p->lookup_was_pending && !o->lookup_pending) ||
        sub(o->now_ms, p->stage_started_ms) >= p->budgets.discovery_ms)
        fail_stage(p, discovery_fail_kind(p), o);
}
static void step_path(fips_probe *p, const fips_probe_observation *o)
{
    fips_probe_stage_record r;
    if (!p->report.has_path_facts) {
        if (!p->report.has_next_hop) {
            p->report.has_no_hop_reason = true;
            p->report.no_hop_reason = FIPS_PROBE_HOP_NoCoords;
        }
        r = o->target_is_direct_peer ? record(FIPS_PROBE_VERDICT_Skipped, FIPS_PROBE_DirectPeer) :
            record(FIPS_PROBE_VERDICT_Failed, FIPS_PROBE_NoNextHop);
    } else if (!p->report.path_facts.same_root) r = record(FIPS_PROBE_VERDICT_Failed, FIPS_PROBE_DisjointTrees);
    else if (!p->report.has_next_hop) r = record(FIPS_PROBE_VERDICT_Failed, FIPS_PROBE_NoNextHop);
    else r = record(FIPS_PROBE_VERDICT_Ok, FIPS_PROBE_Cached);
    finish_stage(p, r, o);
    advance(p, o);
}
static void left_preexisting(fips_probe *p)
{
    if (!p->report.has_left_intact) {
        p->report.has_left_intact = true;
        p->report.left_intact = FIPS_PROBE_LEFT_Preexisting;
    }
}
static void step_session(fips_probe *p, const fips_probe_observation *o, fips_probe_actions *a)
{
    if (o->session_established && p->owns_session) {
        p->report.session_established = true;
        finish_stage(p, record(FIPS_PROBE_VERDICT_Ok, FIPS_PROBE_Cached), o);
        advance(p, o);
        return;
    }
    if (o->session_error) {
        p->owns_session = false;
        fips_probe_stage_record r = record(FIPS_PROBE_VERDICT_Failed, FIPS_PROBE_SendError);
        r.has_detail = true;
        memcpy(r.detail, o->session_error, o->session_error_len);
        r.detail[o->session_error_len] = '\0';
        finish_stage(p, r, o);
        skip_rest(p);
        enter_terminal(p, o);
        return;
    }
    if (o->session_present) {
        if (!p->open_requested) p->report.session_preexisting = true;
        if (!p->owns_session) {
            if (o->session_established) {
                p->report.session_established = true;
                finish_stage(p, record(FIPS_PROBE_VERDICT_Skipped, FIPS_PROBE_Preexisting), o);
                left_preexisting(p);
                advance(p, o);
                return;
            }
            if (sub(o->now_ms, p->stage_started_ms) >= p->budgets.session_ms) {
                finish_stage(p, record(FIPS_PROBE_VERDICT_Skipped, FIPS_PROBE_Preexisting), o);
                left_preexisting(p);
                skip_rest(p);
                enter_terminal(p, o);
            }
            return;
        }
    } else if (!p->open_requested && !p->preflight.target_claimed) {
        p->report.session.verdict = FIPS_PROBE_VERDICT_Running;
        p->open_requested = p->owns_session = true;
        push(a, FIPS_PROBE_OpenSession);
        return;
    }
    if (sub(o->now_ms, p->stage_started_ms) >= p->budgets.session_ms) {
        finish_stage(p, p->owns_session ? record(FIPS_PROBE_VERDICT_Failed, FIPS_PROBE_HandshakeTimeout) :
            record(FIPS_PROBE_VERDICT_Skipped, FIPS_PROBE_Preexisting), o);
        skip_rest(p);
        enter_terminal(p, o);
    }
}
static void step_rtt(fips_probe *p, const fips_probe_observation *o, fips_probe_actions *a)
{
    if (p->report.warmups_sent == 0) {
        p->report.rtt.verdict = FIPS_PROBE_VERDICT_Running;
        p->baseline = o->counters;
        p->report.warmups_sent = 1;
        p->has_last_warmup_ms = true;
        p->last_warmup_ms = o->now_ms;
        push(a, FIPS_PROBE_SendWarmup);
        return;
    }
    fips_probe_rtt_counters d = fips_probe_rtt_counters_delta(o->counters, p->baseline);
    p->report.counters = d;
    if (d.samples > 0) {
        p->report.has_rtt_ms = o->has_last_rtt_ms;
        p->report.rtt_ms = o->last_rtt_ms;
        finish_stage(p, record(FIPS_PROBE_VERDICT_Ok, FIPS_PROBE_Cached), o);
        enter_terminal(p, o);
        return;
    }
    if (p->report.warmups_sent == 1 && d.reports_seen == 0 &&
        sub(o->now_ms, p->has_last_warmup_ms ? p->last_warmup_ms : 0) >=
        max(FIPS_PROBE_WARMUP_RETRY_MS, p->budgets.tick_ms)) {
        p->report.warmups_sent = 2;
        p->has_last_warmup_ms = true;
        p->last_warmup_ms = o->now_ms;
        push(a, FIPS_PROBE_SendWarmup);
    }
    if (sub(o->now_ms, p->stage_started_ms) >= p->budgets.rtt_ms) {
        finish_stage(p, record(FIPS_PROBE_VERDICT_Failed, classify_rtt_failure(d)), o);
        enter_terminal(p, o);
    }
}
fips_status fips_probe_step(fips_probe *p, const fips_probe_observation *o, fips_probe_actions *out)
{
    fips_probe_actions a = {0};
    if (p->finished) { *out = a; return FIPS_OK; }
    if (o->session_error && (o->session_error_len >= FIPS_PROBE_DETAIL_CAPACITY ||
        memchr(o->session_error, '\0', o->session_error_len))) return FIPS_ERR_RANGE;
    absorb(p, o);
    if (p->owns_session && !o->session_is_ours) {
        p->owns_session = false;
        p->report.has_left_intact = true;
        p->report.left_intact = p->open_requested ? FIPS_PROBE_LEFT_Replaced : FIPS_PROBE_LEFT_Preexisting;
    }
    if (p->stage != FIPS_PROBE_Terminal) {
        if (o->now_ms >= p->deadline_ms) {
            expire_running_stage(p);
            enter_terminal(p, o);
        } else {
            for (;;) {
                fips_probe_stage before = p->stage;
                switch (p->stage) {
                case FIPS_PROBE_Bloom: step_bloom(p, o, &a); break;
                case FIPS_PROBE_Discovery: step_discovery(p, o); break;
                case FIPS_PROBE_Path: step_path(p, o); break;
                case FIPS_PROBE_Session: step_session(p, o, &a); break;
                case FIPS_PROBE_Rtt: step_rtt(p, o, &a); break;
                case FIPS_PROBE_Terminal: break;
                }
                if (p->stage == before || p->stage == FIPS_PROBE_Terminal) break;
            }
        }
    }
    if (p->stage == FIPS_PROBE_Terminal) drive_terminal(p, o, &a);
    *out = a;
    return FIPS_OK;
}
void fips_probe_cancel(fips_probe *p, uint64_t now, fips_probe_actions *out)
{
    *out = (fips_probe_actions){0};
    if (p->finished) return;
    expire_running_stage(p);
    p->report.overall = FIPS_PROBE_OVERALL_Cancelled;
    p->stage = FIPS_PROBE_Terminal;
    p->has_ended_ms = true;
    p->ended_ms = now;
    if (p->owns_session && !p->teardown_emitted) {
        p->teardown_emitted = p->report.torn_down = true;
        push(out, FIPS_PROBE_TeardownSession);
    }
    p->finished = true;
    push(out, FIPS_PROBE_Finish);
}
void fips_probe_describe_path(const fips_tree_coordinate *my, const fips_tree_coordinate *their,
    fips_probe_path_facts *out)
{
    memset(out, 0, sizeof(*out));
    out->our_coords_len = my->len;
    out->their_coords_len = their->len;
    for (size_t i = 0; i < my->len; ++i) out->our_coords[i] = my->entries[i].node_addr;
    for (size_t i = 0; i < their->len; ++i) out->their_coords[i] = their->entries[i].node_addr;
    out->our_depth = fips_tree_coordinate_depth(my);
    out->their_depth = fips_tree_coordinate_depth(their);
    const fips_node_addr *lca = fips_tree_coordinate_lca(my, their);
    out->same_root = out->has_lca = out->has_lca_depth = out->has_tree_hops_up =
        out->has_tree_hops_down = out->has_tree_distance = lca != NULL;
    if (lca) {
        out->lca = *lca;
        out->lca_depth = fips_tree_coordinate_lca_depth(my, their);
        out->tree_hops_up = out->our_depth > out->lca_depth ? out->our_depth - out->lca_depth : 0;
        out->tree_hops_down = out->their_depth > out->lca_depth ? out->their_depth - out->lca_depth : 0;
        out->tree_distance = out->tree_hops_up + out->tree_hops_down;
    }
}
