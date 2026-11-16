#ifndef FIPSD_IPV6_TCP_MSS_H
#define FIPSD_IPV6_TCP_MSS_H
/* TCP MSS clamping for MTU handling (upstream tcp_mss.rs): intercept SYN
 * segments and lower the MSS option so segments fit the FIPS effective MTU. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Clamp the MSS option of a SYN-flagged segment to max_mss. The TCP header is
 * found behind any Hop-by-Hop, Routing and Destination Options headers and
 * clamped to the same ceiling as one without them (RFC 9293 Section 3.7.1); a
 * segment behind an Authentication, Fragment, ESP or unknown header is left
 * alone, and so is a ceiling of zero. The TCP checksum is updated for the two
 * bytes rewritten (RFC 1624 eq. 3), never recomputed. True when modified. */
bool fipsd_ipv6_clamp_tcp_mss(uint8_t *packet, size_t len, uint16_t max_mss);
/* Recompute the TCP or UDP checksum (IPv6 pseudo-header + segment) in place,
 * assuming the L4 header follows the fixed header; completes the checksum
 * macOS leaves offloaded on hairpinned self-traffic. No-op otherwise. */
void fipsd_ipv6_recalculate_l4_checksum(uint8_t *packet, size_t len);
#endif
