#include "node/handlers/internal.h"

bool fipsd_dp_peer_rebind(handlers *h, peer_rec *rec)
{
    const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
    uint16_t mtu = fipsd_dp_link_mtu(h, rec, info->transport, &info->addr);
    /* Discard the old path's measurement, even on a same-transport address
     * switch. The path owner closes its old connected socket. Old crypto
     * jobs retain keys/bytes only: dispatch.c resolves the active pair afresh. */
    fipsd_dp_mtu_release(h->dataplane, &rec->addr);
    fipsd_dp_mtu_set(h->dataplane, &rec->addr, mtu);
    h->dataplane->needs_publish = true;
    hs_mark_dirty(h);
    if (!fipsd_dp_publish(h)) return false;
    fipsd_transport *transport = hs_transport(h, info->transport);
    if (transport && transport->ops->authenticated) transport->ops->authenticated(transport, &info->addr, info->direction);
    fipsd_dp_tighten_via(h, &rec->addr, mtu);
    hs_host_link_mtu_changed(h, mtu);
    return true;
}

void fipsd_dp_tighten_via(handlers *h, const fips_node_addr *peer, uint16_t mtu)
{
    const fipsd_dp_snapshot *s = h->dataplane->current.ptr;
    for (size_t i = 0; i < h->sessions.capacity; ++i) {
        fips_session_entry *entry = &h->session_entries[i];
        session_ext *x = &h->session_ext[i];
        if (!entry->occupied || !x->used || !x->mmp_live) continue;
        /* Ask the production routing decision, not the last inbound carrier:
         * upstream tighten_via applies only to sessions whose NEXT hop moved. */
        fips_session_datagram dg;
        fips_session_datagram_init(&dg, h->me, entry->peer, NULL, 0);
        fips_writer w; fips_writer_init(&w, h->route_buf, sizeof h->route_buf);
        fips_route_outcome out;
        if (fipsd_dp_forward(s, &h->router, &dg, NULL, false, hs_now(h),
                h->dataplane->coord_scratch, h->dataplane->route_peers, &w, &out) != FIPS_OK ||
            out.kind != FIPS_ROUTE_Forward || memcmp(out.next_hop.bytes, peer->bytes, 16)) continue;
        fips_mmp_path_mtu_seed_source_mtu(&x->mmp.path_mtu, mtu);
        uint16_t clamp = fipsd_dp_mtu_get(h->dataplane, &entry->peer);
        if (!clamp || x->mmp.path_mtu.current_mtu < clamp)
            fipsd_dp_mtu_set(h->dataplane, &entry->peer, x->mmp.path_mtu.current_mtu);
    }
}

void fipsd_dp_link_deliver(handlers *h, peer_rec *rec, fipsd_dp_packet *p, uint8_t *plain, size_t len)
{
    uint32_t timestamp; const uint8_t *message; size_t n;
    if (!fips_fmp_strip_inner_header(plain, len, &timestamp, &message, &n)) return;
    fipsd_peer_event e = {.kind = FIPSD_PEER_HEARD, .authenticated_bytes = p->wire_size, .wall_ms = hs_wall_ms(h)};
    fipsd_peer_actions a;
    fipsd_active_peer_step(rec->peer, hs_now(h), &e, &a);
    rec->last_heard = hs_now(h); rec->heard = true;
    ++rec->frames_in; rec->bytes_in += p->wire_size; ++h->counters.frames_delivered;
    if (a.count) (void)hs_peer_effects(h, rec->peer, &a);
    if (!rec->used) return;
    if (p->ce) METRIC(h, congestion, ce_received);
    hs_mmp_frame_received(h, rec, p->job.counter, timestamp, p->wire_size, p->ce);
    fipsd_transport *t = hs_transport(h, p->transport);
    if (t && t->ops->authenticated) {
        const fipsd_peer_info *info = fipsd_active_peer_info(rec->peer);
        if (fips_transport_id_equal(info->transport, p->transport) && fips_transport_addr_equal(&info->addr, &p->endpoint))
            t->ops->authenticated(t, &p->endpoint, info->direction);
    }
    if (n && t) hs_path_account(h, rec, t, &p->endpoint, p->wire_size, message[0]);
    if (n > sizeof h->msg_buf) return;
    memcpy(h->msg_buf, message, n);
    /* SessionDatagram carries incoming CE to forwarding; control stays on the
     * dispatcher behind the same validated link-message gate. */
    if (n && message[0] == FIPS_LINK_SessionDatagram) {
        /* Transit forwarding bypasses session/control dispatch. */
        fips_reader rd; fips_reader_init(&rd, message + 1, n - 1);
        fips_session_datagram dg;
        if (fips_session_datagram_decode(&rd, &dg, NULL) == FIPS_OK && memcmp(dg.dest_addr.bytes, h->me.bytes, 16)) {
            fipsd_dp_warm(h, rec, &dg);
            ++h->counters.fwd_received_packets; h->counters.fwd_received_bytes += n - 1;
            (void)fipsd_dp_route(h, &dg, &rec->addr, p->ce); return;
        }
    }
    if (n && message[0] != FIPS_LINK_SessionDatagram) h->dataplane->needs_publish = true;
    /* Path messages are attributed to the frame's ingress transport (t may be
     * NULL if the transport vanished while the open job was in flight). */
    hs_link_message(h, rec, t, &p->endpoint, timestamp, h->msg_buf, n);
}
