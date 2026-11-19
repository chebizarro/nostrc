#include "node/handlers/internal.h"
static bool generation_matches(const fips_rekey *m, const fipsd_dp_packet *p)
{
    const fipsd_dp_snapshot *s = p->snapshot.ptr;
    const fipsd_dp_epoch *old = NULL;
    for (size_t i = 0; i < s->epoch_count; ++i) {
        const fipsd_dp_epoch *e = &s->epochs[i];
        if (e->tag == p->tag && e->session == p->session && !memcmp(e->peer.bytes, p->peer.bytes, 16)) { old = e; break; }
    }
    if (!old) return false;
    for (int slot = FIPS_REKEY_Current; slot <= FIPS_REKEY_Previous; ++slot) {
        uint64_t tag;
        if (!fips_rekey_tag(m, (fips_rekey_slot)slot, &tag) || tag != p->tag) continue;
        const fips_noise_session *keys = fips_rekey_keys(m, (fips_rekey_slot)slot);
        fips_cipher_state clone = {0};
        bool ok = keys && fips_noise_session_recv_cipher_clone(keys, &clone) == FIPS_NOISE_OK &&
            sodium_memcmp(clone.key.bytes, old->recv.key.bytes, sizeof clone.key.bytes) == 0;
        fips_cipher_state_destroy(&clone);
        return ok;
    }
    return false;
}
bool fipsd_dp_result(handlers *h, const fipsd_crypto_job *j)
{
    FIPSD_PERF_BEGIN(perf_start);
    if (!j->recycle || !h->dataplane) FIPSD_PERF_RETURN(perf_start, LINK_DISPATCH, true);
    fipsd_dp_packet *p = j->owner;
    if (p->restart) FIPSD_PERF_RETURN(perf_start, LINK_DISPATCH, hs_restart_result(h, j));
    peer_rec *rec = hs_rec_find(h, &p->peer);
    fips_session_entry *entry = p->session ? fips_session_table_by_peer(&h->sessions, p->peer) : NULL;
    fips_rekey *m = p->session ? (entry ? &entry->machine : NULL) :
        (rec ? fipsd_active_peer_link_machine(rec->peer) : NULL);
    if (!h->running || (!p->farewell && (!m || !generation_matches(m, p)))) {
        ++h->dataplane->stats.stale; FIPSD_PERF_RETURN(perf_start, LINK_DISPATCH, true);
    }
    if (j->result != FIPS_NOISE_OK) {
        if (j->operation == FIPSD_CRYPTO_OPEN && p->session && fipsd_dp_open_retry(h, p)) FIPSD_PERF_RETURN(perf_start, LINK_DISPATCH, true);
        ++h->dataplane->stats.authentication_failed;
        fipsd_dp_reject_frame(h, j->operation == FIPSD_CRYPTO_OPEN ? FIPSD_DP_AUTH : FIPSD_DP_SEND);
        FIPSD_PERF_RETURN(perf_start, LINK_DISPATCH, true);
    }
    size_t len = p->body + j->output_size;
    if (j->operation == FIPSD_CRYPTO_SEAL) {
        bool ok;
        if (p->session) {
            fips_session_datagram dg; fips_session_datagram_init(&dg, h->me, p->peer, p->output, len);
            ok = fipsd_dp_route(h, &dg, NULL, false);
            session_ext *x = hs_session_ext(h, entry);
            if (ok && x && x->mmp_live) fips_mmp_sender_record_sent(&x->mmp.roles.sender, j->counter, p->timestamp, len);
            if (ok && !p->prompt) {
                fips_rekey_env env; hs_env(h, &env, 0); fips_rekey_outputs_clear(&h->session_out);
                if (fips_rekey_step(m, hs_now(h), (fips_rekey_input){.kind = FIPS_REKEY_IN_DataSent}, &env, &h->session_out) == FIPS_OK)
                    (void)hs_session_execute(h, entry, NULL, 0);
            }
        } else {
            fipsd_transport *t = hs_transport(h, p->transport);
            /* Workers only seal bytes. Resolve ordinary traffic on the
             * dispatcher at completion, so a job queued before a switch cannot
             * send via its captured old pair. Path control is deliberately
             * addressed, but a released path must not send queued probes. */
            if (rec && !p->farewell) {
                if (p->path_message) {
                    if (rec->path_live && fips_path_set_find(&rec->path->set, p->transport, &p->endpoint) == SIZE_MAX) {
                        ++h->dataplane->stats.stale; FIPSD_PERF_RETURN(perf_start, LINK_DISPATCH, true);
                    }
                } else {
                    const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
                    p->transport = info->transport; p->endpoint = info->addr;
                }
                t = hs_transport(h, p->transport);
                uint16_t mtu = p->path_message && t ? t->ops->mtu(t, &p->endpoint) :
                    fipsd_dp_link_mtu(h, rec, p->transport, &p->endpoint);
                if (len > mtu) {
                    fipsd_dp_reject_frame(h, FIPSD_DP_MTU); FIPSD_PERF_RETURN(perf_start, LINK_DISPATCH, true);
                }
            }
            ok = t && fipsd_transport_send_lane(t, p->path_message ? FIPSD_TRANSPORT_LANE_CONTROL :
                FIPSD_TRANSPORT_LANE_DATA, &p->endpoint, p->output, len) == 0;
            if (ok) {
                ++h->counters.frames_sent;
                if (rec && m && generation_matches(m, p)) {
                    ++rec->frames_out; rec->bytes_out += len;
                    fips_link_stats_record_sent(&fipsd_active_peer_data(rec->peer)->link_stats, len);
                    hs_mmp_frame_sent(h, rec, j->counter, p->timestamp, len);
                    if (!p->prompt) {
                        link_exec_result r = {0};
                        (void)hs_link_step(h, rec, (fips_rekey_input){.kind = FIPS_REKEY_IN_DataSent}, NULL, NULL, 0, &r);
                        if (r.teardown) hs_remove_peer(h, rec, FIPS_REKEY_CLOSE_Admin, false, FIPS_FMP_ResourceExhaustion);
                    }
                }
            }
        }
        if (!ok) { fipsd_dp_reject_frame(h, FIPSD_DP_SEND); ++h->counters.send_failed; }
        FIPSD_PERF_RETURN(perf_start, LINK_DISPATCH, true);
    }
    /* AEAD succeeded, but the tag and replay window may have changed since
     * dispatch. Only this dispatcher-side machine input can mark the window. */
    memcpy(p->output, p->ad, p->body);
    fips_rekey_input in = {.kind = FIPS_REKEY_IN_AuthenticatedFrame, .tag = p->tag,
        .buf = p->output, .bytes = p->output, .len = len,
        .hint = {j->counter, p->k, (uint32_t)j->token, p->body}};
    if (p->session) {
        peer_rec *carrier = hs_rec_find(h, &p->carrier);
        if (!carrier) { ++h->dataplane->stats.stale; FIPSD_PERF_RETURN(perf_start, LINK_DISPATCH, true); }
        session_ext *x = hs_session_ext(h, entry); if (x) x->carrier = carrier->addr;
        fips_rekey_env env; hs_env(h, &env, 0); fips_rekey_outputs_clear(&h->session_out);
        if (fips_rekey_step(m, hs_now(h), in, &env, &h->session_out) == FIPS_OK) {
            (void)hs_session_execute(h, entry, p->output, len);
        }
    } else {
        link_exec_result result = {.counter = j->counter};
        (void)hs_link_step(h, rec, in, &p->endpoint, p->output, len, &result);
        if (result.teardown) hs_remove_peer(h, rec, FIPS_REKEY_CLOSE_Admin, false, FIPS_FMP_ProtocolError);
        else if (result.delivered && result.begin <= result.end && result.end <= len) {
            fipsd_dp_link_deliver(h, rec, p, p->output + result.begin, result.end - result.begin);
        }
    }
    /* No publish per authenticated frame: state transitions (session up/down,
     * link teardown, tree/bloom/lookup/path/rekey handlers) mark the snapshot
     * dirty themselves; counters and link statistics are published from the
     * tick, as upstream does. A per-packet snapshot build was the dataplane's
     * dominant cost (nostrc-pxwaj). */
    FIPSD_PERF_RETURN(perf_start, LINK_DISPATCH, true);
}
