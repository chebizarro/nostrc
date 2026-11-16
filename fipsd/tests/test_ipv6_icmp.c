/* IPv6 shim vector tests (nostrc-9nxwk.2), ported from upstream
 * src/ipv6tun/{icmp,icmp_rate_limit,inbound,outbound,queue,core}.rs and their
 * tests/ files: ICMPv6 answers and their suppression behind extension headers,
 * the per-source Packet Too Big bucket at exact instants, delivery rebuilding
 * and CE marking, the host half's verdicts and refusal answers, the bounded
 * queue's drop policy, and the shim's copy of the node's state. */
#include "ipv6/icmp.h"
#include "ipv6/inbound.h"
#include "ipv6/outbound.h"
#include "ipv6/queue.h"
#include "ipv6/shim.h"
#include "ipv6/tcp_mss.h"
#include "host_os.h"
#include <fips/proto/framing.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* A nanosecond test clock: instants are counts the tests choose. */
static fips_instant ns(uint64_t n);
static int t_compare(fips_instant a, fips_instant b) { return (a.opaque[0] > b.opaque[0]) - (a.opaque[0] < b.opaque[0]); }
static fips_duration t_since(fips_instant a, fips_instant b)
{ uint64_t d = a.opaque[0] > b.opaque[0] ? a.opaque[0] - b.opaque[0] : 0; return (fips_duration){d / 1000000000u, (uint32_t)(d % 1000000000u)}; }
static fips_status t_add(fips_instant a, fips_duration d, fips_instant *out) { *out = ns(a.opaque[0] + d.secs * 1000000000u + d.nanos); return FIPS_OK; }
static fips_status t_sub(fips_instant a, fips_duration d, fips_instant *out) { *out = ns(a.opaque[0] - d.secs * 1000000000u - d.nanos); return FIPS_OK; }
static const fips_instant_ops t_ops = {t_compare, t_since, t_add, t_sub};
static fips_instant ns(uint64_t n) { return (fips_instant){{n, 0}, &t_ops}; }
static fips_instant ms(uint64_t m) { return ns(m * 1000000u); }
static fips_instant secs(uint64_t s) { return ns(s * 1000000000u); }

static void addr(const char *text, uint8_t out[16])
{
    /* Just the forms these tests use: fd00::N, ff02::N, ff0e::1:3, 2001:db8::1, fe80::1, ::, fd41::1, fddf::2 */
    memset(out, 0, 16);
    if (!strcmp(text, "::")) return;
    if (!strncmp(text, "2001:db8::", 10)) { out[0] = 0x20; out[1] = 0x01; out[2] = 0x0d; out[3] = 0xb8; out[15] = (uint8_t)strtoul(text + 10, NULL, 16); return; }
    if (!strcmp(text, "ff0e::1:3")) { out[0] = 0xff; out[1] = 0x0e; out[13] = 1; out[15] = 3; return; }
    unsigned hi, low;
    assert(sscanf(text, "%x::%x", &hi, &low) == 2);
    out[0] = (uint8_t)(hi >> 8); out[1] = (uint8_t)hi; out[14] = (uint8_t)(low >> 8); out[15] = (uint8_t)low;
}
static size_t make_ipv6_packet(const uint8_t src[16], const uint8_t dst[16], uint8_t nh, const uint8_t *payload, size_t len, uint8_t *p)
{
    memset(p, 0, 40 + len); p[0] = 0x60; put16(p + 4, (uint16_t)len); p[6] = nh; p[7] = 64; memcpy(p + 8, src, 16); memcpy(p + 24, dst, 16);
    if (len) memcpy(p + 40, payload, len);
    return 40 + len;
}
static uint8_t FD1[16], FD2[16], FDFFFF[16], FF02_1[16], FF02_2[16], FE80_1[16], UNSPEC[16], OUTSIDE[16], FD41_1[16], FDDF_2[16];
static void addresses(void)
{
    addr("fd00::1", FD1); addr("fd00::2", FD2); addr("fd00::ffff", FDFFFF); addr("ff02::1", FF02_1); addr("ff02::2", FF02_2);
    addr("fe80::1", FE80_1); addr("::", UNSPEC); addr("2001:db8::1", OUTSIDE); addr("fd41::1", FD41_1); addr("fddf::2", FDDF_2);
}

/* ===== icmp.rs unit tests ===== */
static void icmp_unit_tests(void)
{
    static uint8_t p[2100], r[1300]; uint8_t z8[8] = {0}, zero[2000] = {0};
    size_t n = make_ipv6_packet(FD1, FD2, 17, z8, 8, p); assert(fipsd_ipv6_should_send_icmp_error(p, n));
    n = make_ipv6_packet(UNSPEC, FD2, 17, z8, 8, p); assert(!fipsd_ipv6_should_send_icmp_error(p, n));
    n = make_ipv6_packet(FF02_1, FD2, 17, z8, 8, p); assert(!fipsd_ipv6_should_send_icmp_error(p, n));
    n = make_ipv6_packet(FE80_1, FF02_2, 17, z8, 8, p); assert(!fipsd_ipv6_should_send_icmp_error(p, n));
    { uint8_t err[8] = {1, 0, 0, 0, 0, 0, 0, 0}; n = make_ipv6_packet(FD1, FD2, 58, err, 8, p); assert(!fipsd_ipv6_should_send_icmp_error(p, n)); }
    { uint8_t echo[8] = {128, 0, 0, 0, 0, 0, 0, 0}; n = make_ipv6_packet(FD1, FD2, 58, echo, 8, p); assert(fipsd_ipv6_should_send_icmp_error(p, n)); }
    assert(!fipsd_ipv6_should_send_icmp_error(zero, 20));
    /* test_build_dest_unreachable */
    n = make_ipv6_packet(FD1, FD2, 17, z8, 8, p);
    int m = fipsd_ipv6_build_dest_unreachable(p, n, FIPSD_ICMPV6_NO_ROUTE, FDFFFF, r, sizeof r); assert(m > 0);
    assert(r[0] >> 4 == 6 && r[6] == 58 && !memcmp(r + 8, FDFFFF, 16) && !memcmp(r + 24, FD1, 16) && r[40] == 1 && r[41] == 0);
    assert(fipsd_ipv6_build_dest_unreachable(zero, 20, FIPSD_ICMPV6_NO_ROUTE, FDFFFF, r, sizeof r) < 0);
    n = make_ipv6_packet(FD1, FD2, 17, zero, 2000, p);
    m = fipsd_ipv6_build_dest_unreachable(p, n, FIPSD_ICMPV6_NO_ROUTE, FDFFFF, r, sizeof r); assert(m > 0 && m <= 1280);
    assert(fipsd_ipv6_build_dest_unreachable(p, n, FIPSD_ICMPV6_NO_ROUTE, FDFFFF, r, 100) < 0); /* a short buffer is refused */
    /* test_build_packet_too_big */
    n = make_ipv6_packet(FD1, FD2, 17, zero, 1200, p);
    m = fipsd_ipv6_build_packet_too_big(p, n, 1070, FDFFFF, r, sizeof r); assert(m > 0 && m <= 1280);
    assert(r[0] >> 4 == 6 && r[6] == 58 && !memcmp(r + 8, FDFFFF, 16) && !memcmp(r + 24, FD1, 16) && r[40] == 2 && r[41] == 0 && be32(r + 44) == 1070);
    assert(fipsd_ipv6_build_packet_too_big(zero, 20, 1280, FDFFFF, r, sizeof r) < 0);
    n = make_ipv6_packet(FF02_1, FD2, 17, zero, 1200, p); assert(fipsd_ipv6_build_packet_too_big(p, n, 1280, FDFFFF, r, sizeof r) < 0);
    n = make_ipv6_packet(UNSPEC, FD2, 17, zero, 1200, p); assert(fipsd_ipv6_build_packet_too_big(p, n, 1280, FDFFFF, r, sizeof r) < 0);
    { uint8_t err[8] = {1, 0, 0, 0, 0, 0, 0, 0}; n = make_ipv6_packet(FD1, FD2, 58, err, 8, p); assert(fipsd_ipv6_build_packet_too_big(p, n, 1280, FDFFFF, r, sizeof r) < 0); }
    { uint8_t echo[8] = {128, 0, 0, 0, 0, 0, 0, 0}; n = make_ipv6_packet(FD1, FD2, 58, echo, 8, p); assert(fipsd_ipv6_build_packet_too_big(p, n, 1280, FDFFFF, r, sizeof r) > 0); }
    n = make_ipv6_packet(FD1, FD2, 17, zero, 2000, p); m = fipsd_ipv6_build_packet_too_big(p, n, 1070, FDFFFF, r, sizeof r); assert(m > 0 && m <= 1280);
    /* test_build_packet_too_big_remote_source_for_pmtud */
    n = make_ipv6_packet(FD41_1, FDDF_2, 6, zero, 1200, p);
    m = fipsd_ipv6_build_packet_too_big(p, n, 1203, FDDF_2, r, sizeof r); assert(m > 0);
    assert(!memcmp(r + 8, FDDF_2, 16) && !memcmp(r + 24, FD41_1, 16) && r[40] == 2 && r[41] == 0 && be32(r + 44) == 1203);
    assert(be16(r + 42) == fipsd_ipv6_icmpv6_checksum(r + 40, (size_t)m - 40, FDDF_2, FD41_1));
}

/* ===== tests/icmp.rs ===== */
static size_t packet_of(const uint8_t src[16], const uint8_t dst[16], size_t len, uint8_t *p)
{
    memset(p, 0, len); p[0] = 0x60; put16(p + 4, (uint16_t)(len - 40)); p[6] = 17; p[7] = 63; memcpy(p + 8, src, 16); memcpy(p + 24, dst, 16);
    return len;
}
static fipsd_ipv6_icmp_limiter limiter;
static size_t carrying(uint8_t first, const uint8_t *rest, size_t rest_len, uint8_t *p)
{
    packet_of(FD2, OUTSIDE, 1100, p); p[6] = first; memcpy(p + 40, rest, rest_len); return 1100;
}
/* Whether the packet draws Destination Unreachable, and Packet Too Big through a fresh limiter. */
static void answers(const uint8_t *p, size_t n, bool *unreachable, bool *too_big)
{
    uint8_t r[1300]; fipsd_ipv6_icmp_limiter_init(&limiter);
    *unreachable = fipsd_ipv6_unreachable(p, n, FD1, r, sizeof r) > 0;
    *too_big = fipsd_ipv6_too_big(p, n, 1033, &limiter, fipsd_host_os_clock().now(fipsd_host_os_clock().ctx), true, r, sizeof r) > 0;
}
typedef struct chain_case { const char *what; uint8_t first; uint8_t bytes[64]; size_t len; } chain_case;
static size_t chains(chain_case *out)
{
    const uint8_t options_icmp[8] = {58, 0, 1, 4, 0, 0, 0, 0}, options_60[8] = {60, 0, 1, 4, 0, 0, 0, 0}, options_43[8] = {43, 0, 1, 4, 0, 0, 0, 0};
    uint8_t routing[24] = {58, 2, 4, 1, 0, 0, 0, 0}; memset(routing + 8, 0xfd, 16);
    uint8_t auth[24] = {58, 4, 0, 0}; memset(auth + 4, 0x11, 20);
    const uint8_t fragment[8] = {58, 0, 0, 1, 0, 0, 0x12, 0x34};
    size_t n = 0;
    out[n++] = (chain_case){"no extension header", 58, {0}, 0};
    out[n] = (chain_case){"Hop-by-Hop", 0, {0}, 8}; memcpy(out[n++].bytes, options_icmp, 8);
    out[n] = (chain_case){"Destination Options", 60, {0}, 8}; memcpy(out[n++].bytes, options_icmp, 8);
    out[n] = (chain_case){"Routing", 43, {0}, 24}; memcpy(out[n++].bytes, routing, 24);
    out[n] = (chain_case){"Authentication", 51, {0}, 24}; memcpy(out[n++].bytes, auth, 24);
    out[n] = (chain_case){"a first Fragment", 44, {0}, 8}; memcpy(out[n++].bytes, fragment, 8);
    out[n] = (chain_case){"Hop-by-Hop, Destination Options, Routing", 0, {0}, 40};
    memcpy(out[n].bytes, options_60, 8); memcpy(out[n].bytes + 8, options_43, 8); memcpy(out[n].bytes + 16, routing, 24); ++n;
    return n;
}
static void icmp_answer_tests(void)
{
    static uint8_t over[1300], built[1300], got[1300];
    /* repeated_packet_too_big_to_one_source_is_held_back_once_its_burst_is_spent_with_or_without_a_queue */
    packet_of(FD1, FD2, 1100, over);
    int bn = fipsd_ipv6_build_packet_too_big(over, 1100, 1033, FD2, built, sizeof built); assert(bn > 0);
    fipsd_ipv6_icmp_limiter_init(&limiter);
    for (unsigned i = 0; i < FIPSD_IPV6_PTB_BURST / 2; ++i) assert(fipsd_ipv6_too_big(over, 1100, 1033, &limiter, ms(0), false, got, sizeof got) < 0);
    for (unsigned i = FIPSD_IPV6_PTB_BURST / 2; i < FIPSD_IPV6_PTB_BURST; ++i) {
        int gn = fipsd_ipv6_too_big(over, 1100, 1033, &limiter, ms(0), true, got, sizeof got); assert(gn == bn && !memcmp(got, built, (size_t)bn)); }
    assert(fipsd_ipv6_too_big(over, 1100, 1033, &limiter, ms(0), true, got, sizeof got) < 0);
    assert(fipsd_ipv6_too_big(over, 1100, 1033, &limiter, ms(99), true, got, sizeof got) < 0);
    assert(fipsd_ipv6_too_big(over, 1100, 1033, &limiter, ms(100), true, got, sizeof got) == bn);
    assert(fipsd_ipv6_too_big(over, 1100, 1033, &limiter, ms(150), true, got, sizeof got) < 0);
    /* unreachable_answers_from_our_address_and_not_a_packet_owed_no_error */
    { uint8_t uni[100], mc[100], a[1300], b[1300]; packet_of(FD2, OUTSIDE, 100, uni); packet_of(FF02_1, OUTSIDE, 100, mc);
      int an = fipsd_ipv6_unreachable(uni, 100, FD1, a, sizeof a), dn = fipsd_ipv6_build_dest_unreachable(uni, 100, FIPSD_ICMPV6_NO_ROUTE, FD1, b, sizeof b);
      assert(an > 0 && an == dn && !memcmp(a, b, (size_t)an)); assert(fipsd_ipv6_unreachable(mc, 100, FD1, a, sizeof a) < 0); }
    /* an_icmpv6_error_or_redirect_behind_any_extension_header_chain_draws_no_icmpv6_error;
     * an_echo_request_behind_any_extension_header_chain_still_draws_icmpv6_errors */
    chain_case cases[8]; size_t count = chains(cases); static const uint8_t kinds[] = {1, 2, 3, 4, 127, 137};
    for (size_t i = 0; i < count; ++i) {
        uint8_t rest[80]; memcpy(rest, cases[i].bytes, cases[i].len);
        for (unsigned k = 0; k < 6; ++k) {
            uint8_t icmp[8] = {kinds[k], 0, 0, 0, 0, 0, 0, 0}; memcpy(rest + cases[i].len, icmp, 8);
            bool u, t; size_t n = carrying(cases[i].first, rest, cases[i].len + 8, over); answers(over, n, &u, &t);
            if (u || t) { fprintf(stderr, "ICMPv6 type %u behind %s drew (%d,%d)\n", kinds[k], cases[i].what, u, t); abort(); }
        }
        uint8_t echo[8] = {128, 0, 0, 0, 0, 0, 0, 0}; memcpy(rest + cases[i].len, echo, 8);
        bool u, t; size_t n = carrying(cases[i].first, rest, cases[i].len + 8, over); answers(over, n, &u, &t);
        if (!u || !t) { fprintf(stderr, "Echo Request behind %s drew (%d,%d)\n", cases[i].what, u, t); abort(); }
    }
    /* a_later_fragment_of_an_icmpv6_message_draws_no_icmpv6_error_and_of_udp_does */
    { uint8_t later[8] = {58, 0, 0x05, 0xc9, 0, 0, 0x12, 0x34}; bool u, t; size_t n = carrying(44, later, 8, over); answers(over, n, &u, &t); assert(!u && !t);
      later[0] = 17; n = carrying(44, later, 8, over); answers(over, n, &u, &t); assert(u && t); }
    /* a_packet_whose_extension_headers_run_past_its_end_draws_no_icmpv6_error */
    { uint8_t p[100]; packet_of(FD2, OUTSIDE, 100, p); p[6] = 60; const uint8_t bad[8] = {17, 255, 1, 4, 0, 0, 0, 0}; memcpy(p + 40, bad, 8);
      bool u, t; answers(p, 100, &u, &t); assert(!u && !t); }
    /* packet_too_big_is_never_built_with_a_multicast_or_unspecified_source */
    { uint8_t to_group[1100], to_mesh[1100]; packet_of(FD1, FF02_1, 1100, to_group); packet_of(FD1, FD2, 1100, to_mesh);
      fipsd_ipv6_icmp_limiter_init(&limiter);
      assert(fipsd_ipv6_too_big(to_group, 1100, 1033, &limiter, ms(0), true, got, sizeof got) < 0);
      assert(fipsd_ipv6_build_packet_too_big(to_mesh, 1100, 1033, FF02_1, got, sizeof got) < 0);
      assert(fipsd_ipv6_build_packet_too_big(to_mesh, 1100, 1033, UNSPEC, got, sizeof got) < 0);
      assert(fipsd_ipv6_build_packet_too_big(to_mesh, 1100, 1033, FD2, got, sizeof got) > 0); }
}

/* ===== tests/icmp_rate_limit.rs ===== */
static void spend_burst(fipsd_ipv6_icmp_limiter *l, const uint8_t src[16], fips_instant at)
{ for (unsigned n = 0; n < FIPSD_IPV6_PTB_BURST; ++n) assert(fipsd_ipv6_icmp_limiter_check(l, src, at)); }
static void rate_limit_tests(void)
{
    static fipsd_ipv6_icmp_limiter l; uint8_t src[16], b[16], c[16]; addr("fd00::1", src); addr("fd00::2", b); addr("fd00::3", c);
    fipsd_ipv6_icmp_limiter_init(&l); spend_burst(&l, src, ms(0));   /* held back until one interval has passed and not after */
    assert(!fipsd_ipv6_icmp_limiter_check(&l, src, ms(0)) && !fipsd_ipv6_icmp_limiter_check(&l, src, ms(99)));
    assert(fipsd_ipv6_icmp_limiter_check(&l, src, ms(100)) && !fipsd_ipv6_icmp_limiter_check(&l, src, ms(199)) && fipsd_ipv6_icmp_limiter_check(&l, src, ms(200)));
    fipsd_ipv6_icmp_limiter_init(&l); spend_burst(&l, src, ms(0));   /* a held-back attempt does not restart the interval */
    assert(!fipsd_ipv6_icmp_limiter_check(&l, src, ms(60)) && fipsd_ipv6_icmp_limiter_check(&l, src, ms(100)));
    fipsd_ipv6_icmp_limiter_init(&l);                                /* an entry expires at its maximum age through a later check */
    assert(fipsd_ipv6_icmp_limiter_check(&l, src, ms(0)) && fipsd_ipv6_icmp_limiter_check(&l, b, ms(9999)) && fipsd_ipv6_icmp_limiter_len(&l) == 2);
    assert(fipsd_ipv6_icmp_limiter_check(&l, c, secs(10)) && fipsd_ipv6_icmp_limiter_len(&l) == 2);
    fipsd_ipv6_icmp_limiter_init_interval(&l, fips_duration_from_millis(50)); spend_burst(&l, src, ms(0)); /* a custom interval */
    assert(!fipsd_ipv6_icmp_limiter_check(&l, src, ms(0)) && !fipsd_ipv6_icmp_limiter_check(&l, src, ms(49)) && fipsd_ipv6_icmp_limiter_check(&l, src, ms(50)));
    fipsd_ipv6_icmp_limiter_init_interval(&l, (fips_duration){2, 0}); spend_burst(&l, src, ms(0)); /* kept past the maximum age until refilled */
    assert(fipsd_ipv6_icmp_limiter_check(&l, b, secs(10)) && fipsd_ipv6_icmp_limiter_len(&l) == 2);
    assert(!fipsd_ipv6_icmp_limiter_check(&l, src, ms(1999)) && fipsd_ipv6_icmp_limiter_check(&l, src, secs(2)));
    fipsd_ipv6_icmp_limiter_init_interval(&l, (fips_duration){0, 0}); spend_burst(&l, src, ns(0)); /* a zero interval refills within a microsecond */
    assert(!fipsd_ipv6_icmp_limiter_check(&l, src, ns(0)) && fipsd_ipv6_icmp_limiter_check(&l, src, ns(1000)));
    /* through the host clock: the first send is allowed; rapid sends draw the burst and no more than refills meanwhile */
    fips_clock clock = fipsd_host_os_clock();
    fipsd_ipv6_icmp_limiter_init(&l); assert(fipsd_ipv6_icmp_limiter_check(&l, src, clock.now(clock.ctx)));
    fipsd_ipv6_icmp_limiter_init(&l);
    { fips_instant start = clock.now(clock.ctx); unsigned allowed = 0;
      for (unsigned i = 0; i < 100; ++i) allowed += fipsd_ipv6_icmp_limiter_check(&l, src, clock.now(clock.ctx));
      fips_duration elapsed = start.ops->saturating_duration_since(clock.now(clock.ctx), start);
      unsigned refilled = (unsigned)(((double)elapsed.secs + (double)elapsed.nanos / 1e9) * 10.0) + 1;
      assert(allowed >= FIPSD_IPV6_PTB_BURST && allowed <= FIPSD_IPV6_PTB_BURST + refilled); }
    fipsd_ipv6_icmp_limiter_init(&l); spend_burst(&l, src, ms(0));   /* each source has a bucket of its own */
    assert(!fipsd_ipv6_icmp_limiter_check(&l, src, ms(0))); spend_burst(&l, b, ms(0)); assert(!fipsd_ipv6_icmp_limiter_check(&l, b, ms(0)));
    fipsd_ipv6_icmp_limiter_init(&l);                                /* cleanup removes old entries, preserves recent ones */
    { fips_instant now = clock.now(clock.ctx), future;
      assert(fipsd_ipv6_icmp_limiter_check(&l, src, now) && fipsd_ipv6_icmp_limiter_check(&l, b, now) && fipsd_ipv6_icmp_limiter_len(&l) == 2);
      assert(now.ops->checked_add(now, (fips_duration){11, 0}, &future) == FIPS_OK);
      fipsd_ipv6_icmp_limiter_cleanup(&l, future); assert(fipsd_ipv6_icmp_limiter_len(&l) == 0);
      fipsd_ipv6_icmp_limiter_init(&l); assert(fipsd_ipv6_icmp_limiter_check(&l, src, now) && fipsd_ipv6_icmp_limiter_len(&l) == 1);
      fipsd_ipv6_icmp_limiter_cleanup(&l, clock.now(clock.ctx)); assert(fipsd_ipv6_icmp_limiter_len(&l) == 1); }
    fipsd_ipv6_icmp_limiter_init(&l);                                /* a second 1 ms after the first is allowed */
    assert(fipsd_ipv6_icmp_limiter_check(&l, src, ms(0)) && fipsd_ipv6_icmp_limiter_check(&l, src, ms(1)));
    fipsd_ipv6_icmp_limiter_init(&l);                                /* a burst of five after ten quiet seconds is answered for every packet */
    assert(fipsd_ipv6_icmp_limiter_check(&l, src, ms(0)));
    { unsigned allowed = 0; for (unsigned i = 0; i < 5; ++i) allowed += fipsd_ipv6_icmp_limiter_check(&l, src, secs(10)); assert(allowed == 5); }
    fipsd_ipv6_icmp_limiter_init(&l);                                /* a flood is held to ten at once and about ten more a second */
    { unsigned at_once = 0; for (unsigned i = 0; i < 1000; ++i) at_once += fipsd_ipv6_icmp_limiter_check(&l, src, ms(0)); assert(at_once >= 1 && at_once <= 10);
      fipsd_ipv6_icmp_limiter_init(&l); unsigned over = 0; for (unsigned i = 0; i < 1000; ++i) over += fipsd_ipv6_icmp_limiter_check(&l, src, ms(i));
      assert(over >= 10 && over <= 21); }
    /* the host's bound: the 257th source evicts the one checked longest ago, no more */
    fipsd_ipv6_icmp_limiter_init(&l);
    for (unsigned i = 0; i < FIPSD_IPV6_ICMP_LIMITER_SOURCES + 1; ++i) { uint8_t s[16] = {0xfd}; s[14] = (uint8_t)(i >> 8); s[15] = (uint8_t)i;
        assert(fipsd_ipv6_icmp_limiter_check(&l, s, ms(i))); }
    assert(fipsd_ipv6_icmp_limiter_len(&l) == FIPSD_IPV6_ICMP_LIMITER_SOURCES);
}

/* ===== tests/inbound.rs ===== */
static fips_node_addr key_n(uint8_t k) { fips_node_addr a; for (unsigned i = 0; i < 16; ++i) a.bytes[i] = (uint8_t)(k * 37 + i * 11 + 1); return a; }
static void inbound_tests(void)
{
    uint8_t ect0[4] = {0x60, 0x20, 0, 0}, not_ect[4] = {0x60, 0, 0, 0}, shrt[1] = {0x60};
    fipsd_ipv6_mark_ce(ect0, 4); assert((ect0[1] & 0x30) == 0x30);
    fipsd_ipv6_mark_ce(not_ect, 4); assert((not_ect[1] & 0x30) == 0);
    fipsd_ipv6_mark_ce(shrt, 1); assert(shrt[0] == 0x60);
    /* a_delivered_packet_is_rebuilt_between_the_sender_and_us_and_marked_only_when_the_datagram_was */
    uint8_t ect[100], payload[100], plain[120], marked[120]; memset(ect, 0, 100); ect[0] = 0x60; ect[1] = 0x20; put16(ect + 4, 60); ect[6] = 17; ect[7] = 64;
    for (unsigned i = 40; i < 100; ++i) ect[i] = (uint8_t)(i - 40);
    int pn = fipsd_ipv6_compress(ect, 100, payload, sizeof payload); assert(pn > 0);
    uint8_t source[16] = {0xfd, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, dest[16]; memset(dest, 9, 16); dest[0] = 0xfd;
    fipsd_ipv6_deliver d = {.peer = key_n(1), .payload = payload, .len = (size_t)pn, .ce = false};
    int n = fipsd_ipv6_unpack(&d, source, dest, plain, sizeof plain); assert(n == 100);
    assert(!memcmp(plain + 8, source, 16) && !memcmp(plain + 24, dest, 16) && (plain[1] & 0x30) == 0x20 && !memcmp(plain + 40, ect + 40, 60));
    d.ce = true; n = fipsd_ipv6_unpack(&d, source, dest, marked, sizeof marked); assert(n == 100);
    assert((marked[1] & 0x30) == 0x30 && !memcmp(marked + 2, plain + 2, 98));
    /* a_payload_that_is_not_a_compressed_packet_is_not_delivered */
    { uint8_t bad[3] = {0x60, 0x60, 0x60}, fd[16]; memset(fd, 0xfd, 16); fipsd_ipv6_deliver x = {.peer = key_n(2), .payload = bad, .len = 3, .ce = true};
      assert(fipsd_ipv6_unpack(&x, fd, fd, plain, sizeof plain) < 0); }
}

/* ===== tests/queue.rs ===== */
static void queue_tests(void)
{
    fipsd_ipv6_queue *q = fipsd_ipv6_queue_create(2);
    for (uint8_t b = 0; b < 5; ++b) assert(fipsd_ipv6_queue_send(q, &b, 1) == 0);  /* a full queue drops and counts without waiting */
    fipsd_ipv6_packet *p = fipsd_ipv6_queue_try_receive(q); assert(p && p->len == 1 && p->bytes[0] == 0); free(p);
    p = fipsd_ipv6_queue_try_receive(q); assert(p && p->bytes[0] == 1); free(p);
    assert(!fipsd_ipv6_queue_try_receive(q) && fipsd_ipv6_queue_dropped(q) == 3);
    fipsd_ipv6_queue_destroy(q);
    q = fipsd_ipv6_queue_create(1);                                                /* a queue with room again takes packets and keeps its count */
    { uint8_t one = 1, two = 2, three = 3; assert(fipsd_ipv6_queue_send(q, &one, 1) == 0 && fipsd_ipv6_queue_send(q, &two, 1) == 0);
      p = fipsd_ipv6_queue_try_receive(q); assert(p && p->bytes[0] == 1); free(p);
      assert(fipsd_ipv6_queue_send(q, &three, 1) == 0); p = fipsd_ipv6_queue_try_receive(q); assert(p && p->bytes[0] == 3); free(p);
      assert(fipsd_ipv6_queue_dropped(q) == 1 && *fipsd_ipv6_queue_counter(q) == 1); }
    fipsd_ipv6_queue_destroy(q);
    q = fipsd_ipv6_queue_create(4); fipsd_ipv6_queue_close(q);                      /* a queue whose reader has gone is an error and counts nothing */
    { uint8_t seven = 7; assert(fipsd_ipv6_queue_send(q, &seven, 1) == -1 && fipsd_ipv6_queue_dropped(q) == 0);
      /* a new reader reopens it: what the last drain left is gone, sends are taken again */
      fipsd_ipv6_queue_open(q); assert(fipsd_ipv6_queue_send(q, &seven, 1) == 0);
      fipsd_ipv6_queue_close(q); fipsd_ipv6_queue_open(q); assert(!fipsd_ipv6_queue_try_receive(q) && fipsd_ipv6_queue_dropped(q) == 0); }
    fipsd_ipv6_queue_destroy(q);
    q = fipsd_ipv6_queue_create(1);                                                /* warns at the first drop and each doubling: nine dropped of ten */
    for (uint8_t b = 0; b < 10; ++b) assert(fipsd_ipv6_queue_send(q, &b, 1) == 0);
    assert(fipsd_ipv6_queue_dropped(q) == 9);
    fipsd_ipv6_queue_destroy(q); /* frees the packet still queued */
}

/* ===== outbound.rs, tests/outbound.rs ===== */
static size_t packet(const uint8_t src[16], const uint8_t dst[16], size_t len, uint8_t *p)
{
    memset(p, 0, len); p[0] = 0x6a; p[1] = 0xbc; p[2] = 0xde; p[3] = 0xf1; put16(p + 4, (uint16_t)(len - 40)); p[6] = 17; p[7] = 63;
    memcpy(p + 8, src, 16); memcpy(p + 24, dst, 16);
    for (size_t i = 40; i < len; ++i) p[i] = (uint8_t)((i - 40) * 7);
    return len;
}
static int from_us(const uint8_t *p, size_t n, uint8_t *out, size_t cap) { int m = fipsd_ipv6_build_dest_unreachable(p, n, FIPSD_ICMPV6_NO_ROUTE, FD1, out, cap); assert(m > 0); return m; }
static fipsd_ipv6_mesh_state state;
static void state_with(const fips_node_addr *peer, uint16_t limit)
{
    assert(fipsd_ipv6_mesh_state_init(&state, 16, 16) == 0);
    fipsd_ipv6_from_mesh m = {.kind = FIPSD_IPV6_MESH_NODE_MTU, .payload_limit = limit}; fipsd_ipv6_mesh_state_apply(&state, &m, NULL);
    fipsd_ipv6_from_mesh k = {.kind = FIPSD_IPV6_MESH_KNOWN_KEY, .peer = *peer}; fipsd_ipv6_mesh_state_apply(&state, &k, NULL);
}
static void address_of(const fips_node_addr *k, uint8_t out[16]) { fips_ipv6_address a = fipsd_ipv6_peer_address(k); memcpy(out, a.bytes, 16); }
static uint8_t scratch[4096];
/* The packets answered for a refusal through a fresh limiter, concatenated with lengths. */
typedef struct answered { uint8_t bytes[8][1300]; size_t len[8]; size_t count; fipsd_ipv6_icmp_limiter limiter; } answered;
static void collect(void *user, const fipsd_ipv6_owed *o)
{
    answered *a = user; assert(a->count < 8);
    if (o->kind == FIPSD_IPV6_OWED_UNREACHABLE) { memcpy(a->bytes[a->count], o->packet, o->len); a->len[a->count++] = o->len; return; }
    int n = fipsd_ipv6_too_big(o->packet, o->len, o->mtu, &a->limiter, fipsd_host_os_clock().now(fipsd_host_os_clock().ctx), true, a->bytes[a->count], 1300);
    if (n > 0) a->len[a->count++] = (size_t)n;
}
/* The last answer owed, as the refusal's own verdict before any limiter. */
static struct owed_seen { unsigned count; fipsd_ipv6_owed_kind kind; uint32_t mtu; size_t len; uint8_t packet[2048]; } owed_seen;
static void see_owed(void *user, const fipsd_ipv6_owed *o)
{ (void)user; ++owed_seen.count; owed_seen.kind = o->kind; owed_seen.mtu = o->mtu; owed_seen.len = o->len; memcpy(owed_seen.packet, o->packet, o->len < 2048 ? o->len : 2048); }
static void answer_into(answered *a, const fipsd_ipv6_refusal *r) { fipsd_ipv6_icmp_limiter_init(&a->limiter); a->count = 0; fipsd_ipv6_answer(r, FD1, scratch, sizeof scratch, collect, a); }
static size_t syn(const uint8_t src[16], const uint8_t dst[16], uint8_t *p)
{
    memset(p, 0, 64); p[0] = 0x60; put16(p + 4, 24); p[6] = 6; p[7] = 64; memcpy(p + 8, src, 16); memcpy(p + 24, dst, 16);
    p[52] = 6 << 4; p[53] = 0x02; const uint8_t o[4] = {2, 4, 0x05, 0xa0}; memcpy(p + 60, o, 4); return 64;
}
static uint16_t tcp_sum(const uint8_t *packet_, size_t len, size_t at)
{
    uint32_t sum = 0;
    for (size_t i = 8; i < 40; i += 2) sum += be16(packet_ + i);
    sum += (uint32_t)(len - at) + 6;
    for (size_t i = at; i < len; i += 2) sum += i + 1 < len ? be16(packet_ + i) : (uint32_t)packet_[i] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)sum;
}
/* A SYN advertising MSS 1143 from us to dst behind the 8-byte extension header ext of type nh, with a valid checksum. */
static size_t syn_behind(const uint8_t dst[16], uint8_t nh, const uint8_t ext[8], uint8_t *out)
{
    uint8_t plain[64]; syn(FD1, dst, plain);
    memcpy(out, plain, 40); memcpy(out + 40, ext, 8); memcpy(out + 48, plain + 40, 24);
    put16(out + 4, 32); out[6] = nh; put16(out + 48 + 22, 1143); out[48 + 16] = 0; out[48 + 17] = 0;
    put16(out + 48 + 16, (uint16_t)~tcp_sum(out, 72, 48)); assert(tcp_sum(out, 72, 48) == 0xffff);
    return 72;
}
/* The packet a host reassembles from fragments of one datagram, or 0 when they are not a complete set. */
static size_t reassemble(uint8_t (*f)[1300], const size_t *len, size_t count, uint8_t *out)
{
    size_t data = 0; uint8_t joined[2048];
    for (size_t i = 0; i < count; ++i) {
        if (len[i] < 48 || f[i][6] != 44 || memcmp(f[i] + 8, f[0] + 8, 32) || memcmp(f[i] + 44, f[0] + 44, 4)) return 0;
        uint16_t om = be16(f[i] + 42);
        if ((size_t)(om & 0xfff8u) != data || ((om & 1) == 1) != (i + 1 < count)) return 0;
        memcpy(joined + data, f[i] + 48, len[i] - 48); data += len[i] - 48;
    }
    memcpy(out, f[0], 40); put16(out + 4, (uint16_t)data); out[6] = f[0][40]; memcpy(out + 40, joined, data);
    return 40 + data;
}
static void outbound_tests(void)
{
    static uint8_t sent[2048], read_[2048], payload[2048], a[1300], b[1300];
    fips_node_addr peer = key_n(3), stranger_key = key_n(4); uint8_t peer_addr[16], stranger[16]; address_of(&peer, peer_addr); address_of(&stranger_key, stranger);
    fipsd_ipv6_host h; fipsd_ipv6_from_tun v;
    /* admit_* (outbound.rs unit tests) */
    { uint8_t s39[39]; memset(s39, 0x60, 39); assert(fipsd_ipv6_admit_packet(s39, 39, 1280).kind == FIPSD_IPV6_ADMIT_DROP);
      packet(FD1, peer_addr, 60, sent); sent[0] = 0x45; assert(fipsd_ipv6_admit_packet(sent, 60, 1280).kind == FIPSD_IPV6_ADMIT_DROP);
      packet(FD1, peer_addr, 1281, sent); fipsd_ipv6_admit ad = fipsd_ipv6_admit_packet(sent, 1281, 1280); assert(ad.kind == FIPSD_IPV6_ADMIT_TOO_BIG && ad.mtu == 1280);
      packet(FD1, peer_addr, 1280, sent); assert(fipsd_ipv6_admit_packet(sent, 1280, 1280).kind == FIPSD_IPV6_ADMIT_FORWARD);
      packet(FD1, peer_addr, 60, sent); ad = fipsd_ipv6_admit_packet(sent, 60, 1280);
      assert(ad.kind == FIPSD_IPV6_ADMIT_FORWARD && !memcmp(ad.prefix.bytes, peer_addr + 1, 15)); }
    /* a_packet_to_a_known_key_becomes_a_datagram_carrying_the_compressed_packet */
    state_with(&peer, 1400); packet(FD1, peer_addr, 300, sent);
    fipsd_ipv6_outbound(sent, 300, &state, FD1, scratch, sizeof scratch, &h);
    { int pn = fipsd_ipv6_compress(sent, 300, payload, sizeof payload);
      assert(h.kind == FIPSD_IPV6_HOST_MESH && !memcmp(h.datagram.peer.bytes, peer.bytes, 16) && (int)h.datagram.len == pn && !memcmp(h.datagram.payload, payload, (size_t)pn)); }
    /* a_packet_to_a_prefix_no_key_names_is_answered_unreachable_from_the_raw_packet */
    packet(FD1, stranger, 80, sent); fipsd_ipv6_outbound(sent, 80, &state, FD1, scratch, sizeof scratch, &h);
    { int n = from_us(sent, 80, a, sizeof a); assert(h.kind == FIPSD_IPV6_HOST_UNREACHABLE && (int)h.response_len == n && !memcmp(h.response, a, (size_t)n)); }
    fipsd_ipv6_mesh_state_destroy(&state);
    /* a_packet_over_the_published_limit_to_an_unknown_destination_is_answered_packet_too_big */
    state_with(&peer, 1000); { uint16_t mtu = fipsd_ipv6_mesh_state_ipv6_mtu(&state); assert(mtu == 1033);
      packet(FD1, stranger, mtu + 1u, sent); fipsd_ipv6_outbound(sent, mtu + 1u, &state, FD1, scratch, sizeof scratch, &h); assert(h.kind == FIPSD_IPV6_HOST_TOO_BIG && h.mtu == mtu);
      packet(FD1, stranger, mtu, read_); fipsd_ipv6_outbound(read_, mtu, &state, FD1, scratch, sizeof scratch, &h);
      int n = from_us(read_, mtu, a, sizeof a); assert(h.kind == FIPSD_IPV6_HOST_UNREACHABLE && (int)h.response_len == n && !memcmp(h.response, a, (size_t)n));
      fipsd_ipv6_icmp_limiter_init(&limiter);
      int tn = fipsd_ipv6_too_big(sent, mtu + 1u, mtu, &limiter, ms(0), true, a, sizeof a), bn = fipsd_ipv6_build_packet_too_big(sent, mtu + 1u, mtu, stranger, b, sizeof b);
      assert(tn > 0 && tn == bn && !memcmp(a, b, (size_t)tn)); }
    /* a_packet_to_a_known_key_goes_to_the_node_whatever_its_size */
    { size_t over = fipsd_ipv6_mesh_state_ipv6_mtu(&state) + 1u; packet(FD1, peer_addr, over, sent); fipsd_ipv6_outbound(sent, over, &state, FD1, scratch, sizeof scratch, &h);
      int pn = fipsd_ipv6_compress(sent, over, payload, sizeof payload); assert(h.kind == FIPSD_IPV6_HOST_MESH && (int)h.datagram.len == pn && !memcmp(h.datagram.payload, payload, (size_t)pn)); }
    fipsd_ipv6_mesh_state_destroy(&state);
    /* every_refusal_of_a_packet_sent_from_our_address_is_answered_byte_for_byte_as_before */
    { static const size_t lens[] = {40, 41, 80, 1200, 1500}; static answered got, expected;
      for (unsigned i = 0; i < 5; ++i) {
          size_t len = lens[i]; packet(FD1, peer_addr, len, sent); int pn = fipsd_ipv6_compress(sent, len, payload, sizeof payload); assert(pn > 0);
          uint32_t mtu = len > 1280 ? 1280 : 1033; expected.count = 0; fipsd_ipv6_icmp_limiter_init(&expected.limiter);
          int tn = fipsd_ipv6_too_big(sent, len, mtu, &expected.limiter, ms(0), true, expected.bytes[0], 1300); assert(tn > 0); expected.len[expected.count++] = (size_t)tn;
          for (unsigned k = 0; k < 3; ++k) { expected.len[expected.count] = (size_t)from_us(sent, len, expected.bytes[expected.count], 1300); ++expected.count; }
          assert(expected.count == 4);
          const uint8_t *payloads[2] = {payload, payload}; size_t plens[2] = {(size_t)pn, (size_t)pn};
          static answered part; got.count = 0;
          fipsd_ipv6_refusal too_big = {.kind = FIPSD_IPV6_REFUSAL_TOO_BIG, .peer = peer, .payload_limit = 1000, .payloads = payloads, .lens = plens, .count = 1};
          fipsd_ipv6_refusal table_full = {.kind = FIPSD_IPV6_REFUSAL_TABLE_FULL, .peer = peer, .payloads = payloads, .lens = plens, .count = 1};
          fipsd_ipv6_refusal no_route = {.kind = FIPSD_IPV6_REFUSAL_NO_ROUTE, .peer = peer, .payloads = payloads, .lens = plens, .count = 2};
          const fipsd_ipv6_refusal *refusals[3] = {&too_big, &table_full, &no_route};
          for (unsigned r = 0; r < 3; ++r) { answer_into(&part, refusals[r]); for (size_t j = 0; j < part.count; ++j) { memcpy(got.bytes[got.count], part.bytes[j], part.len[j]); got.len[got.count++] = part.len[j]; } }
          assert(got.count == 4);
          for (unsigned j = 0; j < 4; ++j) assert(got.len[j] == expected.len[j] && !memcmp(got.bytes[j], expected.bytes[j], got.len[j]));
      } }
    /* a_refused_packet_from_a_foreign_source_is_answered_as_if_sent_from_our_address */
    { uint8_t foreign[16]; addr("fd00::99", foreign); packet(foreign, peer_addr, 80, sent); int pn = fipsd_ipv6_compress(sent, 80, payload, sizeof payload);
      const uint8_t *payloads[1] = {payload}; size_t plens[1] = {(size_t)pn}; static answered got;
      fipsd_ipv6_refusal r = {.kind = FIPSD_IPV6_REFUSAL_TABLE_FULL, .peer = peer, .payloads = payloads, .lens = plens, .count = 1}; answer_into(&got, &r);
      assert(got.count == 1 && !memcmp(got.bytes[0] + 24, FD1, 16) && !memcmp(got.bytes[0] + 48 + 8, FD1, 16)); }
    /* packet_too_big_from_a_refusal_reports_the_ipv6_mtu_of_its_payload_limit */
    { packet(FD1, peer_addr, 1250, sent); int pn = fipsd_ipv6_compress(sent, 1250, payload, sizeof payload);
      const uint8_t *payloads[1] = {payload}; size_t plens[1] = {(size_t)pn}; static answered got;
      fipsd_ipv6_refusal r = {.kind = FIPSD_IPV6_REFUSAL_TOO_BIG, .peer = peer, .payload_limit = 1170, .payloads = payloads, .lens = plens, .count = 1}; answer_into(&got, &r);
      assert(got.count == 1 && be32(got.bytes[0] + 44) == 1203); }
    /* a_refused_packet_to_a_non_fd_destination_or_with_a_wrong_length_field_is_quoted_as_rebuilt */
    { uint8_t odd[16]; memcpy(odd, peer_addr, 16); odd[0] = 0xfe;
      for (unsigned which = 0; which < 2; ++which) {
          if (which == 0) packet(FD1, odd, 80, sent); else { packet(FD1, peer_addr, 80, sent); put16(sent + 4, 7); }
          int pn = fipsd_ipv6_compress(sent, 80, payload, sizeof payload); const uint8_t *payloads[1] = {payload}; size_t plens[1] = {(size_t)pn}; static answered got;
          fipsd_ipv6_refusal r = {.kind = FIPSD_IPV6_REFUSAL_TABLE_FULL, .peer = peer, .payloads = payloads, .lens = plens, .count = 1}; answer_into(&got, &r);
          int tn = fipsd_ipv6_unreachable(sent, 80, FD1, a, sizeof a); assert(got.count == 1 && tn > 0);
          size_t fs = which == 0 ? 24 : 4, fe = which == 0 ? 40 : 6;
          assert(!memcmp(a + 48 + fs, sent + fs, fe - fs)); /* as sent */
          if (which == 0) assert(!memcmp(got.bytes[0] + 48 + 24, peer_addr, 16)); else assert(be16(got.bytes[0] + 48 + 4) == 40); /* as rebuilt */
          assert(got.len[0] != (size_t)tn || memcmp(got.bytes[0], a, (size_t)tn) != 0);
      } }
    /* the_system_tun_path_filters_hairpins_and_clamps_where_the_shared_step_does_not */
    state_with(&peer, 1400); fipsd_ipv6_paths *paths = fipsd_ipv6_mesh_state_paths(&state);
    assert(fipsd_ipv6_paths_set(paths, peer_addr, fips_framing_payload_limit(1000)));
    { packet(FD1, OUTSIDE, 80, sent); memcpy(read_, sent, 80); fipsd_ipv6_from_tun_decide(read_, 80, 1360, paths, &state, FD1, scratch, sizeof scratch, &v);
      int n = from_us(sent, 80, a, sizeof a); assert(v.kind == FIPSD_IPV6_FROM_TUN_OFF_MESH && (int)v.response_len == n && !memcmp(v.response, a, (size_t)n));
      fipsd_ipv6_outbound(sent, 80, &state, FD1, scratch, sizeof scratch, &h); assert(h.kind == FIPSD_IPV6_HOST_UNREACHABLE && (int)h.response_len == n && !memcmp(h.response, a, (size_t)n)); }
    { syn(FD1, FD1, sent); uint8_t looped[64], checksummed[64]; memcpy(looped, sent, 64); memcpy(checksummed, sent, 64);
      fipsd_ipv6_from_tun_decide(looped, 64, 1360, paths, &state, FD1, scratch, sizeof scratch, &v); assert(v.kind == FIPSD_IPV6_FROM_TUN_HAIRPIN);
      fipsd_ipv6_recalculate_l4_checksum(checksummed, 64); assert(memcmp(checksummed, sent, 64) != 0 && !memcmp(looped, checksummed, 64));
      fipsd_ipv6_outbound(sent, 64, &state, FD1, scratch, sizeof scratch, &h); int n = from_us(sent, 64, a, sizeof a);
      assert(h.kind == FIPSD_IPV6_HOST_UNREACHABLE && (int)h.response_len == n && !memcmp(h.response, a, (size_t)n)); }
    { syn(FD1, peer_addr, sent); uint8_t clamped[64]; memcpy(clamped, sent, 64);
      fipsd_ipv6_from_tun_decide(clamped, 64, 1360, paths, &state, FD1, scratch, sizeof scratch, &v);
      assert(v.kind == FIPSD_IPV6_FROM_TUN_HOST && v.clamped && v.clamped_mss == 863 && v.host.kind == FIPSD_IPV6_HOST_MESH && be16(clamped + 62) == 863);
      int pn = fipsd_ipv6_compress(clamped, 64, payload, sizeof payload); assert((int)v.host.datagram.len == pn && !memcmp(v.host.datagram.payload, payload, (size_t)pn));
      fipsd_ipv6_outbound(sent, 64, &state, FD1, scratch, sizeof scratch, &h); pn = fipsd_ipv6_compress(sent, 64, payload, sizeof payload);
      assert(h.kind == FIPSD_IPV6_HOST_MESH && (int)h.datagram.len == pn && !memcmp(h.datagram.payload, payload, (size_t)pn)); }
    /* a_packet_that_is_not_ipv6_gets_nothing_on_either_path */
    { const uint8_t *dsts[2] = {FD1, peer_addr}; fipsd_ipv6_paths_clear(paths);
      for (unsigned i = 0; i < 2; ++i) { packet(FD1, dsts[i], 60, sent); sent[0] = 0x45; memcpy(read_, sent, 60);
          fipsd_ipv6_from_tun_decide(read_, 60, 1360, paths, &state, FD1, scratch, sizeof scratch, &v); assert(v.kind == FIPSD_IPV6_FROM_TUN_NOTHING && !memcmp(read_, sent, 60));
          fipsd_ipv6_outbound(sent, 60, &state, FD1, scratch, sizeof scratch, &h); assert(h.kind == FIPSD_IPV6_HOST_NOTHING); } }
    /* a_refusal_of_several_payloads_is_owed_one_answer_each_in_order */
    { uint8_t first[80], second[120], p1[80], p2[120]; packet(FD1, peer_addr, 80, first); packet(FD1, peer_addr, 120, second);
      int n1 = fipsd_ipv6_compress(first, 80, p1, sizeof p1), n2 = fipsd_ipv6_compress(second, 120, p2, sizeof p2);
      const uint8_t *payloads[2] = {p1, p2}; size_t plens[2] = {(size_t)n1, (size_t)n2}; static answered got;
      fipsd_ipv6_refusal r = {.kind = FIPSD_IPV6_REFUSAL_NO_ROUTE, .peer = peer, .payloads = payloads, .lens = plens, .count = 2}; answer_into(&got, &r);
      int e1 = from_us(first, 80, a, sizeof a), e2 = from_us(second, 120, b, sizeof b);
      assert(got.count == 2 && got.len[0] == (size_t)e1 && !memcmp(got.bytes[0], a, (size_t)e1) && got.len[1] == (size_t)e2 && !memcmp(got.bytes[1], b, (size_t)e2));
      /* the TooBig answer is owed on the rebuilt packet with the limit's IPv6 MTU: Owed::TooBig(first, 1033) */
      fipsd_ipv6_refusal tb = {.kind = FIPSD_IPV6_REFUSAL_TOO_BIG, .peer = peer, .payload_limit = 1000, .payloads = payloads, .lens = plens, .count = 1};
      owed_seen = (struct owed_seen){0}; assert(fipsd_ipv6_answer(&tb, FD1, scratch, sizeof scratch, see_owed, NULL) == 1);
      assert(owed_seen.count == 1 && owed_seen.kind == FIPSD_IPV6_OWED_TOO_BIG && owed_seen.mtu == 1033 && owed_seen.len == 80 && !memcmp(owed_seen.packet, first, 80)); }
    /* a_packet_from_a_source_other_than_our_address_is_answered_source_policy_and_not_forwarded;
     * a_packet_from_the_unspecified_address_or_a_multicast_source_is_refused_unanswered */
    { uint8_t srcs[2][16]; addr("2001:db8::5", srcs[0]); addr("fd00::5", srcs[1]); const uint8_t *dsts[2] = {peer_addr, stranger};
      for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 2; ++j) {
          packet(srcs[i], dsts[j], 80, sent); int rn = fipsd_ipv6_build_dest_unreachable(sent, 80, FIPSD_ICMPV6_SOURCE_POLICY, FD1, a, sizeof a); assert(rn > 0);
          memcpy(read_, sent, 80); fipsd_ipv6_from_tun_decide(read_, 80, 1360, paths, &state, FD1, scratch, sizeof scratch, &v);
          assert(v.kind == FIPSD_IPV6_FROM_TUN_HOST && !v.clamped && v.host.kind == FIPSD_IPV6_HOST_UNREACHABLE && (int)v.host.response_len == rn && !memcmp(v.host.response, a, (size_t)rn));
          fipsd_ipv6_outbound(sent, 80, &state, FD1, scratch, sizeof scratch, &h); assert(h.kind == FIPSD_IPV6_HOST_UNREACHABLE && (int)h.response_len == rn && !memcmp(h.response, a, (size_t)rn));
      }
      const uint8_t *bad[2] = {UNSPEC, FF02_1};
      for (unsigned i = 0; i < 2; ++i) { packet(bad[i], peer_addr, 80, sent); memcpy(read_, sent, 80);
          fipsd_ipv6_from_tun_decide(read_, 80, 1360, paths, &state, FD1, scratch, sizeof scratch, &v);
          assert(v.kind == FIPSD_IPV6_FROM_TUN_HOST && !v.clamped && v.host.kind == FIPSD_IPV6_HOST_NOTHING);
          fipsd_ipv6_outbound(sent, 80, &state, FD1, scratch, sizeof scratch, &h); assert(h.kind == FIPSD_IPV6_HOST_NOTHING); } }
    fipsd_ipv6_mesh_state_destroy(&state);
    /* an_oversized_packet_to_a_destination_outside_the_mesh_draws_no_packet_too_big */
    state_with(&stranger_key, 1000); { size_t over = fipsd_ipv6_mesh_state_ipv6_mtu(&state) + 1u; uint8_t groups[2][16]; addr("ff02::1", groups[0]); addr("ff0e::1:3", groups[1]);
      for (unsigned i = 0; i < 2; ++i) { packet(FD1, groups[i], over, sent); fipsd_ipv6_outbound(sent, over, &state, FD1, scratch, sizeof scratch, &h); assert(h.kind == FIPSD_IPV6_HOST_NOTHING); }
      packet(FD1, OUTSIDE, over, sent); fipsd_ipv6_outbound(sent, over, &state, FD1, scratch, sizeof scratch, &h); int n = from_us(sent, over, a, sizeof a);
      assert(h.kind == FIPSD_IPV6_HOST_UNREACHABLE && (int)h.response_len == n && !memcmp(h.response, a, (size_t)n)); }
    fipsd_ipv6_mesh_state_destroy(&state);
    /* a_syn_behind_a_hop_by_hop_header_is_clamped_to_the_stored_path_as_a_plain_syn_is */
    state_with(&peer, 1400); paths = fipsd_ipv6_mesh_state_paths(&state); assert(fipsd_ipv6_paths_set(paths, peer_addr, fips_framing_payload_limit(1000)));
    { uint8_t plain[64]; syn(FD1, peer_addr, plain); memcpy(sent, plain, 40); const uint8_t hbh[8] = {6, 0, 1, 4, 0, 0, 0, 0}; memcpy(sent + 40, hbh, 8); memcpy(sent + 48, plain + 40, 24);
      put16(sent + 4, 32); sent[6] = 0; sent[64] = 0; sent[65] = 0; put16(sent + 64, (uint16_t)~tcp_sum(sent, 72, 48)); assert(tcp_sum(sent, 72, 48) == 0xffff);
      fipsd_ipv6_from_tun_decide(sent, 72, 1360, paths, &state, FD1, scratch, sizeof scratch, &v);
      assert(be16(sent + 70) == 863 && tcp_sum(sent, 72, 48) == 0xffff && v.kind == FIPSD_IPV6_FROM_TUN_HOST && v.clamped && v.host.kind == FIPSD_IPV6_HOST_MESH);
      int pn = fipsd_ipv6_compress(sent, 72, payload, sizeof payload); assert((int)v.host.datagram.len == pn && !memcmp(v.host.datagram.payload, payload, (size_t)pn)); }
    fipsd_ipv6_mesh_state_destroy(&state);
    /* a_compressed_1280_byte_payload_splits_into_compressed_fragments_that_each_fit_the_payload_limit */
    { static fipsd_ipv6_fragments frags; static uint8_t rebuilt[2][1300], back[2048]; size_t rl[2];
      packet(FD1, peer_addr, 1280, sent); int pn = fipsd_ipv6_compress(sent, 1280, payload, sizeof payload);
      assert(fipsd_ipv6_split(payload, (size_t)pn, &peer, FD1, 1090, 0x01020304, &frags) && frags.count == 2 && frags.len[0] == 1087 && frags.len[1] == 183);
      for (size_t i = 0; i < 2; ++i) { assert(frags.bytes[i][0] == 0x00 && frags.bytes[i][5] == 44);
          int rn = fipsd_ipv6_decompress(frags.bytes[i], frags.len[i], FD1, peer_addr, rebuilt[i], 1300); assert(rn > 0); rl[i] = (size_t)rn; }
      assert(reassemble(rebuilt, rl, 2, back) == 1280 && !memcmp(back, sent, 1280));
      /* split_and_can_split_leave_a_payload_to_packet_too_big_when_the_limit_carries_1280_or_the_packet_exceeds_1280 */
      assert(fipsd_ipv6_can_split(payload, (size_t)pn, &peer, FD1, 1090));
      assert(!fipsd_ipv6_split(payload, (size_t)pn, &peer, FD1, 1247, 1, &frags) && !fipsd_ipv6_can_split(payload, (size_t)pn, &peer, FD1, 1247));
      packet(FD1, peer_addr, 1300, sent); int on = fipsd_ipv6_compress(sent, 1300, payload, sizeof payload);
      assert(!fipsd_ipv6_split(payload, (size_t)on, &peer, FD1, 1090, 1, &frags) && !fipsd_ipv6_can_split(payload, (size_t)on, &peer, FD1, 1090)); }
    /* the_held_syn_clamp_reaches_a_syn_behind_destination_options_and_leaves_one_behind_a_fragment_header */
    { const uint8_t dopt[8] = {6, 0, 1, 4, 0, 0, 0, 0}, frag[8] = {6, 0, 0, 0, 0, 0, 0, 9}; uint8_t released[128], held[128];
      size_t n = syn_behind(peer_addr, 60, dopt, sent); int pn = fipsd_ipv6_compress(sent, n, payload, sizeof payload); size_t plen = (size_t)pn;
      assert(fipsd_ipv6_reclamp(payload, &plen, sizeof payload, &peer, FD1, 1143, true, 1090) == 1063);
      int rn = fipsd_ipv6_decompress(payload, plen, FD1, peer_addr, released, sizeof released); assert(rn == 72 && be16(released + 70) == 1063 && tcp_sum(released, 72, 48) == 0xffff);
      n = syn_behind(peer_addr, 44, frag, sent); pn = fipsd_ipv6_compress(sent, n, payload, sizeof payload); plen = (size_t)pn; memcpy(held, payload, plen);
      assert(fipsd_ipv6_reclamp(payload, &plen, sizeof payload, &peer, FD1, 1143, true, 1090) == -1 && plen == (size_t)pn && !memcmp(payload, held, plen)); }
    /* a_refusal_or_an_unknown_destination_over_1280_on_a_limit_under_1280_is_told_1280 */
    { packet(FD1, peer_addr, 1300, sent); int pn = fipsd_ipv6_compress(sent, 1300, payload, sizeof payload);
      const uint8_t *payloads[1] = {payload}; size_t plens[1] = {(size_t)pn};
      fipsd_ipv6_refusal r = {.kind = FIPSD_IPV6_REFUSAL_TOO_BIG, .peer = peer, .payload_limit = 1170, .payloads = payloads, .lens = plens, .count = 1};
      /* answered through the limiter: one Packet Too Big reporting 1280 */
      owed_seen = (struct owed_seen){0}; assert(fipsd_ipv6_answer(&r, FD1, scratch, sizeof scratch, see_owed, NULL) == 1);
      assert(owed_seen.kind == FIPSD_IPV6_OWED_TOO_BIG && owed_seen.mtu == 1280 && owed_seen.len == 1300 && !memcmp(owed_seen.packet, sent, 1300));
      static answered got; answer_into(&got, &r); assert(got.count == 1 && got.bytes[0][40] == 2 && be32(got.bytes[0] + 44) == 1280);
      state_with(&stranger_key, 1000); uint8_t other[16] = {0xfd}; memset(other + 1, 0x77, 15);
      packet(FD1, other, 1300, sent); fipsd_ipv6_outbound(sent, 1300, &state, FD1, scratch, sizeof scratch, &h); assert(h.kind == FIPSD_IPV6_HOST_TOO_BIG && h.mtu == 1280);
      packet(FD1, other, 1100, sent); fipsd_ipv6_outbound(sent, 1100, &state, FD1, scratch, sizeof scratch, &h); assert(h.kind == FIPSD_IPV6_HOST_TOO_BIG && h.mtu == 1033);
      fipsd_ipv6_mesh_state_destroy(&state); }
}

/* ===== tests/core.rs (MeshState) ===== */
static void state_tests(void)
{
    for (unsigned mtu = 0; mtu <= UINT16_MAX; ++mtu) {
        assert(fipsd_ipv6_tcp_mss(fips_framing_payload_limit((uint16_t)mtu)) == fipsd_ipv6_mss_ceiling((uint16_t)mtu));
        if (mtu >= 110) assert(fipsd_ipv6_mtu_of_limit(fips_framing_payload_limit((uint16_t)mtu)) == fipsd_ipv6_effective_mtu((uint16_t)mtu));
        else { assert(fipsd_ipv6_mtu_of_limit(fips_framing_payload_limit((uint16_t)mtu)) == FIPSD_IPV6_HEADER_SAVING); assert(fipsd_ipv6_effective_mtu((uint16_t)mtu) < 40); }
    }
    /* before_any_attach_the_state_is_the_ipv6_minimum */
    assert(fipsd_ipv6_mesh_state_init(&state, 8, 8) == 0);
    assert(fipsd_ipv6_mesh_state_ceiling(&state) == fipsd_ipv6_mss_ceiling(1280) && fipsd_ipv6_mesh_state_ipv6_mtu(&state) == fipsd_ipv6_effective_mtu(1280));
    assert(fipsd_ipv6_paths_count(fipsd_ipv6_mesh_state_paths(&state)) == 0);
    /* applying_the_node_messages_updates_the_shared_state */
    fips_node_addr us = key_n(10), peer = key_n(11); uint8_t peer_addr[16]; address_of(&peer, peer_addr); fips_ipv6_address attached;
    fipsd_ipv6_from_mesh m = {.kind = FIPSD_IPV6_MESH_ATTACHED, .peer = us, .payload_limit = fips_framing_payload_limit(1357)};
    assert(fipsd_ipv6_mesh_state_apply(&state, &m, &attached) == FIPSD_IPV6_APPLIED_ATTACHED && !memcmp(attached.bytes, fipsd_ipv6_peer_address(&us).bytes, 16));
    assert(fipsd_ipv6_mesh_state_ceiling(&state) == fipsd_ipv6_mss_ceiling(1357));
    m = (fipsd_ipv6_from_mesh){.kind = FIPSD_IPV6_MESH_PATH_MTU, .peer = peer, .has_limit = true, .payload_limit = fips_framing_payload_limit(1280)};
    assert(fipsd_ipv6_mesh_state_apply(&state, &m, NULL) == FIPSD_IPV6_APPLIED_STATE);
    { uint16_t l; assert(fipsd_ipv6_paths_get(fipsd_ipv6_mesh_state_paths(&state), peer_addr, &l) && l == fips_framing_payload_limit(1280)); }
    m = (fipsd_ipv6_from_mesh){.kind = FIPSD_IPV6_MESH_NODE_MTU, .payload_limit = fips_framing_payload_limit(1500)}; fipsd_ipv6_mesh_state_apply(&state, &m, NULL);
    assert(fipsd_ipv6_mesh_state_ceiling(&state) == fipsd_ipv6_mss_ceiling(1500) && fipsd_ipv6_mesh_state_ipv6_mtu(&state) == fipsd_ipv6_effective_mtu(1500));
    m = (fipsd_ipv6_from_mesh){.kind = FIPSD_IPV6_MESH_PATH_MTU, .peer = peer, .has_limit = false}; fipsd_ipv6_mesh_state_apply(&state, &m, NULL);
    assert(fipsd_ipv6_paths_count(fipsd_ipv6_mesh_state_paths(&state)) == 0);
    m = (fipsd_ipv6_from_mesh){.kind = FIPSD_IPV6_MESH_SYNCED}; assert(fipsd_ipv6_mesh_state_apply(&state, &m, NULL) == FIPSD_IPV6_APPLIED_SYNCED);
    /* the_key_map_takes_known_keys_and_drops_forgotten_ones_by_prefix */
    fips_node_prefix prefix = fips_node_addr_prefix(&peer); fips_node_addr found;
    assert(!fipsd_ipv6_mesh_state_key(&state, &prefix, &found));
    m = (fipsd_ipv6_from_mesh){.kind = FIPSD_IPV6_MESH_KNOWN_KEY, .peer = peer}; fipsd_ipv6_mesh_state_apply(&state, &m, NULL);
    assert(fipsd_ipv6_mesh_state_key(&state, &prefix, &found) && !memcmp(found.bytes, peer.bytes, 16));
    m = (fipsd_ipv6_from_mesh){.kind = FIPSD_IPV6_MESH_FORGOT_KEY, .peer = peer, .learned = 0}; fipsd_ipv6_mesh_state_apply(&state, &m, NULL);
    assert(!fipsd_ipv6_mesh_state_key(&state, &prefix, &found));
    /* a_key_learned_after_the_node_decided_to_forget_it_is_kept */
    m = (fipsd_ipv6_from_mesh){.kind = FIPSD_IPV6_MESH_KNOWN_KEY, .peer = peer}; fipsd_ipv6_mesh_state_apply(&state, &m, NULL);
    fipsd_ipv6_learned report = fipsd_ipv6_mesh_state_learn(&state, &peer); assert(report.seq == 1 && !memcmp(report.peer.bytes, peer.bytes, 16));
    m = (fipsd_ipv6_from_mesh){.kind = FIPSD_IPV6_MESH_FORGOT_KEY, .peer = peer, .learned = report.seq - 1}; fipsd_ipv6_mesh_state_apply(&state, &m, NULL);
    assert(fipsd_ipv6_mesh_state_key(&state, &prefix, &found));
    m.learned = report.seq; fipsd_ipv6_mesh_state_apply(&state, &m, NULL); assert(!fipsd_ipv6_mesh_state_key(&state, &prefix, &found));
    assert(fipsd_ipv6_mesh_state_report(&state, &peer).seq == 2);
    /* an_attach_drops_every_key_before_its_replay */
    m = (fipsd_ipv6_from_mesh){.kind = FIPSD_IPV6_MESH_KNOWN_KEY, .peer = peer}; fipsd_ipv6_mesh_state_apply(&state, &m, NULL);
    m = (fipsd_ipv6_from_mesh){.kind = FIPSD_IPV6_MESH_ATTACHED, .peer = key_n(12), .payload_limit = 1170}; fipsd_ipv6_mesh_state_apply(&state, &m, NULL);
    assert(!fipsd_ipv6_mesh_state_key(&state, &prefix, &found));
    /* the address of any key is derived from it: fd || node address bytes 0..15 */
    assert(peer_addr[0] == 0xfd && !memcmp(peer_addr + 1, peer.bytes, 15));
    /* the host link applies in line; a bounded key table keeps the newest keys */
    fipsd_ipv6_host_link link = {&state};
    for (uint8_t k = 20; k < 30; ++k) { m = (fipsd_ipv6_from_mesh){.kind = FIPSD_IPV6_MESH_KNOWN_KEY, .peer = key_n(k)}; fipsd_ipv6_host_link_send(&link, &m); }
    fips_node_addr keys[16]; assert(fipsd_ipv6_mesh_state_keys(&state, keys, 16) == 8);
    { fips_node_addr newest = key_n(29); fips_node_prefix np = fips_node_addr_prefix(&newest); assert(fipsd_ipv6_mesh_state_key(&state, &np, &found));
      fips_node_addr oldest = key_n(20); fips_node_prefix op = fips_node_addr_prefix(&oldest); assert(!fipsd_ipv6_mesh_state_key(&state, &op, &found)); }
    fipsd_ipv6_mesh_state_destroy(&state);
}
int main(void)
{
    addresses();
    icmp_unit_tests(); icmp_answer_tests(); rate_limit_tests(); inbound_tests(); queue_tests(); outbound_tests(); state_tests();
    puts("IPv6 shim ICMP: unreachable/too-big builders and suppression, per-source token bucket at exact instants, delivery/CE, host verdicts and refusal answers, bounded queue, mesh state: ok");
    return 0;
}
