#ifndef FIPSD_IPV6_INBOUND_H
#define FIPSD_IPV6_INBOUND_H
/* Host-side half of delivering what a peer sent to the IPv6 port (upstream
 * inbound.rs): the packet is rebuilt with the sender's address as its source
 * and ours as its destination, and marked CE when the datagram arrived marked. */
#include <fips/identity/mod.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct fipsd_ipv6_deliver {
    fips_node_addr peer;      /* the sender */
    const uint8_t *payload;   /* the compressed packet */
    size_t len;
    bool ce;                  /* arrived marked Congestion Experienced */
} fipsd_ipv6_deliver;
/* The rebuilt packet's length, or -1 when the payload is not a compressed
 * IPv6 packet or out is too small. */
int fipsd_ipv6_unpack(const fipsd_ipv6_deliver *deliver, const uint8_t source[16], const uint8_t dest[16],
                      uint8_t *out, size_t cap);
/* Mark ECN-CE in the Traffic Class, only on an ECN-capable packet (RFC 3168). */
void fipsd_ipv6_mark_ce(uint8_t *packet, size_t len);
#endif
