#include "node/handlers/internal.h"
/* Typed equivalents of upstream reject.rs's forwarding and ingress reasons.
 * Malformed, unknown-index, AEAD and TTL rejects are silent. Only no-route and
 * MTU synthesize signals, sharing the core's bounded per-destination limiter. */
void fipsd_dp_reject_frame(handlers *h, fipsd_dp_reject why)
{
    ++h->dataplane->stats.rejects[why];
    switch (why) {
    case FIPSD_DP_DECODE: ++h->counters.fwd_decode_error_packets; ++h->counters.frames_dropped; break;
    case FIPSD_DP_TTL: ++h->counters.fwd_ttl_exhausted_packets; break;
    case FIPSD_DP_NO_ROUTE: ++h->counters.fwd_drop_no_route_packets; break;
    case FIPSD_DP_AUTH: ++h->counters.decrypt_failed; break;
    case FIPSD_DP_REPLAY: ++h->counters.replay; break;
    default: ++h->counters.frames_dropped; break;
    }
}
void fipsd_dp_route_error(handlers *h, const fips_session_datagram *dg, const fips_node_addr *from, uint16_t mtu)
{
    peer_rec *rec = hs_rec_find(h, from);
    if (!rec || !memcmp(dg->src_addr.bytes, h->me.bytes, 16)) return;
    uint8_t bytes[FIPSD_DP_WIRE]; fips_writer w; fips_writer_init(&w, bytes, sizeof bytes);
    fips_error_synth out;
    /* The source is reached along the authenticated arrival link, not by
     * recursively routing a routing-error. Cache lookup selects PathBroken vs
     * CoordsRequired exactly in the core's synthesis function. */
    fips_routing_view rv = {.ctx = h, .cached_coords = hs_cache_coords};
    fips_status status = mtu ? fips_router_synth_mtu_exceeded(&h->router, &dg->dest_addr,
        &dg->src_addr, &h->me, mtu, hs_now(h), 64, &w, &out, NULL) :
        fips_router_synth_routing_error(&h->router, &dg->dest_addr, &dg->src_addr, &h->me,
            &rv, hs_now(h), 64, &w, &out, NULL);
    if (status != FIPS_OK) return;
    if (!out.has_action) { ++h->dataplane->stats.errors_suppressed; return; }
    if (hs_link_send(h, rec, bytes, fips_writer_len(&w))) ++h->dataplane->stats.errors_sent;
}
