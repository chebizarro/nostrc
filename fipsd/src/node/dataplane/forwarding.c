#include "node/handlers/internal.h"

typedef struct route_view {
    const fipsd_dp_snapshot *s;
    fips_tree_coordinate *scratch;
    uint64_t now;
} route_view;
static const fips_tree_coordinate *cached(void *arg, const fips_node_addr *dest, fips_instant now)
{
    (void)now; route_view *v = arg;
    for (size_t i = 0; i < v->s->cache_count; ++i) {
        const fipsd_dp_cached *c = &v->s->cache[i];
        if (memcmp(c->addr.bytes, dest->bytes, 16) || c->expires <= v->now) continue;
        v->scratch->len = c->coords.len;
        for (size_t j = 0; j < c->coords.len; ++j) {
            memcpy(v->scratch->entries[j].node_addr.bytes, c->coords.entries[j].node_addr, 16);
            v->scratch->entries[j].sequence = c->coords.entries[j].sequence;
            v->scratch->entries[j].timestamp = c->coords.entries[j].timestamp;
        }
        return v->scratch;
    }
    return NULL;
}
fips_status fipsd_dp_forward(const fipsd_dp_snapshot *s, fips_router *router,
    const fips_session_datagram *dg, const fips_node_addr *exclude, bool ce, fips_instant now,
    fips_tree_coordinate *scratch, fips_routing_peer *peers, fips_writer *wire, fips_route_outcome *out)
{
    route_view v = {.s = s, .scratch = scratch};
    /* Cache timestamps are relative to the publisher's origin. */
    v.now = s->origin.ops ? hs_elapsed_ms(now, s->origin) : 0;
    size_t n = 0; const fipsd_dp_peer *direct = NULL;
    for (size_t i = 0; i < s->peer_count; ++i) {
        const fipsd_dp_peer *p = &s->peers[i];
        if (exclude && !memcmp(exclude->bytes, p->addr.bytes, 16)) continue;
        peers[n++] = (fips_routing_peer){p->addr, p->coords.ptr, p->filter.ptr, p->cost};
        if (!memcmp(dg->dest_addr.bytes, p->addr.bytes, 16)) direct = p;
    }
    fips_routing_view rv = {.peers = peers, .peer_count = n, .ctx = &v, .cached_coords = cached};
    fips_next_hop hop = {0}; bool found = false;
    if (direct) { hop = (fips_next_hop){direct->addr, direct->mtu}; found = true; }
    else if (memcmp(dg->dest_addr.bytes, s->me.bytes, 16) && dg->ttl > 1) {
        const fips_tree_coordinate *dest = cached(&v, &dg->dest_addr, now);
        const fips_tree_coordinate *mine = s->my_coords.ptr;
        fips_node_addr chosen;
        if (dest && mine) {
            found = fips_select_best_candidate(&rv, &dg->dest_addr, dest, mine, &chosen);
            if (!found) {
                /* Greedy tree fallback on a bloom miss; no reverse bounce. */
                size_t distance = fips_tree_coordinate_distance_to(mine, dest);
                for (size_t i = 0; i < n; ++i) if (peers[i].coords) {
                    size_t next = fips_tree_coordinate_distance_to(peers[i].coords, dest);
                    if (next < distance) { distance = next; chosen = peers[i].addr; found = true; }
                }
            }
        }
        if (found) for (size_t i = 0; i < s->peer_count; ++i)
            if (!memcmp(s->peers[i].addr.bytes, chosen.bytes, 16)) { hop = (fips_next_hop){chosen, s->peers[i].mtu}; break; }
    }
    return fips_router_route(router, dg, &s->me, ce, found ? &hop : NULL, &rv, wire, out, NULL);
}
bool fipsd_dp_route(handlers *h, const fips_session_datagram *dg, const fips_node_addr *exclude, bool ce)
{
    if (!fipsd_dp_publish(h)) return false;
    fipsd_dataplane *d = h->dataplane;
    GoRefPtr ref = d->current; go_refptr_retain(&ref); /* atomic retain, no table lock */
    const fipsd_dp_snapshot *s = ref.ptr;
    fips_writer w; fips_writer_init(&w, h->route_buf, sizeof h->route_buf);
    fips_route_outcome out;
    fips_session_datagram clamped = *dg;
    uint16_t mtu = fipsd_dp_mtu_get(d, &dg->dest_addr);
    if (mtu && mtu < clamped.path_mtu) clamped.path_mtu = mtu;
    fips_status status = fipsd_dp_forward(s, &h->router, &clamped, exclude, ce, hs_now(h),
        d->coord_scratch, d->route_peers, &w, &out);
    go_refptr_release(&ref);
    if (status != FIPS_OK) { fipsd_dp_reject_frame(h, FIPSD_DP_DECODE); return false; }
    size_t metric_bytes = fips_session_datagram_encoded_len(dg) - 1;
    bool originated = !memcmp(dg->src_addr.bytes, h->me.bytes, 16);
    switch (out.kind) {
    case FIPS_ROUTE_Forward: {
        peer_rec *next = hs_rec_find(h, &out.next_hop);
        if (!next) return false;
        const fipsd_peer_info *info = fipsd_active_peer_info(next->peer);
        fipsd_transport *t = hs_transport(h, info->transport);
        size_t len = fips_writer_len(&w);
        uint16_t link_mtu = fipsd_dp_link_mtu(h, next, info->transport, &info->addr);
        uint16_t clamp = fipsd_dp_mtu_get(d, &dg->dest_addr);
        if (clamp && clamp < link_mtu) link_mtu = clamp;
        if (!t || len + FIPS_FRAMING_FMP_ESTABLISHED_HEADER_SIZE + (FIPS_FRAMING_FMP_INNER_HEADER_SIZE - 1) + FIPS_NOISE_TAG_SIZE > link_mtu) {
            fipsd_dp_reject_frame(h, FIPSD_DP_MTU);
            METRIC(h, forwarding, drop_mtu_exceeded_packets); METRIC_ADD(h, forwarding, drop_mtu_exceeded_bytes, metric_bytes);
            if (exclude) fipsd_dp_route_error(h, dg, exclude, link_mtu);
            return false;
        }
        bool ok = fipsd_dp_link_send_ce(h, next, info->transport, &info->addr, h->route_buf, len, out.outgoing_ce);
        if (!ok) { fipsd_dp_reject_frame(h, FIPSD_DP_SEND); METRIC(h, forwarding, drop_send_error_packets); METRIC_ADD(h, forwarding, drop_send_error_bytes, metric_bytes); return false; }
        if (!memcmp(dg->dest_addr.bytes, out.next_hop.bytes, 16)) ++h->counters.fwd_route_direct_peer;
        else ++h->counters.fwd_route_tree_up;
        if (originated) { ++h->counters.fwd_originated_packets; h->counters.fwd_originated_bytes += metric_bytes; }
        else {
            ++h->counters.fwd_forwarded_packets; h->counters.fwd_forwarded_bytes += metric_bytes;
            fips_route_class cls = fips_classify_forward(&dg->dest_addr, &out.next_hop, &h->me,
                fips_tree_state_my_coords(h->tree), hs_cache_coords(h, &dg->dest_addr, hs_now(h)), hs_tree_peer_coords(h, &out.next_hop));
            switch (cls) {
            case FIPS_ROUTE_TreeUp: METRIC(h, forwarding, route_tree_up); break;
            case FIPS_ROUTE_TreeDown: METRIC(h, forwarding, route_tree_down); break;
            case FIPS_ROUTE_TreeDownCross: METRIC(h, forwarding, route_tree_down_cross); break;
            case FIPS_ROUTE_CrosslinkDescend: METRIC(h, forwarding, route_crosslink_descend); break;
            case FIPS_ROUTE_CrosslinkAscend: METRIC(h, forwarding, route_crosslink_ascend); break;
            case FIPS_ROUTE_DirectPeer: METRIC(h, forwarding, route_direct_peer); break;
            }
            if (out.outgoing_ce) METRIC(h, congestion, ce_forwarded);
        }
        return true;
    }
    case FIPS_ROUTE_DeliverLocal: return false; /* endpoint path precedes route */
    case FIPS_ROUTE_Drop: fipsd_dp_reject_frame(h, FIPSD_DP_TTL); METRIC_ADD(h, forwarding, ttl_exhausted_bytes, metric_bytes); return false;
    case FIPS_ROUTE_NoRoute:
        fipsd_dp_reject_frame(h, FIPSD_DP_NO_ROUTE);
        h->counters.fwd_drop_no_route_bytes += metric_bytes;
        if (exclude) fipsd_dp_route_error(h, dg, exclude, 0);
        return false;
    }
    return false;
}
