#ifndef FIPSD_IPV6_ICMP_H
#define FIPSD_IPV6_ICMP_H
/* ICMPv6 error messages the shim sends back to the host (RFC 4443, upstream
 * icmp.rs and the answer helpers of tun.rs): Destination Unreachable for a
 * packet the mesh cannot carry to its destination, Packet Too Big for one
 * larger than the path admits, the latter rate limited per source at the
 * instant the caller gives. */
#include "ipv6/icmp_rate_limit.h"
#include "ipv6/queue.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum fipsd_icmpv6_type {
    FIPSD_ICMPV6_DEST_UNREACHABLE = 1, FIPSD_ICMPV6_PACKET_TOO_BIG = 2, FIPSD_ICMPV6_TIME_EXCEEDED = 3,
    FIPSD_ICMPV6_PARAMETER_PROBLEM = 4, FIPSD_ICMPV6_ECHO_REQUEST = 128, FIPSD_ICMPV6_ECHO_REPLY = 129
} fipsd_icmpv6_type;
typedef enum fipsd_icmpv6_unreachable_code {
    FIPSD_ICMPV6_NO_ROUTE = 0, FIPSD_ICMPV6_ADMIN_PROHIBITED = 1, FIPSD_ICMPV6_BEYOND_SCOPE = 2,
    FIPSD_ICMPV6_ADDRESS_UNREACHABLE = 3, FIPSD_ICMPV6_PORT_UNREACHABLE = 4, FIPSD_ICMPV6_SOURCE_POLICY = 5,
    FIPSD_ICMPV6_REJECT_ROUTE = 6
} fipsd_icmpv6_unreachable_code;
#define FIPSD_IPPROTO_ICMPV6 58u
#define FIPSD_ICMPV6_REDIRECT 137u
/* An ICMPv6 response never exceeds the IPv6 minimum MTU. */
#define FIPSD_IPV6_ICMP_MAX 1280u
#define FIPSD_IPV6_ICMPV6_HEADER_LEN 8u
/* The Destination Unreachable bucket for the whole shim: a burst of ten,
 * then ten a second (RFC 4443 Section 2.4 (f) defaults). */
#define FIPSD_IPV6_UNREACHABLE_BURST 10u
#define FIPSD_IPV6_UNREACHABLE_RATE 10.0
/* False for a packet too short, not IPv6, an ICMPv6 error or Redirect behind
 * any extension headers (or one that cannot be shown not to be), a multicast
 * source or destination, or an unspecified source. */
bool fipsd_ipv6_should_send_icmp_error(const uint8_t *packet, size_t len);
/* Each builder returns the response length, or -1 when the original packet is
 * invalid, owed no error, our_addr is not unicast (Packet Too Big) or cap is
 * too small; cap of FIPSD_IPV6_ICMP_MAX always suffices. */
int fipsd_ipv6_build_dest_unreachable(const uint8_t *packet, size_t len, fipsd_icmpv6_unreachable_code code,
                                      const uint8_t our[16], uint8_t *out, size_t cap);
int fipsd_ipv6_build_packet_too_big(const uint8_t *packet, size_t len, uint32_t mtu, const uint8_t our[16],
                                    uint8_t *out, size_t cap);
/* Destination Unreachable (no route) / code 5 (source address failed policy)
 * from our address, or -1 when the packet is owed no ICMPv6 error. */
int fipsd_ipv6_unreachable(const uint8_t *packet, size_t len, const uint8_t our[16], uint8_t *out, size_t cap);
int fipsd_ipv6_source_refused(const uint8_t *packet, size_t len, const uint8_t our[16], uint8_t *out, size_t cap);
/* Packet Too Big for packet, reporting mtu, if the limiter lets the packet's
 * source have one at now. The reply's source is the packet's destination, so
 * the kernel sees a remote router (Linux ignores a PTB from a local address).
 * The limiter is consulted whether or not a queue is up to take the reply
 * (sink); with no sink nothing is returned. */
int fipsd_ipv6_too_big(const uint8_t *packet, size_t len, uint32_t mtu, fipsd_ipv6_icmp_limiter *limiter,
                       fips_instant now, bool sink, uint8_t *out, size_t cap);
uint16_t fipsd_ipv6_icmpv6_checksum(const uint8_t *message, size_t len, const uint8_t src[16], const uint8_t dst[16]);
/* The tun.rs carriers: a full Destination Unreachable bucket, then the two
 * answers. A token is spent whether or not a queue is up; answer_unreachable
 * is false only when the queue was closed as the answer went to it. */
void fipsd_ipv6_unreachable_bucket_init(fips_token_bucket *bucket, fips_instant now);
bool fipsd_ipv6_answer_unreachable(const uint8_t *response, size_t len, fips_token_bucket *bucket,
                                   fips_instant now, fipsd_ipv6_queue *queue);
bool fipsd_ipv6_answer_oversize(const uint8_t *packet, size_t len, uint32_t mtu, fipsd_ipv6_icmp_limiter *limiter,
                                fips_instant now, fipsd_ipv6_queue *queue);
#endif
