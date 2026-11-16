#ifndef FIPSD_IPV6_FRAG_H
#define FIPSD_IPV6_FRAG_H
/* IPv6 fragmentation at the node that carries a host's packet onto the mesh
 * (upstream ipv6_frag.rs). IPv6 promises every path 1280 bytes and a host
 * ignores a Packet Too Big that reports less (RFC 8201 Section 4), so when a
 * FIPS path cannot carry 1280 bytes after encapsulation the ingress node
 * splits a packet of up to 1280 bytes into ordinary IPv6 fragments and the
 * destination host reassembles them. Only packets the host could not have
 * sized down are split; anything else is left to the caller's Packet Too Big.
 * Reassembly is the destination host kernel's, as upstream. */
#include "ipv6/mtu.h"
#include <stdbool.h>
#include <stddef.h>
/* Smallest data chunk a fragment may carry: the chunk at the smallest path
 * MTU a node acts on (FIPS_MMP_MIN_ACTIONABLE_PATH_MTU 256 -> IPv6 MTU 179 ->
 * 128 bytes), so a 1280-byte packet is never split into more than ten. */
#define FIPSD_IPV6_MIN_FRAG_DATA 128u
#define FIPSD_IPV6_FRAG_MAX 10u
#define FIPSD_IPV6_FRAG_HDR 8u
typedef struct fipsd_ipv6_fragments {
    size_t count;
    size_t len[FIPSD_IPV6_FRAG_MAX];
    uint8_t bytes[FIPSD_IPV6_FRAG_MAX][FIPSD_IPV6_MIN_MTU];
} fipsd_ipv6_fragments;
/* Whether fragment_for_path would split packet for a path whose IPv6 MTU is mtu. */
bool fipsd_ipv6_can_fragment(const uint8_t *packet, size_t len, size_t mtu);
/* True only when the packet is over mtu and at most 1280 bytes, well formed
 * with a payload length matching its size, has no Hop-by-Hop or Routing
 * header, carries a recognised upper-layer header that fits whole in the first
 * fragment, and mtu leaves each fragment at least FIPSD_IPV6_MIN_FRAG_DATA
 * bytes. A packet that is already a fragment is split further under its own
 * identification, offset and final M flag; any other gets ident. */
bool fipsd_ipv6_fragment_for_path(const uint8_t *packet, size_t len, size_t mtu, uint32_t ident, fipsd_ipv6_fragments *out);
/* The MTU a Packet Too Big reports for a packet that does not fit mtu and is
 * not fragmented: 1280 for a packet over 1280 on a path under 1280 (the least
 * a host accepts, and its next packets are fragmented), else the path's own. */
uint32_t fipsd_ipv6_packet_too_big_mtu(size_t packet_len, size_t mtu);
#endif
