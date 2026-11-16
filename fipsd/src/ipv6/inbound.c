#include "ipv6/inbound.h"
#include "ipv6/shim.h"
int fipsd_ipv6_unpack(const fipsd_ipv6_deliver *d, const uint8_t source[16], const uint8_t dest[16], uint8_t *out, size_t cap)
{
    int n = fipsd_ipv6_decompress(d->payload, d->len, source, dest, out, cap);
    if (n > 0 && d->ce) fipsd_ipv6_mark_ce(out, (size_t)n);
    return n;
}
void fipsd_ipv6_mark_ce(uint8_t *packet, size_t len)
{
    if (!packet || len < 2) return;
    /* byte[0] bits[3:0] = TC[7:4]; byte[1] bits[7:4] = TC[3:0]; ECN is TC[1:0]. */
    uint8_t tc = (uint8_t)((packet[0] & 0x0f) << 4 | packet[1] >> 4);
    if ((tc & 0x03) == 0) return; /* Not-ECT is never marked */
    uint8_t marked = tc | 0x03;
    packet[0] = (uint8_t)((packet[0] & 0xf0) | marked >> 4);
    packet[1] = (uint8_t)((marked << 4) | (packet[1] & 0x0f));
}
