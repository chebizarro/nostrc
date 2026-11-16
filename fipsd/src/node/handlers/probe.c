/* Diagnostic probe jobs (upstream handlers/probe.rs): a bounded registry of
 * fips_probe machines driven by observations of lookup, routing, session and
 * MMP state; actions initiate lookups, open/tear down owned sessions and send
 * coordinate warmups. Reports are retained for FIPS_PROBE_REAP_MS. */
#include "node/handlers/internal.h"
#include "control/probe.h"

static probe_job *job_by_id(handlers *h, uint64_t id)
{
    for (size_t i = 0; i < FIPSD_HANDLERS_PROBE_JOBS; ++i) if (h->probes[i].used && h->probes[i].id == id) return &h->probes[i];
    return NULL;
}
static bool target_claimed(handlers *h, const fips_node_addr *target, const probe_job *except)
{
    for (size_t i = 0; i < FIPSD_HANDLERS_PROBE_JOBS; ++i) {
        probe_job *j = &h->probes[i];
        if (j->used && j != except && !fips_probe_is_finished(j->probe) && !memcmp(j->target.bytes, target->bytes, 16)) return true;
    }
    return false;
}
/* probe.rs probe_session_is_ours: created by this job, as initiator, and not
 * replaced since (a session created before the probe started is not ours). */
static bool session_is_ours(handlers *h, const probe_job *j, const session_ext *x)
{
    return x && x->is_initiator && x->created.ops && j->probe->started_ms <= hs_elapsed_ms(x->created, h->origin);
}
static void observe(handlers *h, probe_job *j, fips_probe_observation *obs, fips_probe_next_hop_facts *hop)
{
    memset(obs, 0, sizeof *obs);
    obs->now_ms = hs_now_ms(h);
    obs->coords_cached = fips_coord_cache_contains(&h->cache, j->target.bytes, hs_now(h));
    obs->lookup_pending = fips_lookup_pending(&h->lookup, &j->target) != NULL;
    obs->target_is_direct_peer = hs_rec_find(h, &j->target) != NULL;
    fips_session_entry *entry = fips_session_table_by_peer(&h->sessions, j->target);
    session_ext *x = entry ? hs_session_ext(h, entry) : NULL;
    obs->session_present = entry != NULL;
    obs->session_established = x && x->established;
    obs->session_is_ours = session_is_ours(h, j, x);
    /* Baseline for the adoption check (probe.rs): data on the session after
     * this point means it is no longer the probe's to remove. */
    if (obs->session_is_ours && obs->session_established && !j->has_baseline) {
        j->has_baseline = true; j->sent_at_establish = x->sent; j->received_at_establish = x->received;
        j->last_data_at_establish = x->last_data;
    }
    if (x && x->mmp_live) {
        fips_mmp_metrics *m = &x->mmp.roles.metrics;
        obs->counters = (fips_probe_rtt_counters){m->reports_seen, m->rtt_samples, m->rtt_zero, m->rtt_arith_fail};
        if (m->has_last_rtt) { obs->has_last_rtt_ms = true; obs->last_rtt_ms = m->last_rtt_ms; }
        double srtt;
        if (fips_mmp_metrics_srtt_ms(m, &srtt)) { obs->has_srtt_ms = true; obs->srtt_ms = srtt; }
        if (x->mmp.path_mtu.current_mtu && x->mmp.path_mtu.current_mtu != UINT16_MAX) { obs->has_path_mtu = true; obs->path_mtu = x->mmp.path_mtu.current_mtu; }
    }
    const fips_tree_coordinate *their = hs_tree_peer_coords(h, &j->target);
    if (!their) their = hs_cache_coords(h, &j->target, hs_now(h));
    if (their) {
        fips_probe_describe_path(fips_tree_state_my_coords(h->tree), their, h->probe_facts);
        obs->path = h->probe_facts;
        fips_node_addr next;
        fips_routing_peer peers[64]; size_t n = 0;
        for (size_t i = 0; i < h->cfg.peer_capacity && n < 64; ++i) if (h->recs[i].used)
            peers[n++] = (fips_routing_peer){.addr = h->recs[i].addr, .coords = hs_tree_peer_coords(h, &h->recs[i].addr),
                .filter = hs_bloom_peer_filter(h, &h->recs[i].addr), .link_cost = hs_mmp_link_cost(h, &h->recs[i])};
        fips_routing_view rv = {.peers = peers, .peer_count = n, .ctx = h, .cached_coords = hs_cache_coords};
        if (obs->target_is_direct_peer) {
            *hop = (fips_probe_next_hop_facts){.node_addr = j->target, .route_class = FIPS_ROUTE_DirectPeer, .direct_peer = true};
            obs->next_hop = hop;
        } else if (fips_select_best_candidate(&rv, &j->target, their, fips_tree_state_my_coords(h->tree), &next)) {
            *hop = (fips_probe_next_hop_facts){.node_addr = next,
                .route_class = fips_classify_forward(&j->target, &next, &h->me, fips_tree_state_my_coords(h->tree), their, hs_tree_peer_coords(h, &next)),
                .direct_peer = hs_rec_find(h, &next) != NULL};
            obs->next_hop = hop;
        } else { obs->has_no_hop_reason = true; obs->no_hop_reason = FIPS_PROBE_HOP_NoCloserPeer; }
    } else { obs->has_no_hop_reason = true; obs->no_hop_reason = FIPS_PROBE_HOP_NoCoords; }
}
static void execute(handlers *h, probe_job *j, const fips_probe_actions *actions)
{
    for (size_t i = 0; i < actions->len; ++i) {
        switch (actions->items[i]) {
        case FIPS_PROBE_InitiateLookup: (void)hs_lookup_initiate(h, &j->target); break;
        case FIPS_PROBE_OpenSession:
            if (!fips_session_table_by_peer(&h->sessions, j->target)) (void)hs_session_open(h, &j->target);
            break;
        case FIPS_PROBE_SendWarmup: {
            fips_session_entry *entry = fips_session_table_by_peer(&h->sessions, j->target);
            session_ext *x = entry ? hs_session_ext(h, entry) : NULL;
            if (x && x->established) (void)hs_session_send_message(h, entry, x, FIPS_SESSION_CoordsWarmup, NULL, 0);
            break;
        }
        case FIPS_PROBE_TeardownSession: {
            fips_session_entry *entry = fips_session_table_by_peer(&h->sessions, j->target);
            session_ext *x = entry ? hs_session_ext(h, entry) : NULL;
            /* probe.rs probe_teardown/probe_session_disowned: only a session
             * this job created, still owns and that carried no data since it
             * was established is torn down. Otherwise the refusal and its
             * reason are recorded for the report; a session already gone is
             * neither torn down nor reported as left in place. */
            if (!entry || !x) break;
            if (!fips_probe_owns_session(j->probe)) { j->teardown_refused = true; j->refused_reason = FIPS_PROBE_LEFT_Preexisting; break; }
            if (!session_is_ours(h, j, x)) { j->teardown_refused = true; j->refused_reason = FIPS_PROBE_LEFT_Replaced; break; }
            if (j->has_baseline && (x->sent != j->sent_at_establish || x->received != j->received_at_establish ||
                    memcmp(x->last_data.opaque, j->last_data_at_establish.opaque, sizeof x->last_data.opaque) != 0)) {
                j->teardown_refused = true; j->refused_reason = FIPS_PROBE_LEFT_AdoptedByTraffic; break;
            }
            {
                fips_rekey_carry carry = {0};
                fips_rekey_outputs_clear(&h->session_out);
                if (fips_session_table_remove(&h->sessions, entry, FIPS_REKEY_CLOSE_Admin, &h->session_out, &carry)) {
                    for (size_t k = 0; k < h->session_out.count; ++k) if (h->session_out.items[k].kind == FIPS_REKEY_OUT_Release) {
                        uint32_t index;
                        if (fips_rekey_index_lease_release(&h->session_out.items[k].lease, &index)) (void)fips_index_allocator_free(&h->indices, (fips_session_index){index});
                    }
                }
                fips_rekey_outputs_clear(&h->session_out);
                fipsd_dp_mtu_release(h->dataplane, &j->target); /* release probe-owned clamp */
                x->used = false; x->established = false; ++h->counters.sess_closed;
            }
            break;
        }
        case FIPS_PROBE_Finish:
            j->finished_ms = hs_now_ms(h);
            break;
        }
    }
}
static void drive(handlers *h, probe_job *j)
{
    if (fips_probe_is_finished(j->probe)) return;
    fips_probe_observation obs; fips_probe_next_hop_facts hop;
    observe(h, j, &obs, &hop);
    fips_probe_actions actions;
    if (fips_probe_step(j->probe, &obs, &actions) == FIPS_OK) execute(h, j, &actions);
}
static json_t *render(handlers *h, probe_job *j)
{
    fips_probe_snapshot_get(j->probe, j->report);
    /* control/probe.rs: a teardown the host refused is reported as the
     * refusal, and the core's `torn_down` is withdrawn. */
    if (j->teardown_refused) {
        j->report->torn_down = false;
        j->report->has_left_intact = true; j->report->left_intact = j->refused_reason;
    }
    char hex[33]; (void)fips_node_addr_format(&j->target, hex, sizeof hex);
    char ipv6[64]; fips_ipv6_address a = fips_ipv6_address_from_node_addr(&j->target);
    (void)fips_ipv6_address_format(&a, ipv6, sizeof ipv6);
    char npub[FIPS_NIP19_CAPACITY] = "", display[16];
    const fips_peer_identity *id = fipsd_identity_cache_touch(&h->identities, fips_node_addr_prefix(&j->target), hs_wall_ms(h));
    if (id) (void)fips_peer_identity_npub(id, npub, sizeof npub);
    (void)fips_node_addr_short_hex(&j->target, display, sizeof display);
    uint8_t attempts = 0;
    fips_pending_lookup *p = fips_lookup_pending(&h->lookup, &j->target);
    if (p) attempts = p->attempt;
    fipsd_control_probe_report report = {.snapshot = j->report, .probe_id = j->id,
        .elapsed_ms = fips_probe_elapsed_ms(j->probe, hs_now_ms(h)), .npub = npub, .node_addr = hex,
        .display_name = display, .ipv6_addr = ipv6, .attempt_timeouts_secs = h->cfg.lookup_attempt_timeouts_secs,
        .timeout_count = h->cfg.lookup_attempt_count, .attempts = &attempts};
    return fipsd_control_probe_render(&report);
}
static void job_free(probe_job *j) { free(j->probe); free(j->report); memset(j, 0, sizeof *j); }
json_t *hs_probe_start(handlers *h, const fips_peer_identity *target)
{
    if (!memcmp(target->node_addr.bytes, h->me.bytes, 16)) return fipsd_control_error("cannot probe self");
    probe_job *slot = NULL; size_t live = 0;
    for (size_t i = 0; i < FIPSD_HANDLERS_PROBE_JOBS; ++i) {
        if (h->probes[i].used && !fips_probe_is_finished(h->probes[i].probe)) ++live;
        else if (!h->probes[i].used && !slot) slot = &h->probes[i];
    }
    if (live >= FIPS_PROBE_MAX_CONCURRENT_PROBES || !slot) return fipsd_control_error("too many concurrent probes");
    slot->probe = calloc(1, sizeof *slot->probe); slot->report = calloc(1, sizeof *slot->report);
    if (!slot->probe || !slot->report) { job_free(slot); return NULL; }
    slot->used = true; slot->id = ++h->next_probe_id; slot->target = target->node_addr;
    (void)fipsd_identity_cache_put(&h->identities, *target, hs_wall_ms(h));
    fips_probe_budgets budgets = fips_probe_budgets_derive(h->cfg.tick_ms, h->cfg.lookup_attempt_timeouts_secs, h->cfg.lookup_attempt_count);
    fips_probe_preflight pre = {.session_present = fips_session_table_by_peer(&h->sessions, slot->target) != NULL,
        .coords_cached = fips_coord_cache_contains(&h->cache, slot->target.bytes, hs_now(h)), .identity_cached = true,
        .target_claimed = target_claimed(h, &slot->target, slot)};
    fips_probe_new(slot->probe, hs_now_ms(h), budgets, pre);
    drive(h, slot);
    return fipsd_control_ok(json_pack("{s:I,s:s}", "probe_id", (json_int_t)slot->id, "state", fips_probe_is_finished(slot->probe) ? "done" : "running"));
}
json_t *hs_probe_poll(handlers *h, uint64_t id)
{
    probe_job *j = job_by_id(h, id);
    if (!j) return fipsd_control_error("unknown probe id");
    drive(h, j);
    json_t *report = render(h, j);
    bool done = fips_probe_is_finished(j->probe);
    json_t *d = json_pack("{s:s,s:o}", "state", done ? "done" : "running", "report", report ? report : json_null());
    if (done) job_free(j);
    return fipsd_control_ok(d);
}
json_t *hs_probe_cancel(handlers *h, uint64_t id)
{
    probe_job *j = job_by_id(h, id);
    if (!j) return fipsd_control_error("unknown probe id");
    fips_probe_actions actions;
    fips_probe_cancel(j->probe, hs_now_ms(h), &actions);
    execute(h, j, &actions);
    j->finished_ms = hs_now_ms(h);
    return fipsd_control_ok(json_pack("{s:s}", "state", "cancelled"));
}
void hs_probe_tick(handlers *h, fips_instant now)
{
    (void)now;
    for (size_t i = 0; i < FIPSD_HANDLERS_PROBE_JOBS; ++i) {
        probe_job *j = &h->probes[i];
        if (!j->used) continue;
        if (!fips_probe_is_finished(j->probe)) drive(h, j);
        if (fips_probe_is_finished(j->probe)) {
            if (!j->finished_ms) j->finished_ms = hs_now_ms(h);
            if (hs_now_ms(h) - j->finished_ms > FIPS_PROBE_REAP_MS) job_free(j);
        }
    }
}
void hs_probe_close_all(handlers *h)
{
    for (size_t i = 0; i < FIPSD_HANDLERS_PROBE_JOBS; ++i) if (h->probes[i].used) job_free(&h->probes[i]);
}
