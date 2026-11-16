#ifndef FIPSD_IPV6_EXTHDR_H
#define FIPSD_IPV6_EXTHDR_H
/* The IPv6 extension header chain (RFC 8200 Section 4, upstream exthdr.rs):
 * where a packet's upper-layer header starts, past any extension headers
 * between it and the fixed header. The ICMPv6 error checks use it to find an
 * ICMPv6 message that is not straight after the fixed header, and the TCP MSS
 * clamp to find a TCP header that is not. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIPSD_IPV6_NH_HOP_BY_HOP 0u
#define FIPSD_IPV6_NH_TCP 6u
#define FIPSD_IPV6_NH_UDP 17u
#define FIPSD_IPV6_NH_ROUTING 43u
#define FIPSD_IPV6_NH_FRAGMENT 44u
#define FIPSD_IPV6_NH_ESP 50u
#define FIPSD_IPV6_NH_AUTH 51u
#define FIPSD_IPV6_NH_ICMPV6 58u
#define FIPSD_IPV6_NH_NONE 59u
#define FIPSD_IPV6_NH_DEST_OPTS 60u

typedef enum fipsd_ipv6_chain_kind {
    /* The upper-layer header: its protocol, its offset, and whether an
     * Authentication or Fragment header comes before it. ESP, No Next Header
     * and any unknown value end the walk and are reported here. */
    FIPSD_IPV6_CHAIN_UPPER,
    /* A fragment other than the first: names the protocol, carries no header. */
    FIPSD_IPV6_CHAIN_LATER_FRAGMENT,
    /* Not IPv6, shorter than the fixed header, or a header runs past the end. */
    FIPSD_IPV6_CHAIN_TRUNCATED
} fipsd_ipv6_chain_kind;
typedef struct fipsd_ipv6_chain {
    fipsd_ipv6_chain_kind kind;
    uint8_t proto;
    size_t at;
    bool sealed;
} fipsd_ipv6_chain;
/* Every step is bounds-checked and moves forward by at least 8 bytes. */
fipsd_ipv6_chain fipsd_ipv6_upper(const uint8_t *packet, size_t len);
#endif
