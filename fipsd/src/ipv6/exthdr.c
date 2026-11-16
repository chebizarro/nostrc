#include "ipv6/exthdr.h"
fipsd_ipv6_chain fipsd_ipv6_upper(const uint8_t *packet, size_t len)
{
    fipsd_ipv6_chain truncated = {.kind = FIPSD_IPV6_CHAIN_TRUNCATED};
    if (!packet || len < 40 || packet[0] >> 4 != 6) return truncated;
    uint8_t next = packet[6]; size_t at = 40; bool sealed = false;
    for (;;) {
        size_t step;
        switch (next) {
        case FIPSD_IPV6_NH_HOP_BY_HOP: case FIPSD_IPV6_NH_ROUTING: case FIPSD_IPV6_NH_DEST_OPTS:
            if (len < at + 8) return truncated;
            step = ((size_t)packet[at + 1] + 1) * 8; break;
        case FIPSD_IPV6_NH_AUTH:
            if (len < at + 8) return truncated;
            step = ((size_t)packet[at + 1] + 2) * 4; break;
        case FIPSD_IPV6_NH_FRAGMENT: {
            if (len < at + 8) return truncated;
            unsigned offset = ((unsigned)packet[at + 2] << 8 | packet[at + 3]) >> 3;
            if (offset) return (fipsd_ipv6_chain){.kind = FIPSD_IPV6_CHAIN_LATER_FRAGMENT, .proto = packet[at]};
            step = 8; break;
        }
        default:
            return (fipsd_ipv6_chain){.kind = FIPSD_IPV6_CHAIN_UPPER, .proto = next, .at = at, .sealed = sealed};
        }
        sealed |= next == FIPSD_IPV6_NH_AUTH || next == FIPSD_IPV6_NH_FRAGMENT;
        next = packet[at];
        at += step;
        if (at > len) return truncated;
    }
}
