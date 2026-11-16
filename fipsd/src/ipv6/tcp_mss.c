#include "ipv6/tcp_mss.h"
#include "ipv6/exthdr.h"
#define TCP_HEADER_MIN_LEN 20u
#define TCP_OPT_MSS 2u
#define TCP_OPT_MSS_LEN 4u
#define TCP_FLAGS_OFFSET 13u
#define TCP_FLAG_SYN 0x02u
static uint16_t be16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint32_t words(const uint8_t *p, size_t first, size_t end)
{
    uint32_t sum = 0;
    for (size_t i = first; i + 2 <= end; i += 2) sum += be16(p + i);
    return sum;
}
/* Write value at `at` in the segment starting at `tcp` and update the
 * segment's checksum for the words touched, which may be two at an odd offset. */
static void rewrite(uint8_t *packet, size_t tcp, size_t at, uint16_t value)
{
    size_t first = tcp + ((at - tcp) & ~(size_t)1);
    size_t end = tcp + ((at + 2 - tcp + 1) & ~(size_t)1);
    uint32_t old = words(packet, first, end), old_words = (uint32_t)((end - first) / 2);
    put16(packet + at, value);
    uint32_t fresh = words(packet, first, end);
    uint16_t checksum = be16(packet + tcp + 16);
    /* ~HC + ~m + m', with ~m summed as 0xffff - m for each old word. */
    uint32_t sum = (uint32_t)(uint16_t)~checksum + old_words * 0xffffu - old + fresh;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    put16(packet + tcp + 16, (uint16_t)~(uint16_t)sum);
}
bool fipsd_ipv6_clamp_tcp_mss(uint8_t *packet, size_t len, uint16_t max_mss)
{
    fipsd_ipv6_chain chain = fipsd_ipv6_upper(packet, len);
    if (chain.kind != FIPSD_IPV6_CHAIN_UPPER || chain.proto != FIPSD_IPV6_NH_TCP || chain.sealed) return false;
    /* A ceiling of zero carries no information and an MSS of zero is not a
     * legal segment size: refuse rather than write it. */
    if (!max_mss) return false;
    size_t tcp = chain.at;
    if (len < tcp + TCP_HEADER_MIN_LEN) return false;
    if (!(packet[tcp + TCP_FLAGS_OFFSET] & TCP_FLAG_SYN)) return false;
    size_t header_len = (size_t)(packet[tcp + 12] >> 4) * 4;
    if (header_len < TCP_HEADER_MIN_LEN || header_len > len - tcp) return false;
    size_t i = tcp + TCP_HEADER_MIN_LEN, end = tcp + header_len;
    while (i < end) {
        uint8_t kind = packet[i];
        if (kind == 0) break;
        if (kind == 1) { ++i; continue; }
        if (i + 1 >= end) break;
        size_t length = packet[i + 1];
        if (length < 2 || i + length > end) break;
        if (kind == TCP_OPT_MSS && length == TCP_OPT_MSS_LEN) {
            if (be16(packet + i + 2) > max_mss) { rewrite(packet, tcp, i + 2, max_mss); return true; }
            return false;
        }
        i += length;
    }
    return false;
}
void fipsd_ipv6_recalculate_l4_checksum(uint8_t *packet, size_t len)
{
    if (!packet || len < 40 || packet[0] >> 4 != 6) return;
    size_t payload_len = be16(packet + 4);
    if (!payload_len || 40 + payload_len > len) return;
    uint8_t proto = packet[6]; size_t csum;
    if (proto == FIPSD_IPV6_NH_TCP && payload_len >= TCP_HEADER_MIN_LEN) csum = 40 + 16;
    else if (proto == FIPSD_IPV6_NH_UDP && payload_len >= 8) csum = 40 + 6;
    else return;
    packet[csum] = 0; packet[csum + 1] = 0;
    uint32_t sum = 0;
    for (size_t i = 8; i < 40; i += 2) sum += be16(packet + i);
    sum += (uint32_t)payload_len + proto;
    for (size_t i = 40; i < 40 + payload_len; i += 2)
        sum += i + 1 < 40 + payload_len ? be16(packet + i) : (uint32_t)packet[i] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    uint16_t checksum = (uint16_t)~(uint16_t)sum;
    /* IPv6 forbids an all-zero UDP checksum; send 0xffff instead (TCP keeps 0). */
    if (!checksum && proto == FIPSD_IPV6_NH_UDP) checksum = 0xffff;
    put16(packet + csum, checksum);
}
