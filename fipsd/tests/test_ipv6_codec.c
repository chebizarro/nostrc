/* IPv6 shim vector tests (nostrc-9nxwk.2), ported from upstream
 * src/ipv6tun/{ipv6_shim,exthdr,tcp_mss,mss,mtu,ipv6_frag}.rs and their
 * tests/ files: header compression, the extension header walk, the TCP MSS
 * clamp and L4 checksum, the per-flow ceiling, the MTU arithmetic at every
 * u16 input, and ingress fragmentation. Pure functions; no clock, no I/O. */
#include "ipv6/shim.h"
#include "ipv6/exthdr.h"
#include "ipv6/tcp_mss.h"
#include "ipv6/mss.h"
#include "ipv6/frag.h"
#include <fips/proto/framing.h>
#include <fips/proto/mmp/limits.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* ===== ipv6_shim.rs ===== */
static size_t build_ipv6_packet(uint8_t tc, uint32_t flow, uint8_t nh, uint8_t hop, const uint8_t src[16],
                                const uint8_t dst[16], const uint8_t *payload, size_t len, uint8_t *out)
{
    out[0] = (uint8_t)(0x60 | tc >> 4);
    out[1] = (uint8_t)((uint8_t)(tc << 4) | ((flow >> 16) & 0x0f));
    out[2] = (uint8_t)(flow >> 8); out[3] = (uint8_t)flow;
    put16(out + 4, (uint16_t)len); out[6] = nh; out[7] = hop;
    memcpy(out + 8, src, 16); memcpy(out + 24, dst, 16);
    if (len) memcpy(out + 40, payload, len);
    return 40 + len;
}
static const uint8_t sample_src[16] = {0xfd, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const uint8_t sample_dst[16] = {0xfd, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static void roundtrip(uint8_t tc, uint32_t flow, uint8_t nh, uint8_t hop, size_t payload_len, uint8_t fill)
{
    uint8_t payload[1400], pkt[1500], compressed[1500], back[1500];
    memset(payload, fill, sizeof payload);
    size_t n = build_ipv6_packet(tc, flow, nh, hop, sample_src, sample_dst, payload, payload_len, pkt);
    int c = fipsd_ipv6_compress(pkt, n, compressed, sizeof compressed); assert(c > 0);
    assert((size_t)c == n - 33);
    int d = fipsd_ipv6_decompress(compressed, (size_t)c, sample_src, sample_dst, back, sizeof back);
    assert(d == (int)n && !memcmp(back, pkt, n));
}
static void codec_tests(void)
{
    roundtrip(0, 0, 17, 64, 100, 0xaa);                /* test_compress_decompress_roundtrip */
    roundtrip(0, 0, 59, 1, 0, 0);                      /* test_roundtrip_empty_payload */
    { uint8_t pkt[40], c[40]; build_ipv6_packet(0, 0, 59, 1, sample_src, sample_dst, NULL, 0, pkt);
      assert(fipsd_ipv6_compress(pkt, 40, c, sizeof c) == 1 + 6); }
    roundtrip(0, 0, 6, 128, 1400, 0x55);               /* test_roundtrip_large_payload */
    { uint8_t p[3] = {1, 2, 3}, pkt[64], c[64], back[64]; /* test_preserves_traffic_class */
      size_t n = build_ipv6_packet(0xab, 0, 17, 64, sample_src, sample_dst, p, 3, pkt);
      int cn = fipsd_ipv6_compress(pkt, n, c, sizeof c); assert(cn > 0);
      assert(fipsd_ipv6_decompress(c, (size_t)cn, sample_src, sample_dst, back, sizeof back) == (int)n && !memcmp(back, pkt, n));
      assert((uint8_t)((back[0] & 0x0f) << 4 | back[1] >> 4) == 0xab); }
    roundtrip(0, 0xfedcb, 17, 64, 1, 1);               /* test_preserves_flow_label */
    roundtrip(0xff, 0xfffff, 17, 64, 1, 1);            /* test_preserves_tc_and_flow_label_combined */
    { uint8_t z[20] = {0}, pkt[64], c[64], back[64];   /* test_preserves_next_header_tcp */
      size_t n = build_ipv6_packet(0, 0, 6, 64, sample_src, sample_dst, z, 20, pkt);
      int cn = fipsd_ipv6_compress(pkt, n, c, sizeof c); assert(cn > 0);
      assert(fipsd_ipv6_decompress(c, (size_t)cn, sample_src, sample_dst, back, sizeof back) == (int)n && back[6] == 6); }
    { uint8_t z[8] = {0}, pkt[64], c[64], back[64];    /* test_preserves_next_header_icmpv6 */
      size_t n = build_ipv6_packet(0, 0, 58, 255, sample_src, sample_dst, z, 8, pkt);
      int cn = fipsd_ipv6_compress(pkt, n, c, sizeof c); assert(cn > 0);
      assert(fipsd_ipv6_decompress(c, (size_t)cn, sample_src, sample_dst, back, sizeof back) == (int)n && back[6] == 58 && back[7] == 255); }
    { static const uint8_t hops[] = {0, 1, 64, 128, 255}; /* test_preserves_hop_limit */
      for (size_t i = 0; i < 5; ++i) { uint8_t one = 1, pkt[64], c[64], back[64];
        size_t n = build_ipv6_packet(0, 0, 17, hops[i], sample_src, sample_dst, &one, 1, pkt);
        int cn = fipsd_ipv6_compress(pkt, n, c, sizeof c); assert(cn > 0);
        assert(fipsd_ipv6_decompress(c, (size_t)cn, sample_src, sample_dst, back, sizeof back) == (int)n && back[7] == hops[i]); } }
    { uint8_t p[256], pkt[300], c[300], back[300]; memset(p, 0xbb, sizeof p); /* test_payload_length_reconstructed */
      size_t n = build_ipv6_packet(0, 0, 17, 64, sample_src, sample_dst, p, 256, pkt);
      int cn = fipsd_ipv6_compress(pkt, n, c, sizeof c); assert(cn > 0);
      assert(fipsd_ipv6_decompress(c, (size_t)cn, sample_src, sample_dst, back, sizeof back) == (int)n && be16(back + 4) == 256); }
    { uint8_t p[100] = {0}, pkt[140], c[140];          /* test_compression_saves_bytes */
      size_t n = build_ipv6_packet(0, 0, 17, 64, sample_src, sample_dst, p, 100, pkt);
      assert(n == 140 && fipsd_ipv6_compress(pkt, n, c, sizeof c) == 107); }
    { uint8_t one = 1, pkt[64], c[64];                 /* test_compress_rejects_non_ipv6 */
      size_t n = build_ipv6_packet(0, 0, 17, 64, sample_src, sample_dst, &one, 1, pkt); pkt[0] = 0x40;
      assert(fipsd_ipv6_compress(pkt, n, c, sizeof c) < 0); }
    { uint8_t short39[39], c[64]; memset(short39, 0x60, sizeof short39); /* test_compress_rejects_short_packet */
      assert(fipsd_ipv6_compress(short39, 39, c, sizeof c) < 0 && fipsd_ipv6_compress(short39, 0, c, sizeof c) < 0); }
    { uint8_t c[7] = {0x01, 0, 0, 0, 0, 0, 0}, back[64]; /* test_decompress_rejects_unknown_format */
      assert(fipsd_ipv6_decompress(c, 7, sample_src, sample_dst, back, sizeof back) < 0); }
    { uint8_t c[6] = {0}, back[64];                    /* test_decompress_rejects_short_payload */
      assert(fipsd_ipv6_decompress(c, 6, sample_src, sample_dst, back, sizeof back) < 0);
      assert(fipsd_ipv6_decompress(c, 0, sample_src, sample_dst, back, sizeof back) < 0); }
    { uint8_t osrc[16], odst[16], two[2] = {1, 2}, pkt[64], c[64], back[64]; /* test_addresses_from_context */
      memset(osrc, 0xaa, 16); osrc[0] = 0xfd; memset(odst, 0xbb, 16); odst[0] = 0xfd;
      size_t n = build_ipv6_packet(0, 0, 17, 64, osrc, odst, two, 2, pkt);
      int cn = fipsd_ipv6_compress(pkt, n, c, sizeof c); assert(cn > 0);
      assert(fipsd_ipv6_decompress(c, (size_t)cn, sample_src, sample_dst, back, sizeof back) == (int)n);
      assert(!memcmp(back + 8, sample_src, 16) && !memcmp(back + 24, sample_dst, 16));
      assert(!memcmp(back, pkt, 4) && back[6] == pkt[6] && back[7] == pkt[7] && !memcmp(back + 40, pkt + 40, n - 40)); }
    /* decompress refuses a buffer that cannot hold the rebuilt packet */
    { uint8_t c[8] = {0}, back[40]; assert(fipsd_ipv6_decompress(c, 8, sample_src, sample_dst, back, 40) < 0); }
}

/* ===== exthdr.rs ===== */
static size_t chain(uint8_t first, const uint8_t *rest, size_t rest_len, uint8_t *out)
{
    memset(out, 0, 40); out[0] = 0x60; put16(out + 4, (uint16_t)rest_len); out[6] = first; out[7] = 64;
    if (rest_len) memcpy(out + 40, rest, rest_len);
    return 40 + rest_len;
}
static size_t options(uint8_t next, uint8_t *out) { const uint8_t h[8] = {next, 0, 1, 4, 0, 0, 0, 0}; memcpy(out, h, 8); return 8; }
static size_t routing(uint8_t next, uint8_t *out) { const uint8_t h[8] = {next, 2, 4, 1, 0, 0, 0, 0}; memcpy(out, h, 8); memset(out + 8, 0xfd, 16); return 24; }
static size_t auth(uint8_t next, uint8_t *out) { const uint8_t h[4] = {next, 4, 0, 0}; memcpy(out, h, 4); memset(out + 4, 0x11, 20); return 24; }
static size_t fragment(uint8_t next, uint16_t offset, uint8_t *out)
{
    uint16_t field = (uint16_t)(offset << 3 | 1);
    out[0] = next; out[1] = 0; put16(out + 2, field); out[4] = 0; out[5] = 0; out[6] = 0x12; out[7] = 0x34; return 8;
}
static void expect_chain(const uint8_t *p, size_t n, fipsd_ipv6_chain_kind kind, uint8_t proto, size_t at, bool sealed, const char *what)
{
    fipsd_ipv6_chain c = fipsd_ipv6_upper(p, n);
    if (c.kind != kind || (kind != FIPSD_IPV6_CHAIN_TRUNCATED && c.proto != proto) ||
        (kind == FIPSD_IPV6_CHAIN_UPPER && (c.at != at || c.sealed != sealed))) {
        fprintf(stderr, "exthdr %s: kind %d proto %u at %zu sealed %d\n", what, c.kind, c.proto, c.at, c.sealed); abort();
    }
}
static void exthdr_tests(void)
{
    uint8_t tcp[20] = {0}, rest[128], p[256]; size_t n, r;
    n = chain(6, tcp, 20, p); expect_chain(p, n, FIPSD_IPV6_CHAIN_UPPER, 6, 40, false, "none");
    r = options(6, rest); memcpy(rest + r, tcp, 20); n = chain(0, rest, r + 20, p); expect_chain(p, n, FIPSD_IPV6_CHAIN_UPPER, 6, 48, false, "Hop-by-Hop");
    r = routing(6, rest); memcpy(rest + r, tcp, 20); n = chain(43, rest, r + 20, p); expect_chain(p, n, FIPSD_IPV6_CHAIN_UPPER, 6, 64, false, "Routing");
    r = options(58, rest); memset(rest + r, 1, 8); n = chain(60, rest, r + 8, p); expect_chain(p, n, FIPSD_IPV6_CHAIN_UPPER, 58, 48, false, "Destination Options");
    r = auth(6, rest); memcpy(rest + r, tcp, 20); n = chain(51, rest, r + 20, p); expect_chain(p, n, FIPSD_IPV6_CHAIN_UPPER, 6, 64, true, "Authentication");
    r = fragment(6, 0, rest); memcpy(rest + r, tcp, 20); n = chain(44, rest, r + 20, p); expect_chain(p, n, FIPSD_IPV6_CHAIN_UPPER, 6, 48, true, "first Fragment");
    r = options(60, rest); r += options(43, rest + r); r += routing(17, rest + r); memset(rest + r, 0, 8);
    n = chain(0, rest, r + 8, p); expect_chain(p, n, FIPSD_IPV6_CHAIN_UPPER, 17, 80, false, "Hop-by-Hop, Destination Options, Routing");
    /* the_walk_stops_at_esp_no_next_header_and_an_unknown_header */
    static const uint8_t stops[] = {50, 59, 140};
    for (size_t i = 0; i < 3; ++i) { r = options(stops[i], rest); memset(rest + r, 0, 16); n = chain(60, rest, r + 16, p);
        expect_chain(p, n, FIPSD_IPV6_CHAIN_UPPER, stops[i], 48, false, "stop"); }
    /* a_later_fragment_names_its_protocol_and_no_header */
    r = options(44, rest); r += fragment(58, 185, rest + r); memset(rest + r, 1, 64); n = chain(0, rest, r + 64, p);
    expect_chain(p, n, FIPSD_IPV6_CHAIN_LATER_FRAGMENT, 58, 0, false, "later fragment");
    /* a_chain_that_runs_past_the_packet_is_truncated */
    { const uint8_t over[8] = {6, 255, 1, 4, 0, 0, 0, 0}; memcpy(rest, over, 8); memset(rest + 8, 0, 20); n = chain(60, rest, 28, p);
      expect_chain(p, n, FIPSD_IPV6_CHAIN_TRUNCATED, 0, 0, false, "length overruns"); }
    { r = options(51, rest); const uint8_t cut[4] = {6, 4, 0, 0}; memcpy(rest + r, cut, 4); n = chain(0, rest, r + 4, p);
      expect_chain(p, n, FIPSD_IPV6_CHAIN_TRUNCATED, 0, 0, false, "header cut short"); }
    n = chain(60, NULL, 0, p); expect_chain(p, n, FIPSD_IPV6_CHAIN_TRUNCATED, 0, 0, false, "header missing");
    memset(p, 0x60, 39); expect_chain(p, 39, FIPSD_IPV6_CHAIN_TRUNCATED, 0, 0, false, "under 40 bytes");
    n = chain(6, tcp, 20, p); p[0] = 0x45; expect_chain(p, n, FIPSD_IPV6_CHAIN_TRUNCATED, 0, 0, false, "not IPv6");
}

/* ===== tcp_mss.rs ===== */
static size_t make_tcp_syn_packet(const uint8_t src[16], const uint8_t dst[16], uint16_t mss, uint8_t *p)
{
    memset(p, 0, 80); p[0] = 0x60; put16(p + 4, 40); p[6] = 6; p[7] = 64; memcpy(p + 8, src, 16); memcpy(p + 24, dst, 16);
    put16(p + 40, 12345); put16(p + 42, 80); put32(p + 44, 1000); put32(p + 48, 0); p[52] = 0xa0; p[53] = 0x02; put16(p + 54, 8192);
    p[60] = 2; p[61] = 4; put16(p + 62, mss); p[64] = 0;
    fipsd_ipv6_recalculate_l4_checksum(p, 80);
    return 80;
}
static bool l4_checksum_valid(const uint8_t *pkt)
{
    size_t payload_len = be16(pkt + 4); uint32_t sum = 0;
    for (size_t i = 8; i < 40; i += 2) sum += be16(pkt + i);
    sum += (uint32_t)payload_len + pkt[6];
    for (size_t i = 40; i < 40 + payload_len; i += 2) sum += i + 1 < 40 + payload_len ? be16(pkt + i) : (uint32_t)pkt[i] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)sum == 0xffff;
}
static const uint8_t SELF_ADDR[16] = {0xfd, 0x12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x55};
static uint16_t tcp_sum(const uint8_t *packet, size_t len, size_t at)
{
    uint32_t sum = 0;
    for (size_t i = 8; i < 40; i += 2) sum += be16(packet + i);
    sum += (uint32_t)(len - at) + 6;
    for (size_t i = at; i < len; i += 2) sum += i + 1 < len ? be16(packet + i) : (uint32_t)packet[i] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)sum;
}
/* A SYN from seed-derived addresses behind `chain`, with a 28-byte TCP header holding options and a valid checksum. */
static size_t syn_behind(uint8_t first, const uint8_t *ch, size_t ch_len, const uint8_t *opts, size_t opts_len, uint8_t seed, uint8_t *p)
{
    memset(p, 0, 40); p[0] = 0x60; p[6] = first; p[7] = 64; p[8] = 0xfd; memset(p + 9, seed, 15); p[24] = 0xfd;
    memset(p + 25, (uint8_t)(seed * 31 + 7), 15);
    if (ch_len) memcpy(p + 40, ch, ch_len); /* callers pass NULL, 0 for no chain */
    size_t tcp = 40 + ch_len; memset(p + tcp, 0, 28);
    put16(p + tcp, 40000); put16(p + tcp + 2, 443); put32(p + tcp + 4, (uint32_t)seed * 0x01010101u);
    p[tcp + 12] = 7 << 4; p[tcp + 13] = 0x02; put16(p + tcp + 14, 64240);
    if (opts_len) memcpy(p + tcp + 20, opts, opts_len);
    size_t len = tcp + 28; put16(p + 4, (uint16_t)(len - 40));
    uint16_t sum = (uint16_t)~tcp_sum(p, len, tcp); put16(p + tcp + 16, sum);
    assert(tcp_sum(p, len, tcp) == 0xffff);
    return len;
}
static size_t mss_opt(uint16_t v, uint8_t *o) { o[0] = 2; o[1] = 4; put16(o + 2, v); return 4; }
static void tcp_mss_tests(void)
{
    uint8_t src[16] = {0xfd, 0}, dst[16] = {0xfd, 0}, p[128]; src[15] = 1; dst[15] = 2;
    size_t n = make_tcp_syn_packet(src, dst, 1460, p);
    assert(fipsd_ipv6_clamp_tcp_mss(p, n, 1200) && be16(p + 62) == 1200);          /* reduces large */
    n = make_tcp_syn_packet(src, dst, 1460, p);
    assert(!fipsd_ipv6_clamp_tcp_mss(p, n, 0) && be16(p + 62) == 1460);            /* zero ceiling */
    n = make_tcp_syn_packet(src, dst, 1000, p);
    assert(!fipsd_ipv6_clamp_tcp_mss(p, n, 1200) && be16(p + 62) == 1000);         /* small unchanged */
    n = make_tcp_syn_packet(src, dst, 1460, p); p[53] = 0x10;
    assert(!fipsd_ipv6_clamp_tcp_mss(p, n, 1200));                                  /* non-SYN */
    memset(p, 0, 80); p[0] = 0x60; p[6] = 17; assert(!fipsd_ipv6_clamp_tcp_mss(p, 80, 1200)); /* non-TCP */
    /* recalculate_l4_checksum */
    { uint8_t a[60] = {0}; a[0] = 0x60; put16(a + 4, 20); a[6] = 6; a[7] = 64; memcpy(a + 8, SELF_ADDR, 16); memcpy(a + 24, SELF_ADDR, 16);
      put16(a + 40, 52097); put16(a + 42, 9999); put32(a + 44, 1000); put32(a + 48, 2000); a[52] = 0x50; a[53] = 0x10; put16(a + 54, 2049);
      a[56] = 0x8e; a[57] = 0xce;
      assert(!l4_checksum_valid(a)); fipsd_ipv6_recalculate_l4_checksum(a, 60); assert(l4_checksum_valid(a)); }
    { uint8_t u[52] = {0}; u[0] = 0x60; put16(u + 4, 12); u[6] = 17; u[7] = 64; memcpy(u + 8, SELF_ADDR, 16); memcpy(u + 24, SELF_ADDR, 16);
      put16(u + 40, 40000); put16(u + 42, 5354); put16(u + 44, 12); u[46] = 0x8e; u[47] = 0xce; const uint8_t d[4] = {0xde, 0xad, 0xbe, 0xef}; memcpy(u + 48, d, 4);
      assert(!l4_checksum_valid(u)); fipsd_ipv6_recalculate_l4_checksum(u, 52); assert(l4_checksum_valid(u) && (u[46] || u[47])); }
    { uint8_t z[60] = {0}, before[60]; z[0] = 0x60; put16(z + 4, 20); z[6] = 59; memcpy(before, z, 60);
      fipsd_ipv6_recalculate_l4_checksum(z, 60); assert(!memcmp(z, before, 60)); }
    { uint8_t t[50] = {0}, before[50]; t[0] = 0x60; put16(t + 4, 20); t[6] = 6; memcpy(before, t, 50);
      fipsd_ipv6_recalculate_l4_checksum(t, 50); assert(!memcmp(t, before, 50)); }
    /* tests/tcp_mss.rs: SYNs behind extension headers */
    { uint8_t ch[64], opts[8]; size_t cl, ol = mss_opt(1440, opts);
      struct { const char *what; uint8_t first; } cases[] = {{"Hop-by-Hop", 0}, {"Destination Options", 60}, {"Routing", 43}, {"chain", 0}};
      for (unsigned i = 0; i < 4; ++i) {
          if (i == 0 || i == 1) cl = options(6, ch); else if (i == 2) cl = routing(6, ch);
          else { cl = options(60, ch); cl += options(43, ch + cl); cl += routing(6, ch + cl); }
          n = syn_behind(cases[i].first, ch, cl, opts, ol, 3, p);
          size_t tcp = 40 + cl;
          assert(fipsd_ipv6_clamp_tcp_mss(p, n, 1200)); assert(be16(p + tcp + 22) == 1200); assert(tcp_sum(p, n, tcp) == 0xffff);
      } }
    { uint8_t ch[64], opts[8], before[128]; size_t cl, ol = mss_opt(1440, opts);
      for (unsigned i = 0; i < 4; ++i) {
          uint8_t first;
          if (i == 0) { cl = auth(6, ch); first = 51; }
          else if (i == 1) { const uint8_t f[8] = {6, 0, 0, 1, 0, 0, 0x12, 0x34}; memcpy(ch, f, 8); cl = 8; first = 44; }
          else if (i == 2) { cl = options(50, ch); memset(ch + cl, 0, 8); cl += 8; first = 0; }
          else { cl = options(140, ch); const uint8_t f[8] = {6, 0, 0, 0, 0, 0, 0, 0}; memcpy(ch + cl, f, 8); cl += 8; first = 60; }
          n = syn_behind(first, ch, cl, opts, ol, 5, p); memcpy(before, p, n);
          assert(!fipsd_ipv6_clamp_tcp_mss(p, n, 1200) && !memcmp(p, before, n));
      } }
    { uint8_t ch[8], opts[8], before[128]; size_t cl = options(6, ch);
      static const uint16_t within[] = {1100, 1200};
      for (unsigned i = 0; i < 2; ++i) { size_t ol = mss_opt(within[i], opts); n = syn_behind(0, ch, cl, opts, ol, 9, p); memcpy(before, p, n);
          assert(!fipsd_ipv6_clamp_tcp_mss(p, n, 1200) && !memcmp(p, before, n)); } }
    { uint8_t ch[8], shifted[5] = {1}; mss_opt(1440, shifted + 1);
      for (unsigned i = 0; i < 2; ++i) { size_t cl = i ? options(6, ch) : 0; n = syn_behind(i ? 0 : 6, ch, cl, shifted, 5, 11, p);
          size_t tcp = 40 + cl; assert(fipsd_ipv6_clamp_tcp_mss(p, n, 1200)); assert(be16(p + tcp + 23) == 1200); assert(tcp_sum(p, n, tcp) == 0xffff); } }
    /* clamping_a_syn_with_no_extension_header_writes_what_rewriting_and_recomputing_wrote */
    { unsigned compared = 0; uint8_t opts[8], original[128], clamped[128], expected[128];
      for (uint8_t seed = 0; seed < 8; ++seed) for (size_t nops = 0; nops < 2; ++nops) {
          for (unsigned value = 536; value <= 1460 + 37; value += 37) {
              uint16_t v = value > 1460 ? 65535 : (uint16_t)value;
              size_t ol = 0; for (size_t k = 0; k < nops; ++k) opts[ol++] = 1; ol += mss_opt(v, opts + ol);
              for (unsigned ceiling = 500; ceiling <= 1500; ceiling += 53) {
                  n = syn_behind(6, NULL, 0, opts, ol, seed, original);
                  memcpy(clamped, original, n); memcpy(expected, original, n);
                  bool wrote = fipsd_ipv6_clamp_tcp_mss(clamped, n, (uint16_t)ceiling);
                  if (v > ceiling) { size_t at = 40 + 20 + nops + 2; put16(expected + at, (uint16_t)ceiling); fipsd_ipv6_recalculate_l4_checksum(expected, n); }
                  assert(wrote == (v > ceiling)); assert(!memcmp(clamped, expected, n));
                  ++compared;
              }
          }
      }
      assert(compared > 1000); }
}

/* ===== mss.rs ===== */
static void fips_addr_with_node_byte(uint8_t b, uint8_t out[16]) { memset(out, 0, 16); out[0] = 0xfd; out[1] = b; }
static uint16_t held(uint16_t mtu) { return fips_framing_payload_limit(mtu); }
static size_t syn_packet(const uint8_t src[16], const uint8_t dst[16], uint16_t mss, uint8_t *p)
{
    memset(p, 0, 64); p[0] = 0x60; put16(p + 4, 24); p[6] = 6; p[7] = 64; memcpy(p + 8, src, 16); memcpy(p + 24, dst, 16);
    p[52] = 6 << 4; p[53] = 0x02; p[60] = 2; p[61] = 4; put16(p + 62, mss); return 64;
}
static void mss_tests(void)
{
    fipsd_ipv6_paths lookup; assert(fipsd_ipv6_paths_init(&lookup, 8) == 0);
    uint8_t addr[16], a[16], b[16]; fips_addr_with_node_byte(0x42, addr);
    assert(fipsd_ipv6_per_flow_max_mss(&lookup, addr, 16, 1360) == 1143);   /* empty lookup: conservative */
    assert(fipsd_ipv6_per_flow_max_mss(&lookup, addr, 16, 1100) == 1100);   /* global smaller */
    assert(fipsd_ipv6_paths_set(&lookup, addr, held(1280)));
    assert(fipsd_ipv6_per_flow_max_mss(&lookup, addr, 16, 1360) == 1143);   /* clamps to path */
    assert(fipsd_ipv6_paths_set(&lookup, addr, held(1452)));
    assert(fipsd_ipv6_per_flow_max_mss(&lookup, addr, 16, 1143) == 1143);   /* keeps global when path larger */
    assert(fipsd_ipv6_per_flow_max_mss(&lookup, addr, 16, 1360) == 1315);   /* learned overrides conservative */
    static const uint16_t degenerate[] = {0, 1, 100, 137};
    for (unsigned i = 0; i < 4; ++i) { assert(fipsd_ipv6_paths_set(&lookup, addr, held(degenerate[i])));
        assert(fipsd_ipv6_per_flow_max_mss(&lookup, addr, 16, 1360) == 1143); }
    static const uint16_t want[][2] = {{138, 1}, {240, 103}, {255, 118}};
    for (unsigned i = 0; i < 3; ++i) { assert(fipsd_ipv6_paths_set(&lookup, addr, held(want[i][0])));
        assert(fipsd_ipv6_per_flow_max_mss(&lookup, addr, 16, 1360) == want[i][1]); }
    assert(fipsd_ipv6_paths_set(&lookup, addr, held(FIPS_MMP_MIN_ACTIONABLE_PATH_MTU)));
    assert(fipsd_ipv6_per_flow_max_mss(&lookup, addr, 16, 1360) == 119);    /* actionable floor honored */
    fipsd_ipv6_paths_clear(&lookup); assert(fipsd_ipv6_paths_count(&lookup) == 0);
    { uint8_t ll[16] = {0xfe, 0x80}; assert(fipsd_ipv6_per_flow_max_mss(&lookup, ll, 16, 1360) == 1143); }  /* non-fips */
    { uint8_t s8[8] = {0}; assert(fipsd_ipv6_per_flow_max_mss(&lookup, s8, 8, 1360) == 1143); }            /* short slice */
    fips_addr_with_node_byte(0x10, a); fips_addr_with_node_byte(0x20, b);
    assert(fipsd_ipv6_paths_set(&lookup, a, held(1280)) && fipsd_ipv6_paths_set(&lookup, b, held(1452)));
    assert(fipsd_ipv6_per_flow_max_mss(&lookup, a, 16, 1360) == 1143 && fipsd_ipv6_per_flow_max_mss(&lookup, b, 16, 1360) == 1315);
    /* a bounded table refuses the entry past its capacity, keeps the rest, and forgets on remove */
    for (uint8_t i = 0; i < 6; ++i) { uint8_t c[16]; fips_addr_with_node_byte((uint8_t)(0x30 + i), c); assert(fipsd_ipv6_paths_set(&lookup, c, held(1300))); }
    { uint8_t c[16]; fips_addr_with_node_byte(0x77, c); assert(!fipsd_ipv6_paths_set(&lookup, c, held(1300))); uint16_t l;
      assert(!fipsd_ipv6_paths_get(&lookup, c, &l)); fipsd_ipv6_paths_remove(&lookup, a); assert(fipsd_ipv6_paths_set(&lookup, c, held(1300))); }
    fipsd_ipv6_paths_destroy(&lookup);
    /* clamp: toward the host by the path to its source, from the host by the path to its destination */
    assert(fipsd_ipv6_paths_init(&lookup, 8) == 0);
    uint8_t far[16], host[16], p[80]; fips_addr_with_node_byte(0x42, far); fips_addr_with_node_byte(0x55, host);
    assert(fipsd_ipv6_paths_set(&lookup, far, held(1000)));
    uint16_t applied = 0; size_t n = syn_packet(far, host, 1440, p);
    assert(fipsd_ipv6_clamp(p, n, 1360, &lookup, FIPSD_IPV6_REMOTE_SOURCE, &applied) && applied == 863 && be16(p + 62) == 863);
    n = syn_packet(host, far, 1440, p);
    assert(fipsd_ipv6_clamp(p, n, 1360, &lookup, FIPSD_IPV6_REMOTE_DESTINATION, &applied) && applied == 863 && be16(p + 62) == 863);
    fipsd_ipv6_paths_clear(&lookup);
    { uint8_t s[20], before[20]; memset(s, 0x60, 20); memcpy(before, s, 20);
      assert(!fipsd_ipv6_clamp(s, 20, 1360, &lookup, FIPSD_IPV6_REMOTE_SOURCE, NULL) && !memcmp(s, before, 20)); }
    n = syn_packet(far, host, 1440, p); p[53] = 0x10;
    assert(!fipsd_ipv6_clamp(p, n, 1360, &lookup, FIPSD_IPV6_REMOTE_SOURCE, NULL) && be16(p + 62) == 1440);
    { uint8_t q[72] = {0}; q[0] = 0x60; put16(q + 4, 32); q[6] = 0; q[7] = 64; memcpy(q + 8, far, 16); memcpy(q + 24, host, 16);
      const uint8_t hbh[8] = {6, 0, 1, 4, 0, 0, 0, 0}; memcpy(q + 40, hbh, 8); q[48 + 12] = 6 << 4; q[48 + 13] = 0x12;
      const uint8_t o[4] = {2, 4, 0x05, 0xa0}; memcpy(q + 68, o, 4);
      assert(fipsd_ipv6_clamp(q, 72, 1360, &lookup, FIPSD_IPV6_REMOTE_SOURCE, &applied) && applied == 1143 && be16(q + 70) == 1143); }
    fipsd_ipv6_paths_destroy(&lookup);
    assert(fipsd_ipv6_flow_ceiling(false, 0, 1363) == 1143);
    assert(fipsd_ipv6_flow_ceiling(true, 27, 1363) == 1143);
    assert(fipsd_ipv6_flow_ceiling(true, 1090, 1363) == 1063);
    assert(fipsd_ipv6_flow_ceiling(false, 0, 1000) == 1000);
    assert(fipsd_ipv6_flow_ceiling(true, 1090, 1000) == 1000);
}

/* ===== mtu.rs ===== */
static uint16_t ssub(uint16_t a, uint16_t b) { return a > b ? (uint16_t)(a - b) : 0; }
static uint16_t sadd(uint16_t a, uint16_t b) { return a > UINT16_MAX - b ? UINT16_MAX : (uint16_t)(a + b); }
static void mtu_tests(void)
{
    for (unsigned t = 0; t <= UINT16_MAX; ++t) {
        uint16_t x = (uint16_t)t;
        assert(fipsd_ipv6_effective_mtu(x) == ssub(x, 77));
        assert(fipsd_ipv6_mss_ceiling(x) == ssub(ssub(ssub(x, 77), 40), 20));
        assert(fipsd_ipv6_mtu_of_limit(x) == sadd(x, 33));
        assert(fipsd_ipv6_tcp_mss(x) == ssub(ssub(sadd(x, 33), 40), 20));
        assert(fips_framing_payload_limit(x) == ssub(ssub(x, 77), 33));
    }
    assert(FIPSD_IPV6_OVERHEAD == 77 && FIPSD_IPV6_MIN_MTU + FIPSD_IPV6_OVERHEAD == 1357);
    assert(fipsd_ipv6_effective_mtu(1280) == 1203 && fipsd_ipv6_mss_ceiling(1280) == 1143);
    assert(fipsd_ipv6_mss_ceiling(137) == 0 && fipsd_ipv6_mss_ceiling(138) == 1);
    assert(fipsd_ipv6_mtu_of_limit(1170) == 1203 && fipsd_ipv6_tcp_mss(1170) == 1143);
    assert(fipsd_ipv6_tcp_mss(27) == 0 && fipsd_ipv6_tcp_mss(28) == 1);
    assert(fipsd_tun_mtu(1357, 0) == 1280); /* the default UDP transport carries the IPv6 minimum */
}

/* ===== ipv6_frag.rs ===== */
#define HDR 40u
#define FRAG_HDR 8u
static size_t packet(size_t total, uint8_t nh, uint8_t *p)
{
    memset(p, 0, total); p[0] = 0x60; p[1] = 0x12; p[2] = 0x34; p[3] = 0x56; put16(p + 4, (uint16_t)(total - HDR)); p[6] = nh; p[7] = 64;
    for (size_t i = 8; i < HDR; ++i) p[i] = (uint8_t)(0xf0 | (i - 8));
    for (size_t i = HDR; i < total; ++i) p[i] = (uint8_t)(i - HDR);
    if (nh == FIPSD_IPV6_NH_TCP) p[HDR + 12] = 5 << 4;
    return total;
}
static size_t host_fragment(size_t total, uint8_t inner, uint16_t offset, bool more, uint32_t ident, uint8_t *p)
{
    packet(total, FIPSD_IPV6_NH_FRAGMENT, p);
    p[HDR] = inner; p[HDR + 1] = 0; put16(p + HDR + 2, (uint16_t)(offset | (more ? 1 : 0))); put32(p + HDR + 4, ident);
    return total;
}
typedef struct fields { size_t offset; bool more; uint32_t ident; uint8_t nh; } fields;
static fields frag_fields(const uint8_t *f)
{
    uint16_t om = be16(f + HDR + 2);
    return (fields){om & 0xfff8u, (om & 1) == 1, be32(f + HDR + 4), f[HDR]};
}
static fipsd_ipv6_fragments frags;
static void expect_sizes(const fipsd_ipv6_fragments *fr, const size_t *sizes, size_t n, const char *what)
{
    if (fr->count != n) { fprintf(stderr, "%s: %zu fragments, expected %zu\n", what, fr->count, n); abort(); }
    for (size_t i = 0; i < n; ++i) if (fr->len[i] != sizes[i]) { fprintf(stderr, "%s: fragment %zu is %zu bytes, expected %zu\n", what, i, fr->len[i], sizes[i]); abort(); }
}
/* Reassemble the way a receiving host does, checking each fragment's framing. */
static size_t reassemble(const uint8_t *original, const fipsd_ipv6_fragments *fr, uint8_t *out)
{
    fields first = frag_fields(fr->bytes[0]); size_t data = 0; uint8_t joined[2048];
    for (size_t i = 0; i < fr->count; ++i) {
        const uint8_t *f = fr->bytes[i];
        assert(f[6] == FIPSD_IPV6_NH_FRAGMENT); assert((size_t)be16(f + 4) + HDR == fr->len[i]);
        assert(!memcmp(f, original, 4)); assert(!memcmp(f + 7, original + 7, HDR - 7)); assert(f[HDR + 1] == 0);
        fields x = frag_fields(f);
        assert(x.offset % 8 == 0 && x.offset == data && x.more == (i + 1 < fr->count) && x.ident == first.ident && x.nh == first.nh);
        memcpy(joined + data, f + HDR + FRAG_HDR, fr->len[i] - HDR - FRAG_HDR); data += fr->len[i] - HDR - FRAG_HDR;
    }
    memcpy(out, fr->bytes[0], HDR); put16(out + 4, (uint16_t)data); out[6] = first.nh; memcpy(out + HDR, joined, data);
    return HDR + data;
}
static void frag_tests(void)
{
    static uint8_t p[2048], q[2048], back[2048];
    /* a_1280_byte_packet_splits_into_fragments_that_fit_and_reassemble_to_the_original */
    packet(1280, FIPSD_IPV6_NH_TCP, p);
    { static const size_t mtus[] = {1123, 1203}, exp[][2] = {{1120, 216}, {1200, 136}};
      for (unsigned i = 0; i < 2; ++i) {
          assert(fipsd_ipv6_fragment_for_path(p, 1280, mtus[i], 0x0badcafe, &frags)); expect_sizes(&frags, exp[i], 2, "1280 split");
          for (size_t j = 0; j < frags.count; ++j) assert(frags.len[j] <= mtus[i]);
          assert(frag_fields(frags.bytes[0]).ident == 0x0badcafe && frag_fields(frags.bytes[0]).nh == FIPSD_IPV6_NH_TCP);
          assert(reassemble(p, &frags, back) == 1280 && !memcmp(back, p, 1280)); assert(fipsd_ipv6_can_fragment(p, 1280, mtus[i]));
      } }
    /* fragmenting_is_left_to_packet_too_big_when_the_packet_fits_or_exceeds_1280_or_is_malformed */
    assert(!fipsd_ipv6_fragment_for_path(p, 1280, 1280, 1, &frags));
    packet(1123, FIPSD_IPV6_NH_TCP, q); assert(!fipsd_ipv6_fragment_for_path(q, 1123, 1123, 1, &frags));
    packet(1300, FIPSD_IPV6_NH_TCP, q); assert(!fipsd_ipv6_fragment_for_path(q, 1300, 1123, 1, &frags));
    assert(!fipsd_ipv6_fragment_for_path(p, 1280, 47, 1, &frags) && !fipsd_ipv6_fragment_for_path(p, 1280, 0, 1, &frags));
    memcpy(q, p, 1280); put16(q + 4, 1000); assert(!fipsd_ipv6_fragment_for_path(q, 1280, 1123, 1, &frags));
    packet(1280, FIPSD_IPV6_NH_NONE, q); put16(q + 4, 0); assert(!fipsd_ipv6_fragment_for_path(q, 1280, 1123, 1, &frags));
    memcpy(q, p, 1280); q[0] = 0x40; assert(!fipsd_ipv6_fragment_for_path(q, 1280, 1123, 1, &frags));
    packet(1300, FIPSD_IPV6_NH_TCP, q); assert(!fipsd_ipv6_can_fragment(q, 1300, 1123));
    /* a_host_made_fragment_is_split_further_under_its_own_identification_offset_and_final_flag */
    host_fragment(1280, FIPSD_IPV6_NH_UDP, 1232, true, 0xdeadbeef, q);
    { assert(fipsd_ipv6_fragment_for_path(q, 1280, 1123, 7, &frags)); static const size_t s[] = {1120, 208}; expect_sizes(&frags, s, 2, "middle");
      fields f0 = frag_fields(frags.bytes[0]), f1 = frag_fields(frags.bytes[1]);
      assert(f0.offset == 1232 && f0.more && f0.ident == 0xdeadbeef && f0.nh == FIPSD_IPV6_NH_UDP);
      assert(f1.offset == 2304 && f1.more && f1.ident == 0xdeadbeef && f1.nh == FIPSD_IPV6_NH_UDP);
      size_t at = 0; for (size_t i = 0; i < 2; ++i) { memcpy(back + at, frags.bytes[i] + HDR + FRAG_HDR, frags.len[i] - HDR - FRAG_HDR); at += frags.len[i] - HDR - FRAG_HDR; }
      assert(at == 1280 - HDR - FRAG_HDR && !memcmp(back, q + HDR + FRAG_HDR, at)); }
    host_fragment(1280, FIPSD_IPV6_NH_UDP, 1232, false, 0xdeadbeef, q);
    assert(fipsd_ipv6_fragment_for_path(q, 1280, 1123, 7, &frags) && frag_fields(frags.bytes[0]).more && !frag_fields(frags.bytes[1]).more);
    host_fragment(1280, FIPSD_IPV6_NH_UDP, 65000, true, 0xdeadbeef, q); assert(!fipsd_ipv6_fragment_for_path(q, 1280, 1123, 7, &frags));
    /* extension_headers_the_first_fragment_cannot_carry_whole_leave_the_packet_to_packet_too_big */
    packet(1280, FIPSD_IPV6_NH_HOP_BY_HOP, q); assert(!fipsd_ipv6_fragment_for_path(q, 1280, 1123, 1, &frags));
    packet(1280, FIPSD_IPV6_NH_ROUTING, q); assert(!fipsd_ipv6_fragment_for_path(q, 1280, 1123, 1, &frags));
    { static const uint8_t second[] = {FIPSD_IPV6_NH_ROUTING, FIPSD_IPV6_NH_FRAGMENT, 132};
      for (unsigned i = 0; i < 3; ++i) { packet(1280, FIPSD_IPV6_NH_DEST_OPTS, q); q[HDR] = second[i]; q[HDR + 1] = 0;
          assert(!fipsd_ipv6_fragment_for_path(q, 1280, 1123, 1, &frags)); } }
    packet(1280, 132, q); assert(!fipsd_ipv6_fragment_for_path(q, 1280, 1123, 1, &frags));
    packet(1280, FIPSD_IPV6_NH_DEST_OPTS, q); q[HDR] = FIPSD_IPV6_NH_TCP; q[HDR + 1] = 24; q[HDR + 200 + 12] = 5 << 4;
    assert(!fipsd_ipv6_fragment_for_path(q, 1280, 179, 1, &frags));
    { assert(fipsd_ipv6_fragment_for_path(q, 1280, 300, 1, &frags)); static const size_t s[] = {296, 296, 296, 296, 296}; expect_sizes(&frags, s, 5, "dest opts 200");
      assert(frags.len[0] - HDR - FRAG_HDR >= 220); assert(reassemble(q, &frags, back) == 1280 && !memcmp(back, q, 1280)); }
    packet(1280, FIPSD_IPV6_NH_TCP, q); q[HDR + 12] = 15 << 4;
    assert(fipsd_ipv6_fragment_for_path(q, 1280, 179, 1, &frags) && reassemble(q, &frags, back) == 1280 && !memcmp(back, q, 1280));
    /* a_udp_icmpv6_or_esp_header_that_would_start_in_the_second_fragment_leaves_the_packet_to_packet_too_big */
    { static const uint8_t upper[] = {FIPSD_IPV6_NH_UDP, FIPSD_IPV6_NH_ICMPV6, FIPSD_IPV6_NH_ESP};
      for (unsigned i = 0; i < 3; ++i) {
          packet(1280, FIPSD_IPV6_NH_DEST_OPTS, q); q[HDR] = upper[i]; q[HDR + 1] = 15;
          assert(!fipsd_ipv6_fragment_for_path(q, 1280, 179, 1, &frags) && !fipsd_ipv6_can_fragment(q, 1280, 179));
          q[HDR + 1] = 14;
          assert(fipsd_ipv6_fragment_for_path(q, 1280, 179, 1, &frags) && frags.count == 10);
          assert(reassemble(q, &frags, back) == 1280 && !memcmp(back, q, 1280));
      } }
    /* a_later_host_fragment_whose_inner_header_belongs_before_a_fragment_header_is_left_to_packet_too_big */
    { static const uint8_t inner[] = {FIPSD_IPV6_NH_HOP_BY_HOP, FIPSD_IPV6_NH_ROUTING, FIPSD_IPV6_NH_FRAGMENT};
      for (unsigned i = 0; i < 3; ++i) { host_fragment(1280, inner[i], 1232, true, 9, q);
          assert(!fipsd_ipv6_fragment_for_path(q, 1280, 1123, 1, &frags) && !fipsd_ipv6_can_fragment(q, 1280, 1123)); } }
    /* a_1280_byte_packet_needs_two_fragments_down_to_an_ipv6_mtu_of_672_and_ten_below_192 */
    packet(1280, FIPSD_IPV6_NH_UDP, p);
    { assert(fipsd_ipv6_fragment_for_path(p, 1280, 672, 1, &frags)); static const size_t a[] = {672, 664}; expect_sizes(&frags, a, 2, "672");
      assert(fipsd_ipv6_fragment_for_path(p, 1280, 671, 1, &frags)); static const size_t b[] = {664, 664, 56}; expect_sizes(&frags, b, 3, "671");
      assert(fipsd_ipv6_fragment_for_path(p, 1280, 192, 1, &frags) && frags.count == 9);
      assert(fipsd_ipv6_fragment_for_path(p, 1280, 191, 1, &frags) && frags.count == 10); }
    /* no_packet_is_split_into_more_than_ten_fragments */
    { static const size_t mtus[] = {179, 176};
      for (unsigned i = 0; i < 2; ++i) { assert(fipsd_ipv6_fragment_for_path(p, 1280, mtus[i], 1, &frags));
          size_t exp[10]; for (unsigned j = 0; j < 9; ++j) exp[j] = 176; exp[9] = 136; expect_sizes(&frags, exp, 10, "ten");
          for (size_t j = 0; j < 10; ++j) assert(frags.len[j] <= mtus[i]);
          assert(reassemble(p, &frags, back) == 1280 && !memcmp(back, p, 1280)); }
      static const size_t under[] = {175, 140, 56};
      for (unsigned i = 0; i < 3; ++i) assert(!fipsd_ipv6_fragment_for_path(p, 1280, under[i], 1, &frags));
      assert(FIPSD_IPV6_MIN_FRAG_DATA == 128); }
    /* a_packet_over_1280_on_a_path_under_1280_is_told_1280 */
    assert(fipsd_ipv6_packet_too_big_mtu(1300, 1203) == 1280 && fipsd_ipv6_packet_too_big_mtu(1280, 1203) == 1203 &&
           fipsd_ipv6_packet_too_big_mtu(1500, 1400) == 1400);
}
int main(void)
{
    codec_tests(); exthdr_tests(); tcp_mss_tests(); mss_tests(); mtu_tests(); frag_tests();
    puts("IPv6 shim codec: compression, extension headers, MSS clamp/checksum, per-flow ceiling, MTU arithmetic at every u16, fragmentation: ok");
    return 0;
}
