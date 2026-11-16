#include "ipv6/icmp.h"
#include "ipv6/exthdr.h"
#include "log.h"
#include <string.h>
#define MOD "ipv6/icmp"
#define IPV6_HEADER_LEN 40u
#define MAX_ORIGINAL_PACKET (FIPSD_IPV6_ICMP_MAX - IPV6_HEADER_LEN - FIPSD_IPV6_ICMPV6_HEADER_LEN)
static bool unspecified(const uint8_t a[16]) { for (unsigned i = 0; i < 16; ++i) if (a[i]) return false; return true; }
/* Whether the packet is, or cannot be shown not to be, an ICMPv6 error (types
 * 0 to 127) or a Redirect, which RFC 4443 Section 2.4 (e.1)/(e.2) forbid
 * answering. The header is looked for past any extension headers. */
static bool maybe_error(const uint8_t *packet, size_t len)
{
    fipsd_ipv6_chain c = fipsd_ipv6_upper(packet, len);
    switch (c.kind) {
    case FIPSD_IPV6_CHAIN_UPPER:
        if (c.proto != FIPSD_IPPROTO_ICMPV6) return false;
        return c.at >= len || packet[c.at] < 128 || packet[c.at] == FIPSD_ICMPV6_REDIRECT;
    case FIPSD_IPV6_CHAIN_LATER_FRAGMENT: return c.proto == FIPSD_IPPROTO_ICMPV6;
    default: return true;
    }
}
bool fipsd_ipv6_should_send_icmp_error(const uint8_t *packet, size_t len)
{
    if (!packet || len < IPV6_HEADER_LEN || packet[0] >> 4 != 6) return false;
    if (unspecified(packet + 8) || packet[8] == 0xff) return false;
    if (packet[24] == 0xff) return false; /* e.g. ff02::2 from a Router Solicitation */
    return !maybe_error(packet, len);
}
uint16_t fipsd_ipv6_icmpv6_checksum(const uint8_t *m, size_t len, const uint8_t src[16], const uint8_t dst[16])
{
    uint32_t sum = 0;
    for (unsigned i = 0; i < 16; i += 2) sum += (uint32_t)src[i] << 8 | src[i + 1];
    for (unsigned i = 0; i < 16; i += 2) sum += (uint32_t)dst[i] << 8 | dst[i + 1];
    sum += (uint32_t)len >> 16; sum += (uint32_t)len & 0xffff;
    sum += FIPSD_IPPROTO_ICMPV6;
    size_t i = 0;
    while (i + 1 < len) {
        if (i == 2) { i += 2; continue; } /* the checksum field */
        sum += (uint32_t)m[i] << 8 | m[i + 1]; i += 2;
    }
    if (i < len) sum += (uint32_t)m[i] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~(uint16_t)sum;
}
/* Shared layout of both error messages: our address toward the original
 * source, type/code, four header bytes, then as much of the original as fits. */
static int build(const uint8_t *original, size_t len, uint8_t type, uint8_t code, const uint8_t header[4],
                 const uint8_t our[16], uint8_t *out, size_t cap)
{
    size_t quoted = len < MAX_ORIGINAL_PACKET ? len : MAX_ORIGINAL_PACKET;
    size_t icmp_len = FIPSD_IPV6_ICMPV6_HEADER_LEN + quoted, total = IPV6_HEADER_LEN + icmp_len;
    if (cap < total) return -1;
    memset(out, 0, total);
    out[0] = 0x60;
    out[4] = (uint8_t)(icmp_len >> 8); out[5] = (uint8_t)icmp_len;
    out[6] = FIPSD_IPPROTO_ICMPV6; out[7] = 64;
    memcpy(out + 8, our, 16);
    memcpy(out + 24, original + 8, 16); /* destination = original source */
    out[40] = type; out[41] = code;
    memcpy(out + 44, header, 4);
    memcpy(out + 48, original, quoted);
    uint16_t checksum = fipsd_ipv6_icmpv6_checksum(out + 40, icmp_len, our, original + 8);
    out[42] = (uint8_t)(checksum >> 8); out[43] = (uint8_t)checksum;
    return (int)total;
}
int fipsd_ipv6_build_dest_unreachable(const uint8_t *packet, size_t len, fipsd_icmpv6_unreachable_code code,
                                      const uint8_t our[16], uint8_t *out, size_t cap)
{
    if (!packet || len < IPV6_HEADER_LEN) return -1;
    static const uint8_t unused[4] = {0};
    return build(packet, len, FIPSD_ICMPV6_DEST_UNREACHABLE, (uint8_t)code, unused, our, out, cap);
}
int fipsd_ipv6_build_packet_too_big(const uint8_t *packet, size_t len, uint32_t mtu, const uint8_t our[16],
                                    uint8_t *out, size_t cap)
{
    if (!packet || len < IPV6_HEADER_LEN || packet[0] >> 4 != 6) return -1;
    if (unspecified(packet + 8) || packet[8] == 0xff) return -1;
    /* The message's own source must be unicast (RFC 4443 Section 2.2). */
    if (unspecified(our) || our[0] == 0xff) return -1;
    if (maybe_error(packet, len)) return -1;
    uint8_t field[4] = {(uint8_t)(mtu >> 24), (uint8_t)(mtu >> 16), (uint8_t)(mtu >> 8), (uint8_t)mtu};
    return build(packet, len, FIPSD_ICMPV6_PACKET_TOO_BIG, 0, field, our, out, cap);
}
int fipsd_ipv6_unreachable(const uint8_t *packet, size_t len, const uint8_t our[16], uint8_t *out, size_t cap)
{
    if (!fipsd_ipv6_should_send_icmp_error(packet, len)) return -1;
    return fipsd_ipv6_build_dest_unreachable(packet, len, FIPSD_ICMPV6_NO_ROUTE, our, out, cap);
}
int fipsd_ipv6_source_refused(const uint8_t *packet, size_t len, const uint8_t our[16], uint8_t *out, size_t cap)
{
    if (!fipsd_ipv6_should_send_icmp_error(packet, len)) return -1;
    return fipsd_ipv6_build_dest_unreachable(packet, len, FIPSD_ICMPV6_SOURCE_POLICY, our, out, cap);
}
int fipsd_ipv6_too_big(const uint8_t *packet, size_t len, uint32_t mtu, fipsd_ipv6_icmp_limiter *limiter,
                       fips_instant now, bool sink, uint8_t *out, size_t cap)
{
    if (!packet || len < IPV6_HEADER_LEN) return -1;
    if (!fipsd_ipv6_icmp_limiter_check(limiter, packet + 8, now)) {
        FIPSD_DEBUG(MOD, "Rate limiting ICMP Packet Too Big");
        return -1;
    }
    /* The original packet's destination as the ICMP source: the kernel then
     * sees the PTB coming from a remote router, not from itself. */
    int n = fipsd_ipv6_build_packet_too_big(packet, len, mtu, packet + 24, out, cap);
    if (n < 0 || !sink) return -1;
    FIPSD_DEBUG(MOD, "Sending ICMP Packet Too Big (packet_size=%zu reported_mtu=%u)", len, (unsigned)mtu);
    return n;
}
void fipsd_ipv6_unreachable_bucket_init(fips_token_bucket *bucket, fips_instant now)
{
    fips_token_bucket_init(bucket, (fips_bucket_params){FIPSD_IPV6_UNREACHABLE_BURST, FIPSD_IPV6_UNREACHABLE_RATE}, now);
}
bool fipsd_ipv6_answer_unreachable(const uint8_t *response, size_t len, fips_token_bucket *bucket,
                                   fips_instant now, fipsd_ipv6_queue *queue)
{
    if (!fips_token_bucket_acquire(bucket, 1, now)) {
        FIPSD_DEBUG(MOD, "Rate limiting ICMPv6 Destination Unreachable");
        return true;
    }
    return !queue || fipsd_ipv6_queue_send(queue, response, len) == 0;
}
bool fipsd_ipv6_answer_oversize(const uint8_t *packet, size_t len, uint32_t mtu, fipsd_ipv6_icmp_limiter *limiter,
                                fips_instant now, fipsd_ipv6_queue *queue)
{
    uint8_t response[FIPSD_IPV6_ICMP_MAX];
    int n = fipsd_ipv6_too_big(packet, len, mtu, limiter, now, queue != NULL, response, sizeof response);
    if (n < 0 || !queue) return false;
    (void)fipsd_ipv6_queue_send(queue, response, (size_t)n);
    return true;
}
