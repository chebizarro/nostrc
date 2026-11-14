#include <fips/proto/routing/core.h>
#include <string.h>

bool fips_select_best_candidate(const fips_routing_view *rv, const fips_node_addr *dest,
    const fips_tree_coordinate *dest_coords, const fips_tree_coordinate *my_coords, fips_node_addr *out)
{
    size_t my_distance = fips_tree_coordinate_distance_to(my_coords, dest_coords);
    const fips_routing_peer *best = NULL;
    size_t best_dist = SIZE_MAX;
    for (size_t i = 0; i < rv->peer_count; ++i) {
        const fips_routing_peer *p = &rv->peers[i];
        if (!p->filter || !fips_bloom_contains(p->filter, dest)) continue;
        size_t dist = p->coords ? fips_tree_coordinate_distance_to(p->coords, dest_coords) : SIZE_MAX;
        if (dist >= my_distance) continue;
        if (!best || p->link_cost < best->link_cost ||
            (p->link_cost == best->link_cost && dist < best_dist) ||
            (p->link_cost == best->link_cost && dist == best_dist &&
             memcmp(p->addr.bytes, best->addr.bytes, 16) < 0)) {
            best = p;
            best_dist = dist;
        }
    }
    if (!best) return false;
    *out = best->addr;
    return true;
}
fips_route_class fips_classify_forward(const fips_node_addr *dest, const fips_node_addr *chosen,
    const fips_node_addr *my_addr, const fips_tree_coordinate *my_coords,
    const fips_tree_coordinate *dest_coords, const fips_tree_coordinate *peer_coords)
{
    if (!memcmp(dest->bytes, chosen->bytes, 16)) return FIPS_ROUTE_DirectPeer;
    if (fips_tree_coordinate_has_ancestor(my_coords, chosen)) return FIPS_ROUTE_TreeUp;
    bool subtree = dest_coords && fips_tree_coordinate_has_ancestor(dest_coords, chosen);
    if (peer_coords && fips_tree_coordinate_has_ancestor(peer_coords, my_addr))
        return subtree ? FIPS_ROUTE_TreeDown : FIPS_ROUTE_TreeDownCross;
    return subtree ? FIPS_ROUTE_CrosslinkDescend : FIPS_ROUTE_CrosslinkAscend;
}
const char *fips_route_class_name(fips_route_class c)
{
    switch (c) {
    case FIPS_ROUTE_TreeUp: return "tree_up";
    case FIPS_ROUTE_TreeDown: return "tree_down";
    case FIPS_ROUTE_TreeDownCross: return "tree_down_cross";
    case FIPS_ROUTE_CrosslinkDescend: return "crosslink_descend";
    case FIPS_ROUTE_CrosslinkAscend: return "crosslink_ascend";
    case FIPS_ROUTE_DirectPeer: return "direct_peer";
    default: return "Unknown";
    }
}
fips_status fips_router_route(fips_router *router, const fips_session_datagram *dg,
    const fips_node_addr *my_addr, bool incoming_ce, const fips_next_hop *nh,
    const fips_routing_view *rv, fips_writer *bytes, fips_route_outcome *out, fips_proto_error *e)
{
    (void)router; /* Routing is pure; only error synthesis uses state. */
    fips_route_outcome result = {.kind = FIPS_ROUTE_DeliverLocal};
    if (!memcmp(dg->dest_addr.bytes, my_addr->bytes, 16)) { *out = result; return FIPS_OK; }
    uint8_t ttl;
    if (!fips_ttl_after_hop(dg->ttl, &ttl)) {
        result.kind = FIPS_ROUTE_Drop;
        result.reason = FIPS_DROP_TtlExhausted;
    } else if (!nh) result.kind = FIPS_ROUTE_NoRoute;
    else {
        fips_session_datagram forwarded = *dg;
        forwarded.ttl = ttl;
        if (nh->link_mtu < forwarded.path_mtu) forwarded.path_mtu = nh->link_mtu;
        if (fips_session_datagram_encode(&forwarded, bytes, e) != FIPS_OK) return FIPS_ERR_RANGE;
        result.kind = FIPS_ROUTE_Forward;
        result.next_hop = nh->addr;
        result.outgoing_ce = incoming_ce || (rv->is_congested && rv->is_congested(rv->ctx, &nh->addr));
    }
    *out = result;
    return FIPS_OK;
}
static fips_status synth(fips_router *r, const fips_node_addr *dest, const fips_node_addr *toward,
    const fips_node_addr *my_addr, const fips_routing_view *rv, bool mtu_error, uint16_t mtu,
    fips_instant now, uint8_t ttl, fips_writer *bytes, fips_error_synth *out, fips_proto_error *e)
{
    /* Check on a bounded copy so a C capacity refusal cannot spend the gate.
     * No map growth, allocation or coordinate-sized stack temporaries. */
    fips_routing_error_rate_limiter limiter = r->error_limiter;
    fips_error_synth result = {.verdict = fips_routing_error_rate_limiter_check(&limiter, dest, now)};
    if (result.verdict == FIPS_LIMIT_Suppress) { *out = result; return FIPS_OK; }
    uint8_t payload[40];
    fips_writer p;
    fips_writer_init(&p, payload, sizeof(payload));
    if (mtu_error) {
        fips_mtu_exceeded signal = fips_mtu_exceeded_new(*dest, *my_addr, mtu);
        (void)fips_mtu_exceeded_encode(&signal, &p, NULL);
    } else if (rv->cached_coords && rv->cached_coords(rv->ctx, dest, now)) {
        /* Never disclose cached coordinates to an unbound src_addr. */
        fips_path_broken signal = fips_path_broken_new(*dest, *my_addr);
        (void)fips_path_broken_encode(&signal, &p, NULL);
    } else {
        fips_coords_required signal = fips_coords_required_new(*dest, *my_addr);
        (void)fips_coords_required_encode(&signal, &p, NULL);
    }
    fips_session_datagram dg;
    fips_session_datagram_init(&dg, *my_addr, *toward, payload, p.pos);
    dg.ttl = ttl;
    if (fips_session_datagram_encode(&dg, bytes, e) != FIPS_OK) return FIPS_ERR_RANGE;
    r->error_limiter = limiter;
    result.has_action = true;
    result.action = (fips_route_action){FIPS_ROUTE_SendError, *toward};
    *out = result;
    return FIPS_OK;
}
fips_status fips_router_synth_routing_error(fips_router *r, const fips_node_addr *dest,
    const fips_node_addr *toward, const fips_node_addr *my_addr, const fips_routing_view *rv,
    fips_instant now, uint8_t ttl, fips_writer *bytes, fips_error_synth *out, fips_proto_error *e)
{ return synth(r, dest, toward, my_addr, rv, false, 0, now, ttl, bytes, out, e); }
fips_status fips_router_synth_mtu_exceeded(fips_router *r, const fips_node_addr *dest,
    const fips_node_addr *toward, const fips_node_addr *my_addr, uint16_t mtu,
    fips_instant now, uint8_t ttl, fips_writer *bytes, fips_error_synth *out, fips_proto_error *e)
{ return synth(r, dest, toward, my_addr, NULL, true, mtu, now, ttl, bytes, out, e); }
