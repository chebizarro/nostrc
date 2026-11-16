/* Native API / host-link seams (upstream handlers/native.rs, host_link.rs).
 * An authenticated inbound datagram is handed to the native datagram API's
 * sink when one is installed (fipsd/src/native, nostrc-9nxwk.4) and surfaced
 * as a FIPSD_NOTICE_DATAGRAM otherwise; the outbound path is
 * fipsd_node_handlers_send. Ports in the protocol and standard-service tiers
 * (0-1023) never reach the API: no client may hold them, so they keep the
 * notice channel for the IPv6 adapter and other host consumers. */
#include "node/handlers/internal.h"
#include "native/protocol.h"
#include <fips/identity/local.h>
#include <stdatomic.h>

/* The peer's x-only key, where the dispatcher can name it: our own identity,
 * a linked peer's authenticated identity, or a learned identity. Mirrors the
 * session row's npub resolution in provider.c. */
static bool peer_pubkey(handlers *h, const fips_node_addr *a, fips_xonly_pubkey *out)
{
    if (!memcmp(a->bytes, h->me.bytes, 16)) {
        if (!h->identity) return false;
        *out = fips_identity_pubkey(h->identity);
        return true;
    }
    fipsd_active_peer *p = h->peers ? fipsd_peer_table_find(h->peers->table, a) : NULL;
    if (p) { *out = fipsd_active_peer_info(p)->identity.pubkey; return true; }
    for (size_t i = 0; i < h->identities.count; ++i)
        if (!memcmp(h->identities.entries[i].peer.node_addr.bytes, a->bytes, 16)) {
            *out = h->identities.entries[i].peer.pubkey;
            return true;
        }
    return false;
}
void hs_native_deliver(handlers *h, const fips_node_addr *src, uint16_t src_port, uint16_t dst_port, const uint8_t *payload, size_t len)
{
    ++h->counters.native_delivered;
    fipsd_node_notice n = {.kind = FIPSD_NOTICE_DATAGRAM, .peer = *src, .src_port = src_port, .dst_port = dst_port};
    n.has_pubkey = peer_pubkey(h, src, &n.pubkey);
    GoChannel *sink = atomic_load_explicit(&h->native_sink, memory_order_acquire);
    if (sink && dst_port > FIPSD_NATIVE_PORT_STANDARD_MAX) {
        /* The sink's owner counts received/drop per dispatch outcome. */
        fipsd_node_notice *m = malloc(sizeof(*m) + len);
        if (!m) { METRIC(h, native, drop_flow_queue_full); return; }
        *m = n; m->len = len;
        if (len) memcpy(m->payload, payload, len);
        if (go_channel_try_send(sink, m) != 0) {
            fipsd_node_notice_free(m);
            METRIC(h, native, drop_flow_queue_full);
        }
        return;
    }
    METRIC(h, native, received_datagrams);
    METRIC_ADD(h, native, received_bytes, len);
    hs_notice_full(h, &n, payload, len);
}
void fipsd_node_handlers_set_native_sink(fipsd_node_handlers *h, GoChannel *sink)
{
    atomic_store_explicit(&h->native_sink, sink, memory_order_release);
}
struct fipsd_metrics *fipsd_node_handlers_metrics(fipsd_node_handlers *h)
{
    return &h->metrics;
}
void hs_host_link_mtu_changed(handlers *h, uint16_t mtu)
{
    /* host_link.rs publish_mtu: the adapter owner receives the payload limit. */
    (void)h;
    FIPSD_DEBUG(HMOD, "node payload limit now %u", (unsigned)fips_framing_payload_limit(mtu));
}
