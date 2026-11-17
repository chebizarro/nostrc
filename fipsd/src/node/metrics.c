#include "node/handlers/internal.h"
static const struct metric_info { const char *name; fipsd_metric_family family; } infos[] = {
    {"coord_hint_changed", FIPSD_METRICS_forwarding},
    {"coord_hint_rejected", FIPSD_METRICS_forwarding},
    {"coord_hint_unfunded", FIPSD_METRICS_forwarding},
    {"coord_warm_foreign_root", FIPSD_METRICS_forwarding},
    {"coord_warm_key_mismatch", FIPSD_METRICS_forwarding},
    {"decode_error_bytes", FIPSD_METRICS_forwarding},
    {"decode_error_packets", FIPSD_METRICS_forwarding},
    {"delivered_bytes", FIPSD_METRICS_forwarding},
    {"delivered_packets", FIPSD_METRICS_forwarding},
    {"drop_mtu_exceeded_bytes", FIPSD_METRICS_forwarding},
    {"drop_mtu_exceeded_packets", FIPSD_METRICS_forwarding},
    {"drop_no_route_bytes", FIPSD_METRICS_forwarding},
    {"drop_no_route_packets", FIPSD_METRICS_forwarding},
    {"drop_send_error_bytes", FIPSD_METRICS_forwarding},
    {"drop_send_error_packets", FIPSD_METRICS_forwarding},
    {"forwarded_bytes", FIPSD_METRICS_forwarding},
    {"forwarded_packets", FIPSD_METRICS_forwarding},
    {"originated_bytes", FIPSD_METRICS_forwarding},
    {"originated_packets", FIPSD_METRICS_forwarding},
    {"received_bytes", FIPSD_METRICS_forwarding},
    {"received_packets", FIPSD_METRICS_forwarding},
    {"route_crosslink_ascend", FIPSD_METRICS_forwarding},
    {"route_crosslink_descend", FIPSD_METRICS_forwarding},
    {"route_direct_peer", FIPSD_METRICS_forwarding},
    {"route_tree_down", FIPSD_METRICS_forwarding},
    {"route_tree_down_cross", FIPSD_METRICS_forwarding},
    {"route_tree_up", FIPSD_METRICS_forwarding},
    {"ttl_exhausted_bytes", FIPSD_METRICS_forwarding},
    {"ttl_exhausted_packets", FIPSD_METRICS_forwarding},
    {"warm_malformed_bytes", FIPSD_METRICS_forwarding},
    {"warm_malformed_packets", FIPSD_METRICS_forwarding},
    {"req_backoff_suppressed", FIPSD_METRICS_lookup},
    {"req_bloom_miss", FIPSD_METRICS_lookup},
    {"req_decode_error", FIPSD_METRICS_lookup},
    {"req_dedup_cache_full", FIPSD_METRICS_lookup},
    {"req_dedup_evicted", FIPSD_METRICS_lookup},
    {"req_deduplicated", FIPSD_METRICS_lookup},
    {"req_duplicate", FIPSD_METRICS_lookup},
    {"req_fallback_forwarded", FIPSD_METRICS_lookup},
    {"req_forward_rate_limited", FIPSD_METRICS_lookup},
    {"req_forwarded", FIPSD_METRICS_lookup},
    {"req_initiated", FIPSD_METRICS_lookup},
    {"req_no_tree_peer", FIPSD_METRICS_lookup},
    {"req_own_loopback", FIPSD_METRICS_lookup},
    {"req_received", FIPSD_METRICS_lookup},
    {"req_sign_rate_limited", FIPSD_METRICS_lookup},
    {"req_target_is_us", FIPSD_METRICS_lookup},
    {"req_ttl_exhausted", FIPSD_METRICS_lookup},
    {"resp_accepted", FIPSD_METRICS_lookup},
    {"resp_decode_error", FIPSD_METRICS_lookup},
    {"resp_forwarded", FIPSD_METRICS_lookup},
    {"resp_identity_miss", FIPSD_METRICS_lookup},
    {"resp_no_route", FIPSD_METRICS_lookup},
    {"resp_proof_failed", FIPSD_METRICS_lookup},
    {"resp_received", FIPSD_METRICS_lookup},
    {"resp_timed_out", FIPSD_METRICS_lookup},
    {"resp_unsolicited", FIPSD_METRICS_lookup},
    {"accepted", FIPSD_METRICS_tree},
    {"addr_mismatch", FIPSD_METRICS_tree},
    {"ancestry_changed", FIPSD_METRICS_tree},
    {"ancestry_invalid", FIPSD_METRICS_tree},
    {"decode_error", FIPSD_METRICS_tree},
    {"flap_dampened", FIPSD_METRICS_tree},
    {"loop_detected", FIPSD_METRICS_tree},
    {"outbound_sign_failed", FIPSD_METRICS_tree},
    {"parent_losses", FIPSD_METRICS_tree},
    {"parent_switches", FIPSD_METRICS_tree},
    {"rate_limited", FIPSD_METRICS_tree},
    {"received", FIPSD_METRICS_tree},
    {"send_failed", FIPSD_METRICS_tree},
    {"sent", FIPSD_METRICS_tree},
    {"sig_failed", FIPSD_METRICS_tree},
    {"stale", FIPSD_METRICS_tree},
    {"unknown_peer", FIPSD_METRICS_tree},
    {"accepted", FIPSD_METRICS_bloom},
    {"child_role_rejected", FIPSD_METRICS_bloom},
    {"debounce_suppressed", FIPSD_METRICS_bloom},
    {"decode_error", FIPSD_METRICS_bloom},
    {"fill_exceeded", FIPSD_METRICS_bloom},
    {"invalid", FIPSD_METRICS_bloom},
    {"non_v1", FIPSD_METRICS_bloom},
    {"received", FIPSD_METRICS_bloom},
    {"send_failed", FIPSD_METRICS_bloom},
    {"sent", FIPSD_METRICS_bloom},
    {"stale", FIPSD_METRICS_bloom},
    {"unknown_peer", FIPSD_METRICS_bloom},
    {"ce_forwarded", FIPSD_METRICS_congestion},
    {"ce_received", FIPSD_METRICS_congestion},
    {"congestion_detected", FIPSD_METRICS_congestion},
    {"kernel_drop_events", FIPSD_METRICS_congestion},
    {"broken_below_quorum", FIPSD_METRICS_errors},
    {"broken_demoted", FIPSD_METRICS_errors},
    {"broken_link_mismatch", FIPSD_METRICS_errors},
    {"broken_reporter_mismatch", FIPSD_METRICS_errors},
    {"coords_required", FIPSD_METRICS_errors},
    {"emit_limiter_at_capacity", FIPSD_METRICS_errors},
    {"emit_over_dest_interval", FIPSD_METRICS_errors},
    {"emit_over_peer_budget", FIPSD_METRICS_errors},
    {"lookup_resp_mtu_below_floor", FIPSD_METRICS_errors},
    {"mtu_exceeded", FIPSD_METRICS_errors},
    {"mtu_exceeded_below_floor", FIPSD_METRICS_errors},
    {"mtu_exceeded_uncorroborated", FIPSD_METRICS_errors},
    {"path_broken", FIPSD_METRICS_errors},
    {"path_mtu_notif_below_floor", FIPSD_METRICS_errors},
    {"unbound_broken", FIPSD_METRICS_errors},
    {"unbound_coords", FIPSD_METRICS_errors},
    {"unbound_forged", FIPSD_METRICS_errors},
    {"unbound_mtu", FIPSD_METRICS_errors},
    {"drop_arrival_queue_full", FIPSD_METRICS_native},
    {"drop_backlog_full", FIPSD_METRICS_native},
    {"drop_client_buffer_full", FIPSD_METRICS_native},
    {"drop_flow_queue_full", FIPSD_METRICS_native},
    {"drop_listener_gone", FIPSD_METRICS_native},
    {"drop_listener_not_reading", FIPSD_METRICS_native},
    {"drop_no_port", FIPSD_METRICS_native},
    {"drop_oversize", FIPSD_METRICS_native},
    {"drop_peer_share", FIPSD_METRICS_native},
    {"drop_pending_queue_full", FIPSD_METRICS_native},
    {"drop_too_many_flows", FIPSD_METRICS_native},
    {"flows_accepted", FIPSD_METRICS_native},
    {"flows_closed", FIPSD_METRICS_native},
    {"flows_expired", FIPSD_METRICS_native},
    {"flows_opened", FIPSD_METRICS_native},
    {"received_bytes", FIPSD_METRICS_native},
    {"received_datagrams", FIPSD_METRICS_native},
    {"sent_bytes", FIPSD_METRICS_native},
    {"sent_datagrams", FIPSD_METRICS_native},
    {"unknown_index", FIPSD_METRICS_dataplane},
    {"orphaned_index", FIPSD_METRICS_dataplane},
    {"bad_state", FIPSD_METRICS_handshake},
    {"unknown_connection", FIPSD_METRICS_handshake},
    {"silent_backoff", FIPSD_METRICS_handshake},
    {"max_peers", FIPSD_METRICS_handshake},
    {"rekey_pending", FIPSD_METRICS_handshake},
    {"rekey_tiebreak", FIPSD_METRICS_handshake},
    {"answered_before", FIPSD_METRICS_handshake},
    {"off_link", FIPSD_METRICS_handshake},
    {"unknown_session", FIPSD_METRICS_session},
    {"bad_state", FIPSD_METRICS_session},
    {"addr_mismatch", FIPSD_METRICS_session},
    {"rekey_key_mismatch", FIPSD_METRICS_session},
    {"rekey_armed", FIPSD_METRICS_session},
    {"rekey_tiebreak", FIPSD_METRICS_session},
    {"rekey_yielded", FIPSD_METRICS_session},
    {"rekey_held", FIPSD_METRICS_session},
    {"rekey_pending", FIPSD_METRICS_session},
    {"rekey_expired", FIPSD_METRICS_session},
    {"rekey_unanswered", FIPSD_METRICS_session},
    {"pending_replaced", FIPSD_METRICS_session},
    {"peer_restarted", FIPSD_METRICS_session},
    {"ack_handshake_failed", FIPSD_METRICS_session},
    {"setup_rate_limited", FIPSD_METRICS_session},
    {"table_full", FIPSD_METRICS_session},
    {"half_open_full", FIPSD_METRICS_session},
    {"pending_destinations_full", FIPSD_METRICS_session},
};
void fipsd_metrics_init(fipsd_metrics *m)
{ for (size_t i = 0; i < FIPSD_METRIC_COUNT; ++i) atomic_init(&m->values[i], 0); }
void fipsd_metrics_add(fipsd_metrics *m, fipsd_metric k, uint64_t n)
{ atomic_fetch_add_explicit(&m->values[k], n, memory_order_relaxed); }
void fipsd_metrics_set(fipsd_metrics *m, fipsd_metric k, uint64_t n)
{ atomic_store_explicit(&m->values[k], n, memory_order_relaxed); }
uint64_t fipsd_metrics_get(const fipsd_metrics *m, fipsd_metric k)
{ return atomic_load_explicit(&m->values[k], memory_order_relaxed); }
size_t fipsd_metrics_snapshot(const fipsd_metrics *m, fipsd_metric_family family, fipsd_control_counter *out)
{
    size_t n = 0;
    for (size_t i = 0; i < FIPSD_METRIC_COUNT; ++i) if (infos[i].family == family)
        out[n++] = (fipsd_control_counter){infos[i].name, fipsd_metrics_get(m, (fipsd_metric)i)};
    return n;
}
/* Legacy progress totals are still useful to protocol handlers. Project only
 * semantically identical counters; rejection reasons have their own callsites. */
void hs_metrics_refresh(handlers *h)
{
    const handler_counters *c = &h->counters;
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_forwarding_received_packets, c->fwd_received_packets);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_forwarding_received_bytes, c->fwd_received_bytes);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_forwarding_delivered_packets, c->fwd_delivered_packets);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_forwarding_delivered_bytes, c->fwd_delivered_bytes);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_forwarding_forwarded_packets, c->fwd_forwarded_packets);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_forwarding_forwarded_bytes, c->fwd_forwarded_bytes);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_forwarding_originated_packets, c->fwd_originated_packets);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_forwarding_originated_bytes, c->fwd_originated_bytes);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_forwarding_drop_no_route_packets, c->fwd_drop_no_route_packets);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_forwarding_drop_no_route_bytes, c->fwd_drop_no_route_bytes);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_forwarding_ttl_exhausted_packets, c->fwd_ttl_exhausted_packets);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_forwarding_decode_error_packets, c->fwd_decode_error_packets);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_req_received, c->lookup_req_received);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_req_initiated, c->lookup_req_initiated);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_req_forwarded, c->lookup_req_forwarded);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_req_target_is_us, c->lookup_req_target_is_us);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_req_duplicate, c->lookup_req_duplicate);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_req_ttl_exhausted, c->lookup_req_ttl_exhausted);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_req_no_tree_peer, c->lookup_req_no_tree_peer);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_req_sign_rate_limited, c->lookup_req_rate_limited);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_req_backoff_suppressed, c->lookup_req_backoff_suppressed);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_req_bloom_miss, c->lookup_req_bloom_miss);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_resp_received, c->lookup_resp_received);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_resp_accepted, c->lookup_resp_accepted);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_resp_forwarded, c->lookup_resp_forwarded);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_resp_unsolicited, c->lookup_resp_unsolicited);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_resp_proof_failed, c->lookup_resp_proof_failed);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_lookup_resp_timed_out, c->lookup_resp_timed_out);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_tree_received, c->tree_received);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_tree_accepted, c->tree_accepted);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_tree_sent, c->tree_sent);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_tree_parent_switches, c->tree_switches);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_tree_parent_losses, c->tree_losses);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_tree_outbound_sign_failed, c->tree_sign_failed);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_tree_rate_limited, c->tree_rate_limited);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_bloom_received, c->bloom_received);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_bloom_accepted, c->bloom_accepted);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_bloom_sent, c->bloom_sent);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_bloom_invalid, c->bloom_refused);
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_bloom_send_failed, c->bloom_send_failed);
    /* native.received_* are added at delivery (hs_native_deliver) or by the
     * native API's own dispatch, never set from a dispatcher count. */
    fipsd_metrics_set(&h->metrics, FIPSD_METRIC_session_setup_rate_limited, c->sess_setup_rate_limited);
}

void hs_metrics_rekey(handlers *h, bool session, const fips_rekey_output *o)
{
    if (o->kind == FIPS_REKEY_OUT_Reject) {
        if (session) {
            switch (o->reason) {
            case FIPS_REKEY_REASON_KeyMismatch:
                /* Proven sessions keep their established key on a bad rekey;
                 * a first XK final must instead authenticate its source. */
                METRIC(h, session, rekey_key_mismatch); break;
            case FIPS_REKEY_REASON_TieBreakKept: METRIC(h, session, rekey_tiebreak); break;
            case FIPS_REKEY_REASON_CandidatesFull: METRIC(h, session, rekey_held); break;
            case FIPS_REKEY_REASON_PendingHeld: METRIC(h, session, rekey_pending); break;
            case FIPS_REKEY_REASON_ReplyUnauthenticated: METRIC(h, session, ack_handshake_failed); break;
            case FIPS_REKEY_REASON_FinalNoAttempt: METRIC(h, session, bad_state); break;
            case FIPS_REKEY_REASON_NotEstablished: METRIC(h, session, bad_state); break;
            case FIPS_REKEY_REASON_CandidateExpired: METRIC(h, session, rekey_expired); break;
            default: break;
            }
        } else {
            switch (o->reason) {
            case FIPS_REKEY_REASON_AnsweredBefore: METRIC(h, handshake, answered_before); break;
            case FIPS_REKEY_REASON_OffLink: METRIC(h, handshake, off_link); break;
            case FIPS_REKEY_REASON_PendingHeld: METRIC(h, handshake, rekey_pending); break;
            case FIPS_REKEY_REASON_DualInitWon: METRIC(h, handshake, rekey_tiebreak); break;
            default: break;
            }
        }
    }
    if (!session) return;
    if (o->kind == FIPS_REKEY_OUT_PeerRestarted) METRIC(h, session, peer_restarted);
    if (o->kind == FIPS_REKEY_OUT_Abandoned) {
        if (o->abandon == FIPS_REKEY_ABANDON_Unanswered) METRIC(h, session, rekey_unanswered);
        if (o->abandon == FIPS_REKEY_ABANDON_Yielded) METRIC(h, session, rekey_yielded);
        if (o->abandon == FIPS_REKEY_ABANDON_Superseded) METRIC(h, session, pending_replaced);
    }
}
