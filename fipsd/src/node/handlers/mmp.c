/* Link and session MMP (upstream handlers/mmp.rs, session MMP): per-peer
 * sender/receiver/metrics state lives in the ActivePeer; frames feed it, the
 * tick plans sender/receiver reports and logs, receiver reports update SRTT,
 * delivery evidence for bloom/STP acks and the link cost used by routing. */
#include "node/handlers/internal.h"

void hs_mmp_peer_added(handlers *h, peer_rec *rec)
{
    if (!fipsd_active_peer_mmp_init(rec->peer, h->cfg.core.mmp_mode)) FIPSD_WARN(HMOD, "MMP init refused");
}
void hs_mmp_frame_received(handlers *h, peer_rec *rec, uint64_t counter, uint32_t sender_ts, size_t bytes, bool ce)
{
    fips_mmp_peer_state *mmp = fipsd_active_peer_mmp(rec->peer);
    if (mmp) fips_mmp_receiver_record_frame(&mmp->receiver, counter, sender_ts, bytes, ce, hs_now_ms(h), true);
}
void hs_mmp_frame_sent(handlers *h, peer_rec *rec, uint64_t counter, uint32_t timestamp, size_t bytes)
{
    (void)h;
    fips_mmp_peer_state *mmp = fipsd_active_peer_mmp(rec->peer);
    if (mmp) fips_mmp_sender_record_sent(&mmp->sender, counter, timestamp, bytes);
}
static uint32_t our_timestamp(handlers *h, peer_rec *rec)
{
    fips_rekey_view v = fipsd_active_peer_link_view(rec->peer);
    return v.current.present ? (uint32_t)hs_elapsed_ms(hs_now(h), v.current.base) : 0;
}
void hs_mmp_report_received(handlers *h, peer_rec *rec, fips_link_message_type type, const uint8_t *body, size_t len)
{
    fips_mmp_peer_state *mmp = fipsd_active_peer_mmp(rec->peer);
    fips_reader r; fips_reader_init(&r, body, len);
    if (type == FIPS_LINK_SenderReport) {
        fips_mmp_sender_report sr;
        if (fips_mmp_sender_report_decode(&r, &sr, NULL) == FIPS_OK) ++h->counters.mmp_sr_received;
        return; /* Upstream traces SRs; metrics come from receiver reports. */
    }
    fips_mmp_receiver_report rr;
    if (fips_mmp_receiver_report_decode(&r, &rr, NULL) != FIPS_OK || !mmp) return;
    ++h->counters.mmp_rr_received;
    fips_mmp_rr_log log;
    (void)fips_mmp_metrics_process_receiver_report(&mmp->metrics, &rr, our_timestamp(h, rec), hs_now_ms(h), &log);
    double srtt;
    if (fips_mmp_metrics_srtt_ms(&mmp->metrics, &srtt)) {
        int64_t us = (int64_t)(srtt * 1000.0);
        fips_mmp_sender_update_report_interval_from_srtt(&mmp->sender, us);
        fips_mmp_receiver_update_report_interval_from_srtt(&mmp->receiver, us);
        fipsd_peer_data *d = fipsd_active_peer_data(rec->peer);
        fips_link_stats_update_rtt(&d->link_stats, fips_duration_from_millis((uint64_t)srtt));
    }
    if (log.kind == FIPS_MMP_RrRttSample) { rec->rtt_ms = log.rtt_ms; rec->has_rtt = true; }
    fips_mmp_metrics_update_reverse_delivery(&mmp->metrics, rr.cumulative_packets_recv, rr.highest_counter);
    fips_rekey_view v = fipsd_active_peer_link_view(rec->peer);
    fips_rekey *m = fipsd_active_peer_link_machine(rec->peer);
    rec->evidence = (fips_mmp_link_evidence){.epoch = v.current.tag, .next_counter = m ? fips_rekey_sent(m) : 0,
        .rr = {rr.highest_counter, rr.cumulative_packets_recv, rr.cumulative_reorder_count}, .has_rr = true};
    rec->has_rr = true;
    hs_bloom_rr(h, rec);
    (void)fipsd_active_peer_publish(rec->peer);
    hs_mark_dirty(h);
}
static bool send_report(handlers *h, peer_rec *rec, fips_mmp_link_report_kind kind)
{
    fips_mmp_peer_state *mmp = fipsd_active_peer_mmp(rec->peer);
    if (!mmp) return false;
    fips_writer w; fips_writer_init(&w, h->tx_msg, sizeof h->tx_msg);
    if (kind == FIPS_MMP_LinkSender) {
        fips_mmp_sender_report sr;
        if (!fips_mmp_sender_build_report(&mmp->sender, hs_now_ms(h), &sr)) return false;
        if (fips_mmp_sender_report_encode(&sr, &w, NULL) != FIPS_OK) return false;
    } else {
        fips_mmp_receiver_report rr;
        if (!fips_mmp_receiver_build_report(&mmp->receiver, hs_now_ms(h), &rr)) return false;
        if (fips_mmp_receiver_report_encode(&rr, &w, NULL) != FIPS_OK) return false;
    }
    bool ok = hs_link_send(h, rec, h->tx_msg, fips_writer_len(&w));
    if (ok) { if (kind == FIPS_MMP_LinkSender) ++h->counters.mmp_sr_sent; else ++h->counters.mmp_rr_sent; fips_mmp_sender_record_send_success(&mmp->sender); }
    else fips_mmp_sender_record_send_failure(&mmp->sender);
    return ok;
}
void hs_mmp_tick(handlers *h, fips_instant now)
{
    uint64_t now_ms = hs_now_ms(h);
    /* Dead-link detection: authenticated silence beyond the configured timeout. */
    for (size_t i = 0; i < h->cfg.peer_capacity; ++i) {
        peer_rec *rec = &h->recs[i];
        if (!rec->used) continue;
        fips_instant since = rec->last_heard.ops ? rec->last_heard : rec->established_at;
        if (h->cfg.link_dead_timeout_ms && since.ops && hs_elapsed_ms(now, since) > h->cfg.link_dead_timeout_ms) {
            ++h->counters.links_dead;
            fipsd_peer_event e = {.kind = FIPSD_PEER_LINK_DEAD};
            fipsd_peer_actions a;
            fipsd_active_peer_step(rec->peer, now, &e, &a);
            (void)hs_peer_effects(h, rec->peer, &a);
        }
    }
    fips_mmp_link_report_snapshot snaps[64]; peer_rec *recs[64]; size_t n = 0;
    for (size_t i = 0; i < h->cfg.peer_capacity && n < 64; ++i) {
        peer_rec *rec = &h->recs[i];
        fips_mmp_peer_state *mmp = rec->used ? fipsd_active_peer_mmp(rec->peer) : NULL;
        if (!mmp) continue;
        recs[n] = rec;
        snaps[n++] = (fips_mmp_link_report_snapshot){.peer = rec->addr, .mode = mmp->mode,
            .send_sr = mmp->sender.interval_has_data, .send_rr = mmp->receiver.interval_has_data,
            .sr_due = fips_mmp_sender_should_send_report(&mmp->sender, now_ms),
            .rr_due = fips_mmp_receiver_should_send_report(&mmp->receiver, now_ms),
            .log_due = fips_mmp_peer_should_log(mmp, now_ms)};
    }
    fips_mmp_action items[64 * 3];
    fips_mmp_action_list actions = {items, 0, 64 * 3};
    if (fips_mmp_plan_link_reports(snaps, n, &actions) != FIPS_OK) return;
    for (size_t i = 0; i < actions.len; ++i) {
        fips_mmp_action *a = &items[i];
        peer_rec *rec = NULL;
        for (size_t j = 0; j < n && !rec; ++j) if (!memcmp(recs[j]->addr.bytes, a->peer.bytes, 16)) rec = recs[j];
        if (!rec || !rec->used) continue;
        if (a->kind == FIPS_MMP_SendLinkReport) (void)send_report(h, rec, a->report.link);
        else if (a->kind == FIPS_MMP_LogLink) {
            fips_mmp_peer_state *mmp = fipsd_active_peer_mmp(rec->peer);
            double srtt = 0; bool has = fips_mmp_metrics_srtt_ms(&mmp->metrics, &srtt);
            char hex[33]; (void)fips_node_addr_format(&rec->addr, hex, sizeof hex);
            FIPSD_DEBUG(HMOD, "mmp %s srtt=%s%.1fms loss=%.3f", hex, has ? "" : "n/a ", srtt, fips_mmp_metrics_loss_rate(&mmp->metrics));
            fips_mmp_peer_mark_logged(mmp, now_ms);
        }
    }
}
bool hs_mmp_srtt(handlers *h, const peer_rec *rec, double *out)
{
    (void)h;
    fips_mmp_peer_state *mmp = fipsd_active_peer_mmp(rec->peer);
    return mmp && fips_mmp_metrics_srtt_ms(&mmp->metrics, out);
}
double hs_mmp_link_cost(handlers *h, const peer_rec *rec)
{
    fips_mmp_peer_state *mmp = fipsd_active_peer_mmp(rec->peer);
    double etx = 1.0, srtt = 0.0;
    if (mmp) {
        double smoothed;
        if (fips_mmp_metrics_smoothed_etx(&mmp->metrics, &smoothed) && smoothed >= 1.0) etx = smoothed;
        (void)hs_mmp_srtt(h, rec, &srtt);
    }
    return etx * (1.0 + srtt / 1000.0);
}
void hs_mmp_session_message(handlers *h, fips_session_entry *entry, session_ext *x, uint8_t msg_type, const uint8_t *body, size_t len)
{
    (void)entry;
    if (!x->mmp_live) return;
    fips_reader r; fips_reader_init(&r, body, len);
    if (msg_type == FIPS_SESSION_SenderReport) {
        fips_mmp_session_sender_report sr;
        (void)fips_mmp_session_sender_report_decode(&r, &sr, NULL);
    } else if (msg_type == FIPS_SESSION_ReceiverReport) {
        fips_mmp_session_receiver_report srr;
        if (fips_mmp_session_receiver_report_decode(&r, &srr, NULL) != FIPS_OK) return;
        fips_mmp_receiver_report rr = fips_mmp_receiver_report_from_session_receiver_report(&srr);
        fips_rekey_view v = fips_rekey_get_view(&entry->machine);
        uint32_t ours = v.current.present ? (uint32_t)hs_elapsed_ms(hs_now(h), v.current.base) : 0;
        fips_mmp_rr_log log;
        (void)fips_mmp_metrics_process_receiver_report(&x->mmp.roles.metrics, &rr, ours, hs_now_ms(h), &log);
        double srtt;
        if (fips_mmp_metrics_srtt_ms(&x->mmp.roles.metrics, &srtt)) {
            int64_t us = (int64_t)(srtt * 1000.0);
            fips_mmp_sender_update_report_interval_with_bounds(&x->mmp.roles.sender, us, FIPS_MMP_MIN_SESSION_REPORT_INTERVAL_MS, FIPS_MMP_MAX_SESSION_REPORT_INTERVAL_MS);
            fips_mmp_receiver_update_report_interval_with_bounds(&x->mmp.roles.receiver, us, FIPS_MMP_MIN_SESSION_REPORT_INTERVAL_MS, FIPS_MMP_MAX_SESSION_REPORT_INTERVAL_MS);
            fips_mmp_path_mtu_update_interval_from_srtt(&x->mmp.path_mtu, srtt);
        }
        fips_mmp_metrics_update_reverse_delivery(&x->mmp.roles.metrics, rr.cumulative_packets_recv, rr.highest_counter);
    } else if (msg_type == FIPS_SESSION_PathMtuNotification) {
        fips_mmp_path_mtu_notification n;
        if (fips_mmp_path_mtu_notification_decode(&r, &n, NULL) == FIPS_OK)
            (void)fips_mmp_path_mtu_apply_notification(&x->mmp.path_mtu, n.path_mtu, hs_now_ms(h));
    }
    hs_mark_dirty(h);
}
void hs_mmp_session_tick(handlers *h, fips_instant now)
{
    (void)now;
    uint64_t now_ms = hs_now_ms(h);
    fips_mmp_session_report_snapshot snaps[64]; fips_session_entry *entries[64]; size_t n = 0;
    for (size_t i = 0; i < h->cfg.session_capacity && n < 64; ++i) {
        fips_session_entry *entry = &h->session_entries[i];
        session_ext *x = &h->session_ext[i];
        if (!entry->occupied || !x->used || !x->established || !x->mmp_live) continue;
        entries[n] = entry;
        snaps[n++] = (fips_mmp_session_report_snapshot){.dest = x->peer, .mode = x->mmp.roles.mode,
            .sr_due = x->mmp.roles.sender.interval_has_data && fips_mmp_sender_should_send_report(&x->mmp.roles.sender, now_ms),
            .rr_due = x->mmp.roles.receiver.interval_has_data && fips_mmp_receiver_should_send_report(&x->mmp.roles.receiver, now_ms),
            .mtu_due = fips_mmp_path_mtu_should_send_notification(&x->mmp.path_mtu, now_ms),
            .log_due = fips_mmp_session_should_log(&x->mmp, now_ms)};
    }
    fips_mmp_action items[64 * 4];
    fips_mmp_action_list actions = {items, 0, 64 * 4};
    if (fips_mmp_plan_session_reports(snaps, n, &actions) != FIPS_OK) return;
    for (size_t i = 0; i < actions.len; ++i) {
        fips_mmp_action *a = &items[i];
        fips_session_entry *entry = NULL;
        for (size_t j = 0; j < n && !entry; ++j) if (!memcmp(entries[j]->peer.bytes, a->peer.bytes, 16)) entry = entries[j];
        if (!entry || !entry->occupied) continue;
        session_ext *x = hs_session_ext(h, entry);
        if (a->kind == FIPS_MMP_LogSession) { fips_mmp_session_mark_logged(&x->mmp, now_ms); continue; }
        if (a->kind != FIPS_MMP_SendSessionReport) continue;
        uint8_t body[FIPS_MMP_RECEIVER_REPORT_SIZE]; fips_writer w; fips_writer_init(&w, body, sizeof body);
        uint8_t type = 0;
        if (a->report.session == FIPS_MMP_SessionSender) {
            fips_mmp_sender_report sr;
            if (!fips_mmp_sender_build_report(&x->mmp.roles.sender, now_ms, &sr)) continue;
            fips_mmp_session_sender_report ssr = fips_mmp_session_sender_report_from_sender_report(&sr);
            if (fips_mmp_session_sender_report_encode(&ssr, &w, NULL) != FIPS_OK) continue;
            type = FIPS_SESSION_SenderReport;
        } else if (a->report.session == FIPS_MMP_SessionReceiver) {
            fips_mmp_receiver_report rr;
            if (!fips_mmp_receiver_build_report(&x->mmp.roles.receiver, now_ms, &rr)) continue;
            fips_mmp_session_receiver_report srr = fips_mmp_session_receiver_report_from_receiver_report(&rr);
            if (fips_mmp_session_receiver_report_encode(&srr, &w, NULL) != FIPS_OK) continue;
            type = FIPS_SESSION_ReceiverReport;
        } else {
            uint16_t mtu;
            if (!fips_mmp_path_mtu_build_notification(&x->mmp.path_mtu, now_ms, &mtu)) continue;
            fips_mmp_path_mtu_notification notification = {mtu};
            if (fips_mmp_path_mtu_notification_encode(&notification, &w, NULL) != FIPS_OK) continue;
            type = FIPS_SESSION_PathMtuNotification;
        }
        (void)hs_session_send_message(h, entry, x, type, body, fips_writer_len(&w));
    }
}
