#include "ipv6/frag.h"
#include "ipv6/exthdr.h"
#include <fips/proto/mmp/limits.h>
#include <string.h>
_Static_assert(FIPSD_IPV6_MIN_FRAG_DATA ==
    (((FIPS_MMP_MIN_ACTIONABLE_PATH_MTU - FIPSD_IPV6_OVERHEAD) - FIPSD_IPV6_HEADER_SIZE - FIPSD_IPV6_FRAG_HDR) & ~7u),
    "the smallest fragment chunk follows from the smallest actionable path MTU");
#define HDR FIPSD_IPV6_HEADER_SIZE
#define FRAG_HDR FIPSD_IPV6_FRAG_HDR
#define MIN_MTU FIPSD_IPV6_MIN_MTU
#define MAX_DATA_END 65535u
#define MAX_EXT_HEADERS 8u
typedef struct split {
    uint8_t inner_nh;   /* next header of the fragmentable part */
    size_t base;        /* offset of data within the original payload */
    bool last_more;     /* M flag of the last piece (a host-made middle fragment) */
    bool has_ident;
    uint32_t ident;
    const uint8_t *data;
    size_t data_len, chunk;
} split;
static uint16_t be16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
/* Length of the fragmentable part's headers through the upper-layer one, or
 * false when that chain cannot go into fragments. */
static bool headers_len(uint8_t nh, const uint8_t *data, size_t len, size_t *out)
{
    size_t off = 0;
    for (unsigned i = 0; i < MAX_EXT_HEADERS; ++i) {
        switch (nh) {
        case FIPSD_IPV6_NH_DEST_OPTS: case FIPSD_IPV6_NH_AUTH: {
            if (off + 2 > len) return false;
            size_t step = nh == FIPSD_IPV6_NH_AUTH ? ((size_t)data[off + 1] + 2) * 4 : ((size_t)data[off + 1] + 1) * 8;
            nh = data[off]; off += step; break;
        }
        case FIPSD_IPV6_NH_TCP: {
            if (off + 12 >= len) return false;
            size_t tcp = (size_t)(data[off + 12] >> 4) * 4;
            if (tcp < 20 || off + tcp > len) return false;
            *out = off + tcp; return true;
        }
        case FIPSD_IPV6_NH_UDP: case FIPSD_IPV6_NH_ICMPV6: case FIPSD_IPV6_NH_ESP:
            if (off + 8 > len) return false;
            *out = off + 8; return true;
        case FIPSD_IPV6_NH_NONE:
            if (off > len) return false;
            *out = off; return true;
        default: return false;
        }
    }
    return false;
}
static bool plan(const uint8_t *packet, size_t len, size_t mtu, split *s)
{
    /* Over the MTU but no larger than the IPv6 minimum, which implies an MTU
     * under 1280: the case the host has no remedy for. */
    if (!packet || len <= mtu || len > MIN_MTU) return false;
    if (len < HDR || packet[0] >> 4 != 6) return false;
    if (HDR + (size_t)be16(packet + 4) != len) return false;
    const uint8_t *payload = packet + HDR; size_t payload_len = len - HDR;
    memset(s, 0, sizeof *s);
    if (packet[6] == FIPSD_IPV6_NH_FRAGMENT) {
        if (payload_len < FRAG_HDR) return false;
        uint16_t offset_m = be16(payload + 2);
        s->inner_nh = payload[0]; s->base = offset_m & 0xfff8u; s->last_more = (offset_m & 1) == 1;
        s->has_ident = true; s->ident = be32(payload + 4);
        s->data = payload + FRAG_HDR; s->data_len = payload_len - FRAG_HDR;
    } else {
        s->inner_nh = packet[6]; s->data = payload; s->data_len = payload_len;
    }
    if (s->base + s->data_len > MAX_DATA_END) return false;
    if (mtu < HDR + FRAG_HDR) return false;
    s->chunk = (mtu - HDR - FRAG_HDR) & ~(size_t)7;
    if (s->chunk < FIPSD_IPV6_MIN_FRAG_DATA) return false;
    if (s->base == 0) {
        /* The first fragment must carry every header through the upper-layer
         * one (RFC 8200 Section 4.5). */
        size_t headers;
        if (!headers_len(s->inner_nh, s->data, s->data_len, &headers) || headers > s->chunk) return false;
    } else if (s->inner_nh == FIPSD_IPV6_NH_HOP_BY_HOP || s->inner_nh == FIPSD_IPV6_NH_ROUTING ||
               s->inner_nh == FIPSD_IPV6_NH_FRAGMENT) return false;
    return true;
}
bool fipsd_ipv6_can_fragment(const uint8_t *packet, size_t len, size_t mtu)
{
    split s; return plan(packet, len, mtu, &s);
}
bool fipsd_ipv6_fragment_for_path(const uint8_t *packet, size_t len, size_t mtu, uint32_t ident, fipsd_ipv6_fragments *out)
{
    split s;
    if (!out || !plan(packet, len, mtu, &s)) return false;
    if (!s.has_ident) s.ident = ident;
    out->count = 0;
    for (size_t pos = 0; pos < s.data_len;) {
        size_t end = pos + s.chunk < s.data_len ? pos + s.chunk : s.data_len;
        bool more = end < s.data_len || s.last_more;
        uint16_t offset = (uint16_t)(s.base + pos);
        if (out->count == FIPSD_IPV6_FRAG_MAX) return false; /* unreachable: chunk >= 128 and data <= 1240 */
        uint8_t *f = out->bytes[out->count]; size_t n = HDR + FRAG_HDR + (end - pos);
        memcpy(f, packet, HDR);
        f[4] = (uint8_t)((FRAG_HDR + end - pos) >> 8); f[5] = (uint8_t)(FRAG_HDR + end - pos);
        f[6] = FIPSD_IPV6_NH_FRAGMENT;
        f[HDR] = s.inner_nh; f[HDR + 1] = 0;
        uint16_t field = (uint16_t)(offset | (more ? 1u : 0u));
        f[HDR + 2] = (uint8_t)(field >> 8); f[HDR + 3] = (uint8_t)field;
        f[HDR + 4] = (uint8_t)(s.ident >> 24); f[HDR + 5] = (uint8_t)(s.ident >> 16);
        f[HDR + 6] = (uint8_t)(s.ident >> 8); f[HDR + 7] = (uint8_t)s.ident;
        memcpy(f + HDR + FRAG_HDR, s.data + pos, end - pos);
        out->len[out->count++] = n;
        pos = end;
    }
    return true;
}
uint32_t fipsd_ipv6_packet_too_big_mtu(size_t packet_len, size_t mtu)
{
    return packet_len > MIN_MTU && mtu < MIN_MTU ? MIN_MTU : (uint32_t)mtu;
}
