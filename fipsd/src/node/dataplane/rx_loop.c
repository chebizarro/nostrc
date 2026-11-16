#include "node/handlers/internal.h"
/* Called by the transport-event owner, with no lifetime borrowed across a job.
 * Handshake/control frames are handed to dispatcher handlers; established
 * envelopes go straight to bounded authentication jobs. */
bool fipsd_dp_rx(handlers *h, const fipsd_transport_event *e)
{
    fipsd_transport *t = hs_transport(h, e->id);
    if (!t || !h->running) return true;
    ++h->counters.frames_received;
    if (e->size > FIPSD_DP_WIRE) { fipsd_dp_reject_frame(h, FIPSD_DP_DECODE); return true; }
    fips_fmp_conn_action action; fips_fmp_action_list list = {&action, 0, 1};
    if (fips_fmp_receive((fips_link_id){0}, e->bytes, e->size, &list) != FIPS_OK || action.kind == FIPS_FMP_DropFrame) {
        fipsd_dp_reject_frame(h, FIPSD_DP_DECODE); return true;
    }
    if (action.kind == FIPS_FMP_ReceiveMsg1 || action.kind == FIPS_FMP_ReceiveMsg2) {
        h->dataplane->needs_publish = true;
        memcpy(h->in_wire, e->bytes, e->size);
        (void)hs_handshake_frame(h, t, &e->address, h->in_wire, e->size, action.kind == FIPS_FMP_ReceiveMsg1 ? 1 : 2);
        return true;
    }
    fips_fmp_encrypted_header header;
    if (!fips_fmp_encrypted_header_parse(e->bytes, e->size, &header)) return true;
    if (hs_restart_frame(h, t, &e->address, e->bytes, e->size, &header)) return true;
    peer_rec *rec = hs_index_lookup(h, e->id.value, header.receiver_idx.value);
    if (!rec) { fipsd_dp_reject_frame(h, FIPSD_DP_UNKNOWN_INDEX); return true; }
    (void)fipsd_dp_open(h, rec, NULL, e->id, &e->address, e->bytes, e->size);
    return true;
}
