/* Native API / host-link seams (upstream handlers/native.rs, host_link.rs).
 * Tier B4 attaches the datagram API and the IPv6 adapter here; until then an
 * authenticated inbound datagram is surfaced as a FIPSD_NOTICE_DATAGRAM and the
 * outbound path is fipsd_node_handlers_send. No flow table or TUN is faked. */
#include "node/handlers/internal.h"

void hs_native_deliver(handlers *h, const fips_node_addr *src, uint16_t src_port, uint16_t dst_port, const uint8_t *payload, size_t len)
{
    ++h->counters.native_delivered;
    METRIC_ADD(h, native, received_bytes, len);
    fipsd_node_notice n = {.kind = FIPSD_NOTICE_DATAGRAM, .peer = *src, .src_port = src_port, .dst_port = dst_port};
    hs_notice_full(h, &n, payload, len);
}
void hs_host_link_mtu_changed(handlers *h, uint16_t mtu)
{
    /* host_link.rs publish_mtu: the adapter owner receives the payload limit. */
    (void)h;
    FIPSD_DEBUG(HMOD, "node payload limit now %u", (unsigned)fips_framing_payload_limit(mtu));
}
