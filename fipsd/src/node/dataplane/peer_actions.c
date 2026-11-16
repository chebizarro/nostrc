#include "node/handlers/internal.h"
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
    hs_mmp_frame_received(h, rec, p->job.counter, timestamp, p->wire_size, p->ce);
    fipsd_transport *t = hs_transport(h, p->transport);
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
            ++h->counters.fwd_received_packets; h->counters.fwd_received_bytes += n - 1;
            (void)fipsd_dp_route(h, &dg, &rec->addr, p->ce); return;
        }
    }
    if (n && message[0] != FIPS_LINK_SessionDatagram) h->dataplane->needs_publish = true;
    /* Path messages are attributed to the frame's ingress transport (t may be
     * NULL if the transport vanished while the open job was in flight). */
    hs_link_message(h, rec, t, &p->endpoint, timestamp, h->msg_buf, n);
}
