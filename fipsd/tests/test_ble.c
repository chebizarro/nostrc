/* BLE transport core (nostrc-gfi8h.6.1): ports of upstream
 * transport/ble/{psm,pool,stream_read,stats,io,io_radio,recall,mod}.rs tests
 * and tests/{admission,close,retry,recall}.rs, over pure state and over the
 * simulated radio. Every wait is a bounded channel select on an event, never
 * a sleep. */
#define _POSIX_C_SOURCE 200809L
#include "transport/ble/internal.h"
#include "control/transport.h"
#include "host_os.h"
#include "test_tmpdir.h"
#include <fips/identity/local.h>
#include <fips/identity/node_addr.h>
#include <fips/identity/peer.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_MS 15000u
#define GRACE 10000u
static fipsd_ble_peer peer_n(const char *adapter, uint8_t n)
{ fipsd_ble_peer p = {{0}, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, n}}; strcpy(p.adapter, adapter); return p; }
static fips_transport_addr addr_n(uint8_t n)
{ fips_transport_addr a; fipsd_ble_peer p = peer_n("hci0", n); assert(fipsd_ble_peer_to_addr(&p, &a)); return a; }
static fips_node_addr node_n(uint8_t n) { fips_node_addr a = {{0}}; a.bytes[0] = n; return a; }

/* ---- psm.rs ---- */
static void psm_wire(void)
{
    uint8_t b[2]; uint16_t v;
    fipsd_ble_encode_psm(0x0085, b); assert(b[0] == 0x85 && b[1] == 0x00);
    fipsd_ble_encode_psm(0x1234, b); assert(b[0] == 0x34 && b[1] == 0x12);
    const uint16_t round[] = {0, 1, 0x0085, 0x00FF, 0x1234, UINT16_MAX};
    for (size_t i = 0; i < 6; ++i) { fipsd_ble_encode_psm(round[i], b); assert(fipsd_ble_decode_psm(b, 2, &v) && v == round[i]); }
    assert(!fipsd_ble_decode_psm(NULL, 0, &v));                       /* legacy UUID-only advert */
    assert(!fipsd_ble_decode_psm((const uint8_t[]){0x85}, 1, &v));    /* truncated */
    assert(fipsd_ble_decode_psm((const uint8_t[]){0x85, 0, 0xFF, 0xFF}, 4, &v) && v == 0x0085); /* trailing ignored */
    assert(FIPSD_BLE_ADV_FLAGS_BYTES + FIPSD_BLE_ADV_UUID128_BYTES + FIPSD_BLE_ADV_PSM_BYTES == 27);
    assert(FIPSD_BLE_ADV_FLAGS_BYTES + FIPSD_BLE_ADV_UUID128_BYTES + 20 == 41); /* a 128-bit key overflows */
    fipsd_ble_encode_psm(0x00C1, b);
    assert(fipsd_ble_decode_gatt_psm(b, 2, &v) && v == 0x00C1);
    assert(fipsd_ble_decode_gatt_psm((const uint8_t[]){0xC1, 0, 0xFF}, 3, &v) && v == 0x00C1);
    assert(!fipsd_ble_decode_gatt_psm((const uint8_t[]){0xC1}, 1, &v) && !fipsd_ble_decode_gatt_psm(NULL, 0, &v));
    assert(!fipsd_ble_decode_gatt_psm((const uint8_t[]){0, 0}, 2, &v)); /* zero is not a PSM */
    unsigned lead = 0; assert(sscanf(FIPSD_BLE_SERVICE_UUID, "%4x", &lead) == 1 && lead == FIPSD_BLE_PSM_SERVICE_DATA_UUID16);
    puts("psm: little-endian, legacy/truncated/trailing, 27-byte legacy PDU budget, GATT value rejects 0, key = leading 16 bits: ok");
}
/* ---- addr.rs, io_macos peer_addr ---- */
static void addresses(void)
{
    fipsd_ble_peer p, q;
    assert(fipsd_ble_peer_parse("hci0/AA:BB:CC:DD:EE:FF", 22, &p) && !strcmp(p.adapter, "hci0") && p.device[5] == 0xFF);
    assert(fipsd_ble_peer_parse("hci1/0a:1b:2c:3d:4e:5f", 22, &q) && q.device[0] == 0x0A && q.device[5] == 0x5F);
    char text[64]; assert(fipsd_ble_peer_format(&q, text, sizeof text) == 22 && !strcmp(text, "hci1/0A:1B:2C:3D:4E:5F"));
    const char *bad[] = {"AA:BB:CC:DD:EE:FF", "/AA:BB:CC:DD:EE:FF", "hci0/AA:BB:CC:DD:EE", "hci0/AA:BB:CC:DD:EE:GG",
        "hci0/AA-BB-CC-DD-EE-FF", "hci0/AA:BB:CC:DD:EE:FF:00", "averyveryverylongadapter/AA:BB:CC:DD:EE:FF"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) assert(!fipsd_ble_peer_parse(bad[i], strlen(bad[i]), &p));
    fips_transport_addr a; assert(fipsd_ble_peer_to_addr(&q, &a) && fipsd_ble_peer_from_addr(&a, &p) && fipsd_ble_peer_equal(&p, &q));
    uint8_t id[16]; for (int i = 0; i < 16; ++i) id[i] = (uint8_t)i;
    fipsd_ble_peer m = fipsd_ble_peer_from_identifier(id), again = fipsd_ble_peer_from_identifier(id);
    const uint8_t pinned[6] = {0x7E, 0x84, 0xDC, 0x94, 0x77, 0x85};
    assert(!memcmp(m.device, pinned, 6) && !strcmp(m.adapter, FIPSD_BLE_RADIO_ADAPTER) && fipsd_ble_peer_equal(&m, &again));
    id[15] ^= 1; fipsd_ble_peer other = fipsd_ble_peer_from_identifier(id);
    assert(!fipsd_ble_peer_equal(&m, &other) && (other.device[0] & 0x02) && !(other.device[0] & 0x01));
    puts("addresses: adapter/MAC parse+format, rejects malformed, identifier mapping pinned/stable/local unicast: ok");
}
/* ---- pool.rs ---- */
static ble_link links[32];
static ble_link *claimed(uint8_t n, int node, bool verified, uint64_t at)
{
    ble_link *l = &links[n];
    *l = (ble_link){.addr = addr_n(n), .peer = peer_n("hci0", n), .id = n, .verified = verified,
        .established_ms = at, .grace_from_ms = at, .held_from_ms = at, .send_mtu = 2048, .recv_mtu = 2048};
    if (node >= 0) { l->has_node = true; l->node = node_n((uint8_t)node); }
    return l;
}
static ble_insert ins(ble_pool *p, ble_link *l, uint64_t at) { return ble_pool_insert(p, l, at); }
static bool ADMITTED(ble_insert r, const ble_link *evicted) { return r.kind == BLE_INSERT_ADMITTED && r.evicted == evicted; }
static bool REFUSED(ble_insert r, bool first) { return r.kind == BLE_INSERT_REFUSED && r.first == first; }
static void pool_rules(void)
{
    const uint64_t t0 = 1000000; fips_transport_addr a;
    ble_pool p; assert(!ble_pool_init(&p, 7, GRACE));
    assert(ADMITTED(ins(&p, claimed(1, -1, false, t0), t0), NULL) && p.count == 1); a = addr_n(1); assert(ble_pool_get(&p, &a));
    assert(ble_pool_remove(&p, &links[1]) && !p.count);
    /* a full pool evicts its oldest unverified link once past its grace */
    for (uint8_t n = 1; n <= 5; ++n) (void)ins(&p, claimed(n, n, true, t0), t0);
    (void)ins(&p, claimed(6, 6, false, t0 + 1000), t0 + 1000); (void)ins(&p, claimed(7, 7, false, t0 + 2000), t0 + 2000);
    ble_insert r = ins(&p, claimed(8, 8, false, t0 + 20000), t0 + 20000);
    assert(ADMITTED(r, &links[6]) && !r.has_ended && p.count == 7);
    ble_pool_free(&p);
    /* occupied whatever it claims, incumbent kept */
    for (int claim = 1; claim <= 2; ++claim) {
        assert(!ble_pool_init(&p, 2, GRACE)); (void)ins(&p, claimed(1, 1, true, t0), t0);
        ble_link extra = *claimed(2, claim, false, t0); extra.addr = addr_n(1);
        assert(ins(&p, &extra, t0).kind == BLE_INSERT_OCCUPIED && p.count == 1);
        a = addr_n(1); assert(ble_pool_get(&p, &a)->verified); ble_pool_free(&p);
    }
    ble_link m = *claimed(1, -1, false, t0); m.send_mtu = 1024; assert(ble_link_mtu(&m) == 1024);
    /* find_verified / verified addr / claims */
    assert(!ble_pool_init(&p, 7, GRACE)); (void)ins(&p, claimed(1, 1, true, t0), t0);
    fips_node_addr n1 = node_n(1), n2 = node_n(2);
    assert(ble_pool_find_verified(&p, &n1) == &links[1] && !ble_pool_find_verified(&p, &n2));
    a = addr_n(99); assert(!ble_pool_get(&p, &a)); /* a rotated address looks new by address only */
    (void)ins(&p, claimed(2, 2, true, t0), t0); assert(ble_pool_find_verified(&p, &n2) == &links[2]);
    (void)ins(&p, claimed(3, -1, false, t0), t0); /* unidentified */
    (void)ins(&p, claimed(4, 4, false, t0), t0);
    fips_node_addr n4 = node_n(4); assert(!ble_pool_find_verified(&p, &n4) && ble_pool_claims(&p, &n4));
    ble_pool_free(&p);
    /* mark_verified: mismatch, absent, demotion */
    assert(!ble_pool_init(&p, 7, GRACE)); (void)ins(&p, claimed(1, 1, false, t0), t0); (void)ins(&p, claimed(2, 2, false, t0), t0);
    a = addr_n(1); assert(ble_pool_mark_verified(&p, &a, &n2, NULL) == BLE_VERIFY_MISMATCH && !links[1].verified);
    assert(!memcmp(links[1].node.bytes, n1.bytes, 16));
    a = addr_n(9); assert(ble_pool_mark_verified(&p, &a, &n1, NULL) == BLE_VERIFY_ABSENT);
    ble_pool_free(&p);
    assert(!ble_pool_init(&p, 7, GRACE)); (void)ins(&p, claimed(1, 1, false, t0), t0); (void)ins(&p, claimed(2, 1, false, t0), t0);
    size_t demoted;
    a = addr_n(1); assert(ble_pool_mark_verified(&p, &a, &n1, &demoted) == BLE_VERIFY_MARKED && !demoted);
    a = addr_n(2); assert(ble_pool_mark_verified(&p, &a, &n1, &demoted) == BLE_VERIFY_MARKED && demoted == 1);
    assert(p.count == 2 && !links[1].verified && links[2].verified);
    for (uint8_t n = 3; n <= 7; ++n) (void)ins(&p, claimed(n, n, true, t0), t0);
    assert(ADMITTED(ins(&p, claimed(8, 8, false, t0 + 20000), t0 + 20000), &links[1]));
    ble_pool_free(&p);
    /* full of verified links: the newcomer is refused */
    assert(!ble_pool_init(&p, 7, GRACE));
    for (uint8_t n = 1; n <= 7; ++n) (void)ins(&p, claimed(n, n, true, t0), t0);
    assert(ins(&p, claimed(8, 8, false, t0 + 60000), t0 + 60000).kind == BLE_INSERT_REFUSED && p.count == 7);
    ble_pool_free(&p);
    /* a newly admitted unverified link keeps its slot through its grace */
    assert(!ble_pool_init(&p, 7, GRACE));
    for (uint8_t n = 1; n <= 6; ++n) (void)ins(&p, claimed(n, n, true, t0), t0);
    uint64_t t = t0 + 100000; (void)ins(&p, claimed(7, 7, false, t), t);
    r = ins(&p, claimed(8, 8, false, t + 1000), t + 1000); assert(REFUSED(r, true) && !r.has_ended);
    r = ins(&p, claimed(9, 9, false, t + 2000), t + 2000); assert(REFUSED(r, false) && !r.has_ended);
    fips_node_addr n7 = node_n(7); a = addr_n(7); assert(ble_pool_mark_verified(&p, &a, &n7, NULL) == BLE_VERIFY_MARKED);
    ble_pool_free(&p);
    /* the refusal run is reported once and cleared after a quiet minute */
    assert(!ble_pool_init(&p, 1, GRACE)); (void)ins(&p, claimed(1, 1, true, t0), t0);
    const uint64_t ks[] = {0, 10000, 20000}; const bool firsts[] = {true, false, false};
    for (int i = 0; i < 3; ++i) { r = ins(&p, claimed(2, 2, false, t0 + ks[i]), t0 + ks[i]); assert(REFUSED(r, firsts[i]) && !r.has_ended); }
    uint64_t last = t0 + 20000; (void)ble_pool_remove(&p, &links[1]);
    r = ins(&p, claimed(3, 3, false, last + 30000), last + 30000); assert(ADMITTED(r, NULL) && !r.has_ended);
    (void)ble_pool_remove(&p, &links[3]);
    r = ins(&p, claimed(4, 4, false, last + 61000), last + 61000); assert(ADMITTED(r, NULL) && r.has_ended && r.ended == 3);
    fips_node_addr n4b = node_n(4); a = addr_n(4); (void)ble_pool_mark_verified(&p, &a, &n4b, NULL);
    r = ins(&p, claimed(5, 5, false, last + 62000), last + 62000); assert(REFUSED(r, true) && !r.has_ended);
    ble_pool_free(&p);
    assert(!ble_pool_init(&p, 1, GRACE)); (void)ins(&p, claimed(1, 1, true, t0), t0);
    (void)ins(&p, claimed(2, 2, false, t0), t0); (void)ins(&p, claimed(2, 2, false, t0 + 1000), t0 + 1000);
    r = ins(&p, claimed(2, 2, false, t0 + 91000), t0 + 91000); assert(REFUSED(r, true) && r.has_ended && r.ended == 2);
    ble_pool_free(&p);
    /* a node's duplicate goes before another node's only link */
    assert(!ble_pool_init(&p, 3, GRACE));
    (void)ins(&p, claimed(1, 3, false, t0), t0); (void)ins(&p, claimed(2, 1, false, t0 + 1000), t0 + 1000);
    (void)ins(&p, claimed(3, 1, false, t0 + 2000), t0 + 2000);
    assert(ADMITTED(ins(&p, claimed(4, 4, false, t0 + 20000), t0 + 20000), &links[2]));
    assert(ADMITTED(ins(&p, claimed(5, 5, false, t0 + 21000), t0 + 21000), &links[1]));
    ble_pool_free(&p);
    /* a link its peer moved off keeps the grace it had; a withdrawn one gets it again */
    assert(!ble_pool_init(&p, 2, GRACE)); (void)ins(&p, claimed(1, 1, true, t0), t0); (void)ins(&p, claimed(2, 1, false, t0), t0);
    a = addr_n(1); assert(ble_pool_clear_verified(&p, &a, &n1, false, t0 + 60000));
    a = addr_n(2); (void)ble_pool_mark_verified(&p, &a, &n1, NULL);
    assert(ADMITTED(ins(&p, claimed(3, 3, false, t0 + 60000), t0 + 60000), &links[1]));
    ble_pool_free(&p);
    assert(!ble_pool_init(&p, 2, GRACE)); (void)ins(&p, claimed(1, 1, true, t0), t0); (void)ins(&p, claimed(2, 2, false, t0 + 1000), t0 + 1000);
    a = addr_n(1); assert(ble_pool_clear_verified(&p, &a, &n1, true, t0 + 60000) && links[1].established_ms == t0);
    assert(ADMITTED(ins(&p, claimed(3, 3, false, t0 + 61000), t0 + 61000), &links[2]));
    assert(ins(&p, claimed(4, 4, false, t0 + 62000), t0 + 62000).kind == BLE_INSERT_REFUSED);
    assert(ADMITTED(ins(&p, claimed(5, 5, false, t0 + 70500), t0 + 70500), &links[1]));
    ble_pool_free(&p);
    /* an occupied address leaves the latch untouched */
    for (int verified = 0; verified < 2; ++verified) {
        assert(!ble_pool_init(&p, 2, GRACE)); (void)ins(&p, claimed(1, 2, verified, t0), t0);
        ble_link extra = *claimed(9, 3, false, t0); extra.addr = addr_n(1);
        assert(ins(&p, &extra, t0).kind == BLE_INSERT_OCCUPIED);
        fips_node_addr n2b = node_n(2); a = addr_n(1);
        assert(!memcmp(ble_pool_get(&p, &a)->node.bytes, n2b.bytes, 16) && ble_pool_get(&p, &a)->verified == (bool)verified);
        (void)ins(&p, claimed(2, 4, true, t0), t0);
        r = ins(&p, claimed(3, 5, false, t0 + 1000), t0 + 1000); assert(REFUSED(r, true) && !r.has_ended);
        ble_pool_free(&p);
    }
    /* remove_own: a newer connection at the address is left alone */
    assert(!ble_pool_init(&p, 2, GRACE)); (void)ins(&p, claimed(1, 1, false, t0), t0);
    a = addr_n(1); assert(!ble_pool_remove_own(&p, &a, 99) && ble_pool_remove_own(&p, &a, 1) == &links[1]);
    /* unproven: never held, never proven, past timeout */
    (void)ins(&p, claimed(1, 1, false, t0), t0); (void)ins(&p, claimed(2, 2, false, t0), t0);
    ble_link *out[4]; assert(!ble_pool_unproven(&p, t0 + 4000, 5000, out, 4));
    a = addr_n(1); ble_pool_hold(&p, &a, t0 + 4000);
    assert(ble_pool_unproven(&p, t0 + 5000, 5000, out, 4) == 1 && out[0] == &links[2]);
    a = addr_n(2); fips_node_addr n2c = node_n(2); (void)ble_pool_mark_verified(&p, &a, &n2c, NULL);
    (void)ble_pool_clear_verified(&p, &a, &n2c, false, t0); /* proven stays proven */
    assert(!ble_pool_unproven(&p, t0 + 8000, 5000, out, 4) && ble_pool_unproven(&p, t0 + 9000, 5000, out, 4) == 1 && out[0] == &links[1]);
    ble_pool_free(&p);
    puts("pool: insert/remove/occupied, eviction by grace and duplicates, verified never evicted, refusal latch, demotion, regrace, unproven: ok");
}
/* ---- mod.rs arbitrate / newcomer_wins ---- */
static void arbitration(void)
{
    const bool cases[8][4] = {{1, 1, 0, 1}, {1, 0, 1, 0}, {0, 0, 1, 1}, {0, 1, 0, 0},
        {1, 1, 1, 0}, {1, 0, 0, 0}, {0, 1, 1, 0}, {0, 0, 0, 0}};
    for (int i = 0; i < 8; ++i) assert(ble_newcomer_wins(cases[i][0], cases[i][1], cases[i][2]) == cases[i][3]);
    const uint64_t t0 = 5000000, window = 10000;
    fips_node_addr small = node_n(1), big = node_n(9), other = node_n(5);
    fips_transport_addr at1 = addr_n(1), at2 = addr_n(2);
    ble_pool p; assert(!ble_pool_init(&p, 7, GRACE));
    /* a settled verified link to the node declines any newcomer, wherever it is */
    ble_link *v = claimed(2, 9, true, t0); (void)ins(&p, v, t0);
    ble_admission a = ble_arbitrate(&p, &small, &big, &at1, true, window, t0 + window);
    assert(a.kind == BLE_DECLINE && a.existing == v);
    a = ble_arbitrate(&p, &small, &big, &at1, false, window, t0 + window + 1);
    assert(a.kind == BLE_DECLINE && a.existing == v);
    /* inside the window a verified link still racing at its own address (the
     * crossing) is settled by the tie-break (the deliberate deviation): the
     * loser is replaced, the winner declines */
    a = ble_arbitrate(&p, &small, &big, &at2, true, window, t0 + 1); /* smaller keeps its outbound: replace the inbound */
    assert(a.kind == BLE_REPLACE && a.existing == v);
    /* an unauthenticated claim from another address never displaces it */
    a = ble_arbitrate(&p, &small, &big, &at1, true, window, t0 + 1);
    assert(a.kind == BLE_DECLINE && a.existing == v);
    a = ble_arbitrate(&p, &small, &big, &at2, false, window, t0 + 1); /* same direction: a rotation, declined */
    assert(a.kind == BLE_DECLINE && a.existing == v);
    v->outbound = true;
    a = ble_arbitrate(&p, &small, &big, &at2, false, window, t0 + 1); /* the verified link is the winner */
    assert(a.kind == BLE_DECLINE && a.existing == v);
    ble_pool_free(&p);
    /* the newcomer's address holds a link claiming another node, or none */
    for (int claim = -1; claim <= 5; claim += 6) {
        assert(!ble_pool_init(&p, 7, GRACE)); (void)ins(&p, claimed(1, claim, false, t0), t0);
        a = ble_arbitrate(&p, &small, &big, &at1, true, window, t0 + 1); assert(a.kind == BLE_OCCUPIED);
        ble_pool_free(&p);
    }
    /* same address, same node, racing: the link the smaller node dialled stays */
    for (int local_small = 0; local_small < 2; ++local_small) for (int out = 0; out < 2; ++out) {
        const fips_node_addr *local = local_small ? &small : &big, *peer = local_small ? &big : &small;
        assert(!ble_pool_init(&p, 7, GRACE));
        ble_link *inc = claimed(1, peer->bytes[0], false, t0); inc->outbound = !out; (void)ins(&p, inc, t0);
        a = ble_arbitrate(&p, local, peer, &at1, out, window, t0 + 1);
        assert(a.existing == inc && a.kind == (ble_newcomer_wins(local_small, out, !out) ? BLE_REPLACE : BLE_DECLINE));
        /* outside the window the incumbent is settled and kept */
        a = ble_arbitrate(&p, local, peer, &at1, out, window, t0 + window);
        assert(a.kind == BLE_DECLINE && a.existing == inc);
        ble_pool_free(&p);
    }
    /* a racing rival at another address, opposite direction */
    assert(!ble_pool_init(&p, 7, GRACE));
    ble_link *rival = claimed(2, 9, false, t0); rival->outbound = false; (void)ins(&p, rival, t0);
    a = ble_arbitrate(&p, &small, &big, &at1, true, window, t0 + 1); assert(a.kind == BLE_REPLACE && a.existing == rival);
    a = ble_arbitrate(&p, &big, &small, &at1, true, window, t0 + 1); /* the rival claims node 9, not 1 */
    assert(a.kind == BLE_ADMIT);
    ble_pool_free(&p);
    assert(!ble_pool_init(&p, 7, GRACE));
    rival = claimed(2, 1, false, t0); rival->outbound = false; (void)ins(&p, rival, t0);
    a = ble_arbitrate(&p, &big, &small, &at1, true, window, t0 + 1); assert(a.kind == BLE_DECLINE && a.existing == rival);
    /* a same-direction link is a rotation, not a race; an old one blocks nothing */
    a = ble_arbitrate(&p, &big, &small, &at1, false, window, t0 + 1); assert(a.kind == BLE_ADMIT);
    a = ble_arbitrate(&p, &big, &small, &at1, true, window, t0 + window); assert(a.kind == BLE_ADMIT);
    (void)at2; (void)other;
    ble_pool_free(&p);
    puts("arbitration: newcomer_wins table, settled verified declines, racing verified tie-break, occupied, racing same/other address, rotation and settled admits: ok");
}
/* ---- mod.rs PendingProbes ---- */
static void probe_book(void)
{
    const uint64_t cd = 30000, t0 = 1000;
    ble_probes b; ble_probes_init(&b, cd);
    fipsd_ble_peer a0 = peer_n("ble0", 0), a1 = peer_n("ble0", 1), a2 = peer_n("ble0", 2), a7 = peer_n("ble0", 7), out;
    ble_probes_observe(&b, &a1, t0); assert(ble_probes_is_due(&b, &a1, t0));
    for (uint32_t shift = 0; shift < BLE_MAX_PROBE_BACKOFF_SHIFT; ++shift) {
        assert(ble_probes_record_failure(&b, &a1, t0) == shift + 1);
        uint64_t wait = cd << shift;
        assert(!ble_probes_is_due(&b, &a1, t0 + wait - 1) && ble_probes_is_due(&b, &a1, t0 + wait));
    }
    for (int i = 0; i < 8; ++i) { (void)ble_probes_record_failure(&b, &a1, t0); assert(ble_probes_is_due(&b, &a1, t0 + (cd << BLE_MAX_PROBE_BACKOFF_SHIFT))); }
    /* a dead address is dialled a handful of times in 37 minutes */
    ble_probes_init(&b, cd); ble_probes_observe(&b, &a1, t0);
    unsigned dials = 0;
    for (uint64_t now = t0; now <= t0 + 37 * 60000; now += cd)
        if (ble_probes_is_due(&b, &a1, now)) { ble_probes_mark_attempt(&b, &a1, now); (void)ble_probes_record_failure(&b, &a1, now); ++dials; }
    assert(dials >= 1 && dials <= 10);
    /* repeated sightings do not wash out the backoff; failures never evict the address */
    ble_probes_init(&b, cd); ble_probes_observe(&b, &a1, t0);
    for (int i = 0; i < 4; ++i) (void)ble_probes_record_failure(&b, &a1, t0);
    for (int i = 0; i < 100; ++i) ble_probes_observe(&b, &a1, t0 + cd);
    assert(!ble_probes_is_due(&b, &a1, t0 + cd) && b.count == 1);
    for (int i = 0; i < 500; ++i) (void)ble_probes_record_failure(&b, &a1, t0);
    assert(b.count == 1 && ble_probes_next_due(&b, t0 + (cd << BLE_MAX_PROBE_BACKOFF_SHIFT), &out) && fipsd_ble_peer_equal(&out, &a1));
    /* resolving clears the history */
    ble_probes_resolve(&b, &a1); assert(!b.count);
    ble_probes_observe(&b, &a1, t0); assert(ble_probes_is_due(&b, &a1, t0));
    /* the retry tick rotates; a backed-off address blocks nobody */
    ble_probes_init(&b, cd);
    ble_probes_observe(&b, &a0, t0); ble_probes_observe(&b, &a1, t0); ble_probes_observe(&b, &a2, t0);
    const fipsd_ble_peer *order[] = {&a0, &a1, &a2, &a0, &a1, &a2};
    for (int i = 0; i < 6; ++i) assert(ble_probes_next_due(&b, t0, &out) && fipsd_ble_peer_equal(&out, order[i]));
    ble_probes_init(&b, cd); ble_probes_observe(&b, &a0, t0); ble_probes_observe(&b, &a1, t0);
    (void)ble_probes_record_failure(&b, &a0, t0);
    assert(ble_probes_next_due(&b, t0, &out) && fipsd_ble_peer_equal(&out, &a1));
    assert(ble_probes_next_due(&b, t0, &out) && fipsd_ble_peer_equal(&out, &a1));
    ble_probes_init(&b, cd); ble_probes_observe(&b, &a0, t0); (void)ble_probes_record_failure(&b, &a0, t0);
    assert(!ble_probes_next_due(&b, t0, &out));
    /* a full book evicts the most failed address */
    ble_probes_init(&b, cd);
    for (uint8_t n = 0; n < BLE_MAX_PENDING_PROBES; ++n) { fipsd_ble_peer x = peer_n("ble0", n); ble_probes_observe(&b, &x, t0); }
    for (int i = 0; i < 3; ++i) (void)ble_probes_record_failure(&b, &a7, t0);
    fipsd_ble_peer a200 = peer_n("ble0", 200); ble_probes_observe(&b, &a200, t0);
    assert(b.count == BLE_MAX_PENDING_PROBES && !ble_probes_contains(&b, &a7) && ble_probes_contains(&b, &a200) && ble_probes_contains(&b, &a0));
    puts("probe book: due at once, exponential backoff capped, a handful of dials per hour, sightings keep backoff, rotation, eviction: ok");
}
/* ---- recall.rs ---- */
static fipsd_ble_peer raddr(uint32_t n)
{ fipsd_ble_peer p = {"hci0", {0x30, 0x00, (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n}}; return p; }
static bool live_none(void *c, const fipsd_ble_peer *p) { (void)c; (void)p; return false; }
static bool live_all(void *c, const fipsd_ble_peer *p) { (void)c; (void)p; return true; }
static bool live_one(void *c, const fipsd_ble_peer *p) { fipsd_ble_peer x = raddr(*(uint32_t *)c); return fipsd_ble_peer_equal(p, &x); }
static bool live_low(void *c, const fipsd_ble_peer *p) { (void)c; return p->device[5] < 70 && !p->device[2] && !p->device[3] && !p->device[4]; }
static uint16_t recalled(ble_recall *r, uint32_t n) { fipsd_ble_peer p = raddr(n); const ble_recall_entry *e = ble_recall_get(r, &p); return e ? e->psm : 0xFFFF; }
static void note(ble_recall *r, uint32_t n, uint16_t v, uint64_t now, ble_peer_pred live, void *ctx)
{ fipsd_ble_peer p = raddr(n); (void)ble_recall_note(r, &p, v, NULL, now, live, ctx); }
static void recall_bounds(void)
{
    ble_recall r; uint64_t t0 = 1000;
    assert(!ble_recall_init(&r, 140));
    for (uint32_t n = 0; n < 10000; ++n) { note(&r, n, (uint16_t)n, t0, live_none, NULL); assert(ble_recall_len(&r) <= BLE_MAX_RECALLED); }
    assert(ble_recall_len(&r) == BLE_MAX_RECALLED); ble_recall_free(&r);
    uint32_t zero = 0; assert(!ble_recall_init(&r, 140));
    for (uint32_t n = 0; n <= BLE_MAX_RECALLED; ++n) note(&r, n, (uint16_t)n, t0, live_one, &zero);
    assert(ble_recall_len(&r) == BLE_MAX_RECALLED + 1);
    note(&r, 1000, 1000, t0, live_one, &zero);
    assert(recalled(&r, 0) == 0 && recalled(&r, 1) == 0xFFFF && recalled(&r, 2) == 2 && recalled(&r, 1000) == 1000);
    assert(ble_recall_len(&r) == BLE_MAX_RECALLED + 1); ble_recall_free(&r);
    assert(!ble_recall_init(&r, 140));
    for (uint32_t n = 0; n < 70; ++n) note(&r, n, (uint16_t)n, t0, live_low, NULL);
    for (uint32_t n = 0; n < 1000; ++n) note(&r, 10000 + n, (uint16_t)n, t0, live_low, NULL);
    for (uint32_t n = 0; n < 70; ++n) assert(recalled(&r, n) == n);
    assert(ble_recall_len(&r) - 70 <= BLE_MAX_RECALLED); ble_recall_free(&r);
    uint32_t one = 1; assert(!ble_recall_init(&r, 140));
    note(&r, 1, 1, t0, live_one, &one); note(&r, 2, 2, t0, live_one, &one); note(&r, 3, 3, t0 + 500000, live_one, &one);
    ble_recall_prune(&r, t0 + BLE_RECALL_MAX_AGE_MS + 1000, live_one, &one);
    assert(recalled(&r, 1) == 1 && recalled(&r, 2) == 0xFFFF && recalled(&r, 3) == 3 && ble_recall_len(&r) == 2);
    ble_recall_free(&r);
    assert(!ble_recall_init(&r, 140));
    for (uint32_t n = 0; n < BLE_MAX_RECALLED; ++n) note(&r, n, (uint16_t)n, t0, live_none, NULL);
    note(&r, 0, 100, t0 + 500000, live_none, NULL);
    assert(ble_recall_len(&r) == BLE_MAX_RECALLED && recalled(&r, 0) == 100);
    note(&r, 1000, 1000, t0 + 500000, live_none, NULL); assert(recalled(&r, 0) == 100 && recalled(&r, 1) == 0xFFFF);
    ble_recall_prune(&r, t0 + BLE_RECALL_MAX_AGE_MS + 1000, live_none, NULL);
    assert(recalled(&r, 0) == 100 && recalled(&r, 2) == 0xFFFF); ble_recall_free(&r);
    assert(!ble_recall_init(&r, 140)); note(&r, 1, 1, t0, live_all, NULL);
    ble_recall_prune(&r, t0 + 2000000, live_all, NULL); assert(recalled(&r, 1) == 1);
    ble_recall_prune(&r, t0 + 2001000, live_none, NULL); assert(recalled(&r, 1) == 1); /* kept for the age limit from release */
    ble_recall_prune(&r, t0 + 2000000 + BLE_RECALL_MAX_AGE_MS, live_none, NULL); assert(recalled(&r, 1) == 0xFFFF);
    ble_recall_free(&r);
    puts("recall: bounded over 10k addresses, live never evicted, prune by age, renote refreshes, released live kept for the age: ok");
}
/* ---- stream_read.rs: a scripted stream ---- */
typedef struct scripted { fipsd_ble_stream base; const uint8_t *chunks[8]; size_t lens[8], count, next; } scripted;
static ssize_t scripted_read(fipsd_ble_stream *s, uint8_t *buf, size_t cap, uint64_t deadline)
{
    (void)deadline; scripted *x = (scripted *)s;
    if (x->next == x->count) return 0;
    size_t n = x->lens[x->next] < cap ? x->lens[x->next] : cap;
    memcpy(buf, x->chunks[x->next++], n); return (ssize_t)n;
}
static int scripted_write(fipsd_ble_stream *s, const uint8_t *p, size_t n, uint64_t d) { (void)s; (void)p; (void)n; (void)d; return 0; }
static void scripted_nop(fipsd_ble_stream *s) { (void)s; }
static const fipsd_ble_stream_ops scripted_ops = {.read = scripted_read, .write = scripted_write, .cancel = scripted_nop, .close = scripted_nop};
static void frame_bytes(uint8_t *p, size_t n)
{ memset(p, 0x5A, n); size_t payload = n - FIPS_FRAMING_FMP_ENCRYPTED_MIN_SIZE; p[0] = 0; p[1] = 0; p[2] = (uint8_t)payload; p[3] = (uint8_t)(payload >> 8); }
static void reader_rules(void)
{
    ble_reader r; uint8_t out[16];
    scripted s = {.base.ops = &scripted_ops, .chunks = {(const uint8_t *)"abc", (const uint8_t *)"defg", (const uint8_t *)"hij"}, .lens = {3, 4, 3}, .count = 3};
    assert(!ble_reader_init(&r, &s.base, -1, 2048) && !ble_reader_read_exact(&r, out, 10, FIPSD_BLE_NO_DEADLINE) && !memcmp(out, "abcdefghij", 10));
    ble_reader_free(&r);
    s = (scripted){.base.ops = &scripted_ops, .chunks = {(const uint8_t *)"0123456789"}, .lens = {10}, .count = 1};
    assert(!ble_reader_init(&r, &s.base, -1, 2048) && !ble_reader_read_exact(&r, out, 4, FIPSD_BLE_NO_DEADLINE) && !memcmp(out, "0123", 4));
    assert(ble_reader_buffered(&r) == 6 && !ble_reader_read_exact(&r, out, 6, FIPSD_BLE_NO_DEADLINE) && !memcmp(out, "456789", 6));
    assert(ble_reader_read_exact(&r, out, 1, FIPSD_BLE_NO_DEADLINE) == -EPIPE && ble_reader_read_exact(&r, out, 1, FIPSD_BLE_NO_DEADLINE) == -EPIPE);
    ble_reader_free(&r);
    s = (scripted){.base.ops = &scripted_ops, .chunks = {(const uint8_t *)"ab"}, .lens = {2}, .count = 1};
    assert(!ble_reader_init(&r, &s.base, -1, 0) && r.capacity == BLE_MIN_RECV_CHUNK); /* a degenerate MTU still reads */
    assert(ble_reader_read_exact(&r, out, 4, FIPSD_BLE_NO_DEADLINE) == -EPIPE); /* peer drop: UnexpectedEof */
    ble_reader_free(&r);
    /* frames: coalesced split, fragmented reassembled, oversize rejected, clean EOF only between frames */
    uint8_t two[128], buf[2048]; frame_bytes(two, 40); frame_bytes(two + 40, 50);
    s = (scripted){.base.ops = &scripted_ops, .chunks = {two}, .lens = {90}, .count = 1};
    fipsd_stream f; fipsd_stream_init(&f, buf, sizeof buf, 2048); size_t n;
    assert(!ble_reader_init(&r, &s.base, -1, 2048));
    assert(ble_reader_frame(&r, &f, FIPSD_BLE_NO_DEADLINE, &n) == 1 && n == 40);
    assert(ble_reader_frame(&r, &f, FIPSD_BLE_NO_DEADLINE, &n) == 1 && n == 50);
    assert(ble_reader_frame(&r, &f, FIPSD_BLE_NO_DEADLINE, &n) == 0);
    ble_reader_free(&r);
    s = (scripted){.base.ops = &scripted_ops, .chunks = {two, two + 3, two + 20}, .lens = {3, 17, 20}, .count = 3};
    assert(!ble_reader_init(&r, &s.base, -1, 2048));
    assert(ble_reader_frame(&r, &f, FIPSD_BLE_NO_DEADLINE, &n) == 1 && n == 40 && !memcmp(buf, two, 40));
    ble_reader_free(&r);
    uint8_t big[200]; frame_bytes(big, 200);
    s = (scripted){.base.ops = &scripted_ops, .chunks = {big}, .lens = {200}, .count = 1};
    fipsd_stream small; fipsd_stream_init(&small, buf, sizeof buf, 128);
    assert(!ble_reader_init(&r, &s.base, -1, 128) && ble_reader_frame(&r, &small, FIPSD_BLE_NO_DEADLINE, &n) == -EPROTO);
    ble_reader_free(&r);
    s = (scripted){.base.ops = &scripted_ops, .chunks = {two}, .lens = {20}, .count = 1};
    assert(!ble_reader_init(&r, &s.base, -1, 2048) && ble_reader_frame(&r, &f, FIPSD_BLE_NO_DEADLINE, &n) == -EPROTO);
    ble_reader_free(&r);
    puts("stream_read: fragments reassemble, coalesced tail kept, EOF latched, degenerate MTU, FMP frames split/joined/oversize/truncated: ok");
}
/* ---- stats.rs, config ---- */
static void stats_and_config(void)
{
    size_t n = 0; const char *const *fields = fipsd_transport_stats_fields("ble", &n);
    const char *upstream[] = {"packets_sent", "bytes_sent", "packets_recv", "bytes_recv", "send_errors", "recv_errors",
        "mtu_exceeded", "connections_established", "connections_accepted", "connections_rejected", "handshakes_aborted",
        "connect_timeouts", "connect_errors", "pubkey_exchange_failures", "pool_evictions", "advertisements_sent",
        "scan_results", "duplicate_node_declines", "duplicate_link_replacements"};
    assert(n == sizeof upstream / sizeof upstream[0]);
    for (size_t i = 0; i < n; ++i) assert(!strcmp(fields[i], upstream[i]));
    fipsd_ble_config c; fipsd_ble_config_default(&c);
    assert(!strcmp(c.adapter, "hci0") && c.psm == 0x0085 && c.mtu == 2048 && c.max_connections == 7 && c.connect_timeout_ms == 10000);
    assert(c.advertise && c.scan && !c.auto_connect && c.accept_connections && c.probe_cooldown_ms == 30000);
    assert(fipsd_ble_verify_grace_ms(&c) == 10000);
    c.connect_timeout_ms = 2000; assert(fipsd_ble_verify_grace_ms(&c) == 10000);
    c.connect_timeout_ms = 15000; assert(fipsd_ble_verify_grace_ms(&c) == 15000);
    /* transports.ble parses with upstream names. Without a platform backend
     * it is ignored with a warning; over BlueZ it is created, not started. */
    const char *yaml = "transports:\n  ble:\n    adapter: hci1\n    psm: 0x00C1\n    mtu: 512\n    auto_connect: true\n";
    fipsd_config *cfg = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(cfg);
    fipsd_transport_registry r = {0}; fipsd_transport *owned[32]; size_t count = 0;
    const char *reason = NULL; fipsd_ble_backend b;
#if defined(FIPSD_ENABLE_BLE_BLUEZ)
    assert(fipsd_ble_register_config(&r, cfg, owned, &count) == 0 && count == 1 && r.count == 1 && fipsd_ble_is(owned[0]));
    assert(owned[0]->ops->mtu(owned[0], NULL) == 512 && owned[0]->auto_connect);
    fipsd_ble_destroy(owned[0]);
    assert(fipsd_ble_platform_backend(&c, &b, &reason) && fipsd_ble_backend_valid(&b));
    b.ops->destroy(b.self);
#else
    assert(fipsd_ble_register_config(&r, cfg, owned, &count) == 0 && count == 0 && r.count == 0);
    assert(!fipsd_ble_platform_backend(&c, &b, &reason) && reason && strstr(reason, "gfi8h.6.2"));
#endif
    fipsd_config_free(cfg);
    puts("stats/config: upstream BleStatsSnapshot field order, defaults, verify grace floor, no silent simulated fallback: ok");
}

/* ======================================================================
 * Over the simulated radio
 * ====================================================================== */
static fips_secp *secp;
static fipsd_runtime *rt;
static fipsd_ble_air *air;
typedef struct tnode {
    fipsd_ble_radio *radio;
    fipsd_transport *t;
    GoChannel *events;
    fips_identity *id;
    fips_xonly_pubkey key;
    fips_node_addr node;
    fipsd_ble_peer peer;
    fips_transport_addr addr;
} tnode;
typedef struct raw {
    fipsd_ble_radio *radio;
    fipsd_ble_backend b;
    fipsd_ble_sink sink;
    fipsd_ble_peer peer;
    fips_xonly_pubkey key;
    fips_node_addr node;
} raw;
static tnode *nodes[8]; static size_t node_count;
static raw *raws[24]; static size_t raw_count;
static fipsd_ble_radio_config radio_cfg(uint8_t dev)
{ fipsd_ble_radio_config c = {.device = {0x02, 0x00, 0x00, 0x00, 0x00, dev}}; return c; }
static void identity_of(uint8_t seed, fips_identity **out, fips_xonly_pubkey *key, fips_node_addr *node)
{
    uint8_t secret[32]; memset(secret, seed, 32);
    fips_identity *id = NULL;
    assert(fips_identity_from_secret_bytes(secp, fipsd_host_os_entropy(), secret, &id) == FIPS_IDENTITY_OK);
    *key = fips_identity_pubkey(id); *node = fips_node_addr_from_pubkey(key);
    if (out) *out = id; else fips_identity_destroy(id);
}
static fipsd_ble_config fast_cfg(void)
{
    fipsd_ble_config c; fipsd_ble_config_default(&c);
    c.adapter = "ble0"; c.connect_timeout_ms = 1500; c.probe_cooldown_ms = 150; c.pubkey_exchange_ms = 400;
    return c;
}
/* Advertises and accepts but never scans, so it never dials on its own. */
static fipsd_ble_config passive_cfg(void) { fipsd_ble_config c = fast_cfg(); c.scan = false; return c; }
static tnode *tnode_new(uint8_t dev, const fipsd_ble_radio_config *rc, const fipsd_ble_config *bc, bool identity)
{
    tnode *n = calloc(1, sizeof *n); assert(n);
    fipsd_ble_radio_config r = rc ? *rc : radio_cfg(dev);
    memcpy(r.device, (uint8_t[]){0x02, 0, 0, 0, 0, dev}, 6);
    n->radio = fipsd_ble_radio_create(air, &r); assert(n->radio);
    fipsd_ble_config c = bc ? *bc : fast_cfg();
    if (identity) { identity_of(dev, &n->id, &n->key, &n->node); c.identity = &n->key; c.secp = secp; }
    char name[16]; snprintf(name, sizeof name, "ble%u", dev);
    n->t = fipsd_ble_create((fips_transport_id){dev}, name, &c, fipsd_ble_radio_backend(n->radio)); assert(n->t);
    n->peer = (fipsd_ble_peer){FIPSD_BLE_RADIO_ADAPTER, {0x02, 0, 0, 0, 0, dev}};
    if (r.adapter) strcpy(n->peer.adapter, r.adapter);
    assert(fipsd_ble_peer_to_addr(&n->peer, &n->addr));
    n->events = go_channel_create(512); assert(n->events);
    nodes[node_count++] = n;
    return n;
}
static void tnode_start(tnode *n) { assert(n->t->ops->start_async(n->t, rt, NULL, n->events) == 0); }
static raw *raw_new(uint8_t dev, const fipsd_ble_radio_config *rc, uint8_t claim_seed)
{
    raw *x = calloc(1, sizeof *x); assert(x);
    fipsd_ble_radio_config r = rc ? *rc : radio_cfg(dev);
    memcpy(r.device, (uint8_t[]){0x02, 0, 0, 0, 0, dev}, 6);
    x->radio = fipsd_ble_radio_create(air, &r); assert(x->radio);
    x->b = fipsd_ble_radio_backend(x->radio);
    x->sink.events = go_channel_create(64); x->sink.adapter_wake = go_channel_create(1); atomic_store(&x->sink.open, true);
    x->peer = (fipsd_ble_peer){FIPSD_BLE_RADIO_ADAPTER, {0x02, 0, 0, 0, 0, dev}};
    identity_of(claim_seed, NULL, &x->key, &x->node);
    assert(x->b.ops->start(x->b.self, rt, &x->sink) == 0);
    raws[raw_count++] = x;
    return x;
}
static void *take(GoChannel *c, uint64_t ms)
{
    void *v = NULL; GoSelectCase k = {GO_SELECT_RECEIVE, c, NULL, &v};
    GoSelectResult r = go_select_timeout(&k, 1, ms);
    return r.selected_case == 0 ? v : NULL;
}
/* The next event of `kind` (at `at`, if given) within WAIT_MS; other events
 * are dropped, and do not extend the deadline. */
static fipsd_transport_event *await(tnode *n, fipsd_transport_event_kind kind, const fips_transport_addr *at)
{
    const uint64_t deadline = ble_now() + WAIT_MS;
    for (;;) {
        uint64_t now = ble_now();
        fipsd_transport_event *e = now < deadline ? take(n->events, deadline - now) : NULL;
        if (!e) { fprintf(stderr, "%s: no event %d\n", n->t->name, (int)kind); assert(e); }
        if (e->kind == kind && (!at || fips_transport_addr_equal(&e->address, at))) return e;
        free(e);
    }
}
static void await_free(tnode *n, fipsd_transport_event_kind kind, const fips_transport_addr *at) { free(await(n, kind, at)); }
/* Until the transport's pool reaches `count`; woken by link exits. */
static void await_pooled(tnode *n, size_t count)
{
    ble_instance *u = n->t->user;
    for (;;) {
        if (fipsd_ble_connected_count(n->t) == count) return;
        assert(take(u->changed, WAIT_MS));
    }
}
static fipsd_ble_stream *raw_connect(raw *x, const tnode *to, uint16_t psm)
{
    fipsd_ble_stream *s = NULL;
    int rc = x->b.ops->connect(x->b.self, &to->peer, psm ? psm : FIPSD_BLE_DEFAULT_PSM, ble_now() + WAIT_MS, &s);
    assert(rc == 0 && s);
    return s;
}
static void raw_hello(raw *x, fipsd_ble_stream *s, uint8_t prefix)
{ uint8_t m[33] = {prefix}; memcpy(m + 1, x->key.bytes, 32); assert(s->ops->write(s, m, 33, ble_now() + WAIT_MS) == 0); }
/* Read until the far end closes: true on EOF/reset, false on the deadline. */
static bool raw_until_closed(fipsd_ble_stream *s, uint8_t *got, size_t cap, size_t *len)
{
    uint8_t buf[4096]; size_t total = 0;
    for (;;) {
        ssize_t n = s->ops->read(s, buf, sizeof buf, ble_now() + WAIT_MS);
        if (n == -ETIMEDOUT) return false;
        if (n <= 0) { if (len) *len = total; return true; }
        if (got && total + (size_t)n <= cap) memcpy(got + total, buf, (size_t)n);
        total += (size_t)n;
    }
}
static void raw_close(fipsd_ble_stream *s) { s->ops->cancel(s); s->ops->close(s); }
static fipsd_ble_stats_view stats(tnode *n) { return fipsd_ble_stats_snapshot(n->t); }
static int stop_all(fipsd_subsystem *s, fipsd_runtime *r) { (void)s; (void)r; return 0; }
static void stop_nodes(fipsd_subsystem *s, fipsd_runtime *r)
{
    (void)s; (void)r;
    for (size_t i = 0; i < node_count; ++i) nodes[i]->t->ops->stop(nodes[i]->t);
    for (size_t i = 0; i < raw_count; ++i) raws[i]->b.ops->stop(raws[i]->b.self);
}
typedef struct body { void (*fn)(void); GoChannel *done; } body;
static void trampoline(void *arg) { body *b = arg; b->fn(); assert(go_channel_send(b->done, b) == 0); }
static void scenario(void (*fn)(void))
{
    fipsd_runtime_config rc; fipsd_runtime_config_default(&rc); rc.workers = 2; rc.handle_signals = false; rc.grace_ms = 10000;
    rt = fipsd_runtime_create(&rc); assert(rt);
    air = fipsd_ble_air_create(); assert(air);
    fipsd_subsystem sub = {.name = "ble-test", .start = stop_all, .stop = stop_nodes};
    assert(!fipsd_runtime_register(rt, &sub) && !fipsd_runtime_start(rt));
    body b = {fn, go_channel_create(1)};
    assert(fipsd_runtime_spawn(rt, trampoline, &b) == 0);
    assert(take(b.done, 120000) == &b);
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    for (size_t i = 0; i < node_count; ++i) {
        tnode *n = nodes[i]; void *v;
        fipsd_ble_destroy(n->t);
        while (go_channel_try_receive(n->events, &v) == 0) free(v);
        go_channel_unref(n->events); if (n->id) fips_identity_destroy(n->id); free(n);
    }
    for (size_t i = 0; i < raw_count; ++i) {
        raw *x = raws[i]; void *v;
        x->b.ops->destroy(x->b.self);
        while (go_channel_try_receive(x->sink.events, &v) == 0) {
            fipsd_ble_event *e = v; if (e->stream) raw_close(e->stream); free(e);
        }
        go_channel_unref(x->sink.events); go_channel_unref(x->sink.adapter_wake); free(x);
    }
    node_count = raw_count = 0;
    fipsd_runtime_destroy(rt); rt = NULL;
    fipsd_ble_air_destroy(air); air = NULL;
    go_channel_unref(b.done);
}
static void frame(uint8_t *p, size_t n, uint8_t fill)
{ frame_bytes(p, n); memset(p + 4, fill, n - 4); }
static int tsend(tnode *from, const tnode *to, size_t n, uint8_t fill)
{ uint8_t buf[4096]; frame(buf, n, fill); return from->t->ops->send(from->t, &to->addr, buf, n); }
static void await_packet(tnode *at, const tnode *from, size_t n, uint8_t fill)
{
    fipsd_transport_event *e = await(at, FIPSD_TRANSPORT_PACKET, &from->addr);
    uint8_t want[4096]; frame(want, n, fill);
    assert(e->size == n && !memcmp(e->bytes, want, n)); free(e);
}

/* mod.rs transport basics, io_radio listen/advertise/slot semantics */
static void lifecycle_body(void)
{
    fipsd_ble_radio_config rc = radio_cfg(1); rc.assigned_psm = 0x00C1; /* an OS-assigned PSM */
    tnode *a = tnode_new(1, &rc, NULL, true);
    assert(!strcmp(a->t->type, "ble") && a->t->ops->state(a->t) == FIPSD_TRANSPORT_STOPPED);
    assert(a->t->ops->mtu(a->t, NULL) == 2048 && !a->t->auto_connect && a->t->accept_connections);
    assert(fipsd_transport_medium(a->t) == FIPS_PATH_Unknown);
    tnode_start(a);
    await_free(a, FIPSD_TRANSPORT_BECAME_PRESENT, NULL); assert(take(fipsd_ble_bound(a->t), WAIT_MS));
    assert(a->t->ops->state(a->t) == FIPSD_TRANSPORT_BOUND && fipsd_transport_medium(a->t) == FIPS_PATH_Ble);
    fipsd_ble_radio_stats s = fipsd_ble_radio_stats_read(a->radio);
    /* the advertised PSM is the one actually bound, not the one requested */
    assert(fipsd_ble_listener_psm(a->t) == 0x00C1 && s.bound_psm == 0x00C1 && s.advertised_psm == 0x00C1);
    assert(s.listens == 1 && s.advertising && s.scanning && !s.advertise_restarts && stats(a).advertisements_sent == 1);
    /* a backend that honours the request advertises it */
    tnode *b = tnode_new(2, NULL, NULL, false); tnode_start(b);
    await_free(b, FIPSD_TRANSPORT_BECAME_PRESENT, NULL);
    assert(fipsd_ble_listener_psm(b->t) == FIPSD_BLE_DEFAULT_PSM && fipsd_ble_radio_stats_read(b->radio).advertised_psm == FIPSD_BLE_DEFAULT_PSM);
    /* an adapter missing at start: up cleanly, absent; adopted when it appears */
    fipsd_ble_radio_config absent = radio_cfg(3); absent.absent = true;
    tnode *c = tnode_new(3, &absent, NULL, true); tnode_start(c);
    fips_transport_addr nowhere = addr_n(77);
    assert(c->t->ops->state(c->t) == FIPSD_TRANSPORT_ABSENT && c->t->ops->connect_async(c->t, &nowhere) == 0);
    await_free(c, FIPSD_TRANSPORT_DISCONNECTED, &nowhere); /* dials fail while it is missing */
    assert(fipsd_ble_radio_stats_read(c->radio).listens == 0);
    fipsd_ble_radio_set_adapter(c->radio, true, true);
    await_free(c, FIPSD_TRANSPORT_BECAME_PRESENT, NULL);
    s = fipsd_ble_radio_stats_read(c->radio); assert(s.listens == 1 && s.advertising && s.scanning);
    /* the control projection: upstream show_transports carries no local_addr or interface for BLE */
    fipsd_control_transport_view *v = calloc(1, sizeof *v); assert(v);
    fipsd_control_transport_read(a->t, ble_now(), v);
    assert(!strcmp(v->row.type, "ble") && !strcmp(v->row.state, "up") && v->row.mtu == 2048 && !v->row.local_addr && !v->row.interface);
    free(v);
    puts("lifecycle: type/state/mtu defaults, OS-assigned PSM advertised, honoured PSM, absent adapter adopted later, control row: ok");
}
/* mod.rs test_scan_discovers_peers: without a local key a sighting is
 * reported by address and nothing is dialled. The advertiser only advertises,
 * like upstream's injected scan result: were it to scan, its own probe could
 * pool an inbound link at its address first, and a pooled address is not
 * reported again. */
static void discovery_body(void)
{
    fipsd_ble_config passive = passive_cfg();
    tnode *b = tnode_new(2, NULL, &passive, true); tnode_start(b); await_free(b, FIPSD_TRANSPORT_BECAME_PRESENT, NULL);
    tnode *a = tnode_new(1, NULL, NULL, false); tnode_start(a);
    fipsd_transport_event *e = await(a, FIPSD_TRANSPORT_DISCOVERED, &b->addr);
    assert(e->size == 0); free(e);
    assert(!fipsd_ble_radio_stats_read(a->radio).connects && stats(a).scan_results >= 1);
    puts("discovery: an advert is announced by address alone when there is no local key, nothing dialled: ok");
}
/* Two transports with keys find each other, link once, carry frames both ways. */
/* a dials b, which only advertises (passive_cfg): exactly one channel joins
 * the pair, so it is settled once both pools hold it, and every step there is
 * an event (a pool entry is announced as DISCOVERED, a channel opened or freed
 * wakes the air). Were b to scan as well, its own probe could still dial after
 * that point, and the tie-break would replace the link under frames in flight;
 * a probe that finds the address pooled resolves without any event to wait
 * on. Crossing dials are settled by the tie-break, covered by arbitration. */
static void link_pair(tnode *a, tnode *b)
{
    for (;;) {
        if (fipsd_ble_radio_channels(a->radio) == 1 && fipsd_ble_radio_channels(b->radio) == 1 &&
            fipsd_ble_connected_count(a->t) == 1 && fipsd_ble_connected_count(b->t) == 1) return;
        void *v = NULL;
        GoSelectCase cases[] = {{GO_SELECT_RECEIVE, a->events, NULL, &v}, {GO_SELECT_RECEIVE, b->events, NULL, &v},
            {GO_SELECT_RECEIVE, fipsd_ble_air_changed(air), NULL, &v}};
        GoSelectResult r = go_select_timeout(cases, 3, WAIT_MS);
        assert(r.selected_case >= 0);
        if (r.selected_case < 2 && v) {
            fipsd_transport_event *e = v; tnode *at = r.selected_case ? b : a, *other = r.selected_case ? a : b;
            if (e->kind == FIPSD_TRANSPORT_DISCOVERED)
                assert(fips_transport_addr_equal(&e->address, &other->addr) && e->size == 32 && !memcmp(e->bytes, other->key.bytes, 32));
            (void)at; free(e);
        }
    }
}
static void pair_body_mode(fipsd_ble_radio_mode mode)
{
    fipsd_ble_radio_config ra = radio_cfg(1), rb = radio_cfg(2);
    ra.mode = rb.mode = mode; ra.chunk = rb.chunk = 5; ra.send_mtu = ra.recv_mtu = rb.send_mtu = rb.recv_mtu = 512;
    fipsd_ble_config passive = passive_cfg();
    tnode *a = tnode_new(1, &ra, NULL, true), *b = tnode_new(2, &rb, &passive, true);
    tnode_start(a); tnode_start(b);
    link_pair(a, b);
    assert(fipsd_ble_connected_count(a->t) == 1 && fipsd_ble_connected_count(b->t) == 1);
    for (unsigned i = 0; i < 20; ++i) {
        size_t n = 40 + i * 23;
        assert(tsend(a, b, n, (uint8_t)i) == 0); await_packet(b, a, n, (uint8_t)i);
        assert(tsend(b, a, n, (uint8_t)(i + 100)) == 0); await_packet(a, b, n, (uint8_t)(i + 100));
    }
    /* a burst coalesces on a byte stream and still splits into frames */
    for (unsigned i = 0; i < 8; ++i) assert(tsend(a, b, 100, (uint8_t)(200 + i)) == 0);
    for (unsigned i = 0; i < 8; ++i) await_packet(b, a, 100, (uint8_t)(200 + i));
    /* MTU: the negotiated one, and a rejection is not a send error */
    assert(a->t->ops->mtu(a->t, &b->addr) == 512 && a->t->ops->mtu(a->t, &a->addr) == 2048);
    uint64_t errors = stats(a).send_errors;
    assert(tsend(a, b, 600, 1) == -1 && stats(a).mtu_exceeded == 1 && stats(a).send_errors == errors);
    fipsd_ble_stats_view sa = stats(a), sb = stats(b);
    assert(sa.connections_established + sa.connections_accepted >= 1 && !sa.pubkey_exchange_failures && !sb.pubkey_exchange_failures);
    assert(sa.packets_sent == 28 && sb.packets_recv == 28 && sa.bytes_sent == sb.bytes_recv);
}
static void pair_sdu_body(void) { pair_body_mode(FIPSD_BLE_RADIO_SDU); puts("pair (SDU): discovery, PSM, L2CAP, exchange, one link, frames both ways, MTU 512, mtu_exceeded not an error: ok"); }
static void pair_bytes_body(void) { pair_body_mode(FIPSD_BLE_RADIO_BYTES); puts("pair (byte stream, 5-byte fragments): exchange and frames reassembled, bursts split: ok"); }
static void pair_fd_body(void) { pair_body_mode(FIPSD_BLE_RADIO_FD); puts("pair (socketpair fd fast path through libgo): frames both ways: ok"); }
/* Link loss and adapter power: DISCONNECTED, the retry book, BECAME_ABSENT /
 * BECAME_PRESENT, a fresh listener and advert, and the link back. */
static void reconnect_body(void)
{
    fipsd_ble_config passive = passive_cfg();
    tnode *a = tnode_new(1, NULL, NULL, true), *b = tnode_new(2, NULL, &passive, true);
    tnode_start(a); tnode_start(b);
    link_pair(a, b);
    assert(fipsd_ble_radio_drop_links(a->radio) == 1);
    await_free(a, FIPSD_TRANSPORT_DISCONNECTED, &b->addr); await_free(b, FIPSD_TRANSPORT_DISCONNECTED, &a->addr);
    /* nothing re-advertises (BlueZ reports a device once a session): the
     * node's dial brings the link back, through connect_async */
    assert(a->t->ops->connect_async(a->t, &b->addr) == 0);
    await_free(a, FIPSD_TRANSPORT_CONNECTED, &b->addr);
    fipsd_transport_event *e = await(b, FIPSD_TRANSPORT_DISCOVERED, &a->addr); assert(e->size == 32); free(e);
    assert(a->t->ops->connect_async(a->t, &b->addr) == 0); /* already connected: CONNECTED at once */
    await_free(a, FIPSD_TRANSPORT_CONNECTED, &b->addr);
    assert(tsend(a, b, 64, 7) == 0); await_packet(b, a, 64, 7);
    fipsd_ble_radio_set_adapter(a->radio, true, false);
    await_free(a, FIPSD_TRANSPORT_BECAME_ABSENT, NULL);
    await_free(b, FIPSD_TRANSPORT_DISCONNECTED, &a->addr);
    assert(a->t->ops->state(a->t) == FIPSD_TRANSPORT_ABSENT && !fipsd_ble_listener_psm(a->t));
    await_pooled(a, 0);
    assert(tsend(a, b, 64, 8) == -1);
    fipsd_ble_radio_set_adapter(a->radio, true, true);
    await_free(a, FIPSD_TRANSPORT_BECAME_PRESENT, NULL);
    fipsd_ble_radio_stats s = fipsd_ble_radio_stats_read(a->radio);
    assert(s.listens == 2 && s.advertising && s.scanning && fipsd_ble_listener_psm(a->t) == FIPSD_BLE_DEFAULT_PSM);
    link_pair(a, b);
    assert(tsend(b, a, 80, 9) == 0); await_packet(a, b, 80, 9);
    /* removal is absence too */
    fipsd_ble_radio_set_adapter(b->radio, false, false);
    await_free(b, FIPSD_TRANSPORT_BECAME_ABSENT, NULL); await_free(a, FIPSD_TRANSPORT_DISCONNECTED, &b->addr);
    fipsd_ble_radio_set_adapter(b->radio, true, true);
    await_free(b, FIPSD_TRANSPORT_BECAME_PRESENT, NULL);
    link_pair(a, b);
    puts("reconnect: link loss -> DISCONNECTED -> node redial -> CONNECTED; adapter off/on and removal -> ABSENT/PRESENT, relisten, relink: ok");
}
static void raw_serve(raw *x, uint16_t requested, uint16_t advertised)
{
    uint16_t bound = 0;
    assert(x->b.ops->listen(x->b.self, requested, ble_now() + WAIT_MS, &bound) == 0 && bound);
    fipsd_ble_advert ad = {.psm = advertised ? advertised : bound}; fipsd_ble_encode_psm(ad.psm, ad.service_data);
    assert(x->b.ops->advertise(x->b.self, &ad, ble_now() + WAIT_MS) == 0);
}
static fipsd_ble_event *raw_event(raw *x, fipsd_ble_event_kind kind)
{
    for (;;) {
        fipsd_ble_event *e = take(x->sink.events, WAIT_MS); assert(e);
        if (e->kind == kind) return e;
        if (e->stream) raw_close(e->stream);
        free(e);
    }
}
/* mod.rs advertised PSM tests and the GATT PSM characteristic */
static void psm_body(void)
{
    fipsd_ble_radio_config ra = radio_cfg(1); ra.read_gatt = true;
    tnode *a = tnode_new(1, &ra, NULL, false); /* no key: probes only learn and report */
    raw *r = raw_new(10, NULL, 10);
    raw_serve(r, FIPSD_BLE_DEFAULT_PSM, 0x00C1); /* a stale advert: listens on 0x0085 */
    fipsd_ble_radio_config r2c = radio_cfg(11); r2c.assigned_psm = 0x00C1;
    raw *r2 = raw_new(11, &r2c, 11); raw_serve(r2, FIPSD_BLE_DEFAULT_PSM, 0);
    fipsd_ble_radio_config r3c = radio_cfg(12); r3c.assigned_psm = 0x00D1; r3c.legacy_advert = r3c.serve_gatt_psm = true;
    raw *r3 = raw_new(12, &r3c, 12); raw_serve(r3, FIPSD_BLE_DEFAULT_PSM, 0);
    tnode_start(a);
    fips_transport_addr ra_addr, r2_addr, r3_addr;
    assert(fipsd_ble_peer_to_addr(&r->peer, &ra_addr) && fipsd_ble_peer_to_addr(&r2->peer, &r2_addr) && fipsd_ble_peer_to_addr(&r3->peer, &r3_addr));
    await_free(a, FIPSD_TRANSPORT_DISCOVERED, &ra_addr); await_free(a, FIPSD_TRANSPORT_DISCOVERED, &r2_addr);
    await_free(a, FIPSD_TRANSPORT_DISCOVERED, &r3_addr);
    /* the advertised PSM is dialled; a refusal forgets it, and the fallback then reaches the listener */
    assert(a->t->ops->connect_async(a->t, &ra_addr) == 0); await_free(a, FIPSD_TRANSPORT_DISCONNECTED, &ra_addr);
    fipsd_ble_stats_view s = stats(a); assert(s.connect_errors == 1 && !s.connect_timeouts);
    assert(a->t->ops->connect_async(a->t, &ra_addr) == 0); await_free(a, FIPSD_TRANSPORT_CONNECTED, &ra_addr);
    fipsd_ble_event *in = raw_event(r, FIPSD_BLE_INBOUND_STREAM); raw_close(in->stream); free(in);
    /* a timed-out dial keeps what it learned */
    fipsd_ble_radio_set_stall(a->radio, true);
    assert(a->t->ops->connect_async(a->t, &r2_addr) == 0); await_free(a, FIPSD_TRANSPORT_DISCONNECTED, &r2_addr);
    assert(stats(a).connect_timeouts == 1 && stats(a).connect_errors == 1);
    fipsd_ble_radio_set_stall(a->radio, false);
    assert(a->t->ops->connect_async(a->t, &r2_addr) == 0); await_free(a, FIPSD_TRANSPORT_CONNECTED, &r2_addr);
    in = raw_event(r2, FIPSD_BLE_INBOUND_STREAM); raw_close(in->stream); free(in);
    /* a UUID-only advertiser serving its OS-assigned PSM over GATT */
    assert(a->t->ops->connect_async(a->t, &r3_addr) == 0); await_free(a, FIPSD_TRANSPORT_CONNECTED, &r3_addr);
    in = raw_event(r3, FIPSD_BLE_INBOUND_STREAM); raw_close(in->stream); free(in);
    assert(fipsd_ble_radio_stats_read(a->radio).gatt_reads == 1 && stats(a).connect_errors == 1);
    /* a backend without the GATT read dials the configured PSM, and is refused */
    tnode *b = tnode_new(2, NULL, NULL, false); tnode_start(b);
    await_free(b, FIPSD_TRANSPORT_DISCOVERED, &r3_addr);
    assert(b->t->ops->connect_async(b->t, &r3_addr) == 0); await_free(b, FIPSD_TRANSPORT_DISCONNECTED, &r3_addr);
    assert(stats(b).connect_errors == 1 && !fipsd_ble_radio_stats_read(b->radio).gatt_reads);
    puts("psm: advertised PSM dialled, refusal forgets it and falls back, timeout keeps it, GATT PSM read for UUID-only adverts: ok");
}
/* mod.rs pubkey exchange tests and tests/admission.rs */
static void exchange_body(void)
{
    fipsd_ble_config c = fast_cfg(); c.scan = false;
    tnode *a = tnode_new(1, NULL, &c, true); tnode_start(a); await_free(a, FIPSD_TRANSPORT_BECAME_PRESENT, NULL);
    uint8_t got[4096]; size_t len;
    /* a bad prefix fails the exchange and nothing is pooled */
    raw *r1 = raw_new(20, NULL, 20); fipsd_ble_stream *s1 = raw_connect(r1, a, 0);
    raw_hello(r1, s1, 0x01);
    assert(raw_until_closed(s1, got, sizeof got, &len)); raw_close(s1);
    assert(stats(a).pubkey_exchange_failures == 1 && !fipsd_ble_connected_count(a->t));
    /* the exchange preserves a frame the peer coalesced behind it */
    raw *r2 = raw_new(21, NULL, 21); fipsd_ble_stream *s2 = raw_connect(r2, a, 0);
    uint8_t m[33 + 64] = {0}; memcpy(m + 1, r2->key.bytes, 32); frame(m + 33, 64, 0x42);
    assert(s2->ops->write(s2, m, sizeof m, ble_now() + WAIT_MS) == 0);
    fips_transport_addr r2a; assert(fipsd_ble_peer_to_addr(&r2->peer, &r2a));
    /* the announcement and the frame race each other out of the pool */
    for (bool announced = false, delivered = false; !announced || !delivered;) {
        fipsd_transport_event *e = take(a->events, WAIT_MS); assert(e);
        if (e->kind == FIPSD_TRANSPORT_DISCOVERED && fips_transport_addr_equal(&e->address, &r2a)) {
            assert(!announced && e->size == 32 && !memcmp(e->bytes, r2->key.bytes, 32)); announced = true;
        } else if (e->kind == FIPSD_TRANSPORT_PACKET && fips_transport_addr_equal(&e->address, &r2a)) {
            assert(!delivered && e->size == 64 && !memcmp(e->bytes, m + 33, 64)); delivered = true;
        }
        free(e);
    }
    ssize_t n = s2->ops->read(s2, got, sizeof got, ble_now() + WAIT_MS);
    assert(n == 33 && got[0] == 0 && !memcmp(got + 1, a->key.bytes, 32));
    assert(stats(a).connections_accepted == 1);
    /* a fragmented exchange is reassembled */
    fipsd_ble_radio_config bytes = radio_cfg(22); bytes.mode = FIPSD_BLE_RADIO_BYTES; bytes.chunk = 5;
    raw *r3 = raw_new(22, &bytes, 22); fipsd_ble_stream *s3 = raw_connect(r3, a, 0);
    raw_hello(r3, s3, 0);
    fips_transport_addr r3a; assert(fipsd_ble_peer_to_addr(&r3->peer, &r3a));
    await_free(a, FIPSD_TRANSPORT_DISCOVERED, &r3a);
    /* a silent connector does not delay the next accept, and times out itself */
    raw *r4 = raw_new(23, NULL, 23); fipsd_ble_stream *s4 = raw_connect(r4, a, 0);
    raw *r5 = raw_new(24, NULL, 24); fipsd_ble_stream *s5 = raw_connect(r5, a, 0);
    raw_hello(r5, s5, 0);
    fips_transport_addr r5a; assert(fipsd_ble_peer_to_addr(&r5->peer, &r5a));
    await_free(a, FIPSD_TRANSPORT_DISCOVERED, &r5a);
    uint64_t failures = stats(a).pubkey_exchange_failures;
    assert(raw_until_closed(s4, NULL, 0, NULL)); raw_close(s4);
    assert(stats(a).pubkey_exchange_failures == failures + 1);
    /* the exchange's send half has a deadline: a peer that never drains */
    fipsd_ble_radio_config slow = radio_cfg(25); slow.mode = FIPSD_BLE_RADIO_BYTES; slow.chunk = 4; slow.queue = 1;
    raw *r6 = raw_new(25, &slow, 25); fipsd_ble_stream *s6 = raw_connect(r6, a, 0);
    /* neither side reads: both writes stall on one-slot queues until the
     * transport's send deadline gives up and closes, failing ours */
    uint8_t hello[33] = {0}; memcpy(hello + 1, r6->key.bytes, 32);
    assert(s6->ops->write(s6, hello, sizeof hello, ble_now() + WAIT_MS) == -EPIPE); raw_close(s6);
    assert(stats(a).pubkey_exchange_failures == failures + 2);
    raw_close(s2); raw_close(s3); raw_close(s5);
    puts("exchange: bad prefix fails, coalesced frame kept, fragments reassembled, silent peer times out without blocking accepts, send deadline: ok");
}
static void flood_body(void)
{
    fipsd_ble_config c = fast_cfg(); c.scan = false; c.pubkey_exchange_ms = 60000; c.max_connections = 16;
    tnode *a = tnode_new(1, NULL, &c, true); tnode_start(a); await_free(a, FIPSD_TRANSPORT_BECAME_PRESENT, NULL);
    fipsd_ble_stream *silent[10];
    for (uint8_t i = 0; i < 10; ++i) silent[i] = raw_connect(raw_new((uint8_t)(30 + i), NULL, (uint8_t)(30 + i)), a, 0);
    /* past the in-flight budget the oldest pending handshakes are aborted */
    assert(raw_until_closed(silent[0], NULL, 0, NULL) && raw_until_closed(silent[1], NULL, 0, NULL));
    assert(stats(a).handshakes_aborted == 2 && !stats(a).pubkey_exchange_failures);
    raw *good = raw_new(50, NULL, 50); fipsd_ble_stream *g = raw_connect(good, a, 0); raw_hello(good, g, 0);
    fips_transport_addr ga; assert(fipsd_ble_peer_to_addr(&good->peer, &ga));
    await_free(a, FIPSD_TRANSPORT_DISCOVERED, &ga);
    assert(raw_until_closed(silent[2], NULL, 0, NULL) && stats(a).handshakes_aborted == 3);
    /* inbound peers within the budget are all admitted */
    fipsd_ble_stream *admitted[4]; /* connect hands each dialler's end to us: closed below */
    for (uint8_t i = 0; i < 4; ++i) {
        raw *x = raw_new((uint8_t)(60 + i), NULL, (uint8_t)(60 + i)); admitted[i] = raw_connect(x, a, 0); raw_hello(x, admitted[i], 0);
        fips_transport_addr xa; assert(fipsd_ble_peer_to_addr(&x->peer, &xa)); await_free(a, FIPSD_TRANSPORT_DISCOVERED, &xa);
    }
    assert(fipsd_ble_connected_count(a->t) == 5);
    for (int i = 0; i < 10; ++i) raw_close(silent[i]);
    for (int i = 0; i < 4; ++i) raw_close(admitted[i]);
    raw_close(g);
    puts("admission: a flood of silent connectors aborts the oldest past the budget of 8, never delays a well-behaved peer: ok");
}
/* mod.rs claim/verification tests, tests/close.rs, FRC-1301 6.5.4 */
static bool held_all(void *c, const fips_transport_addr *a) { (void)c; (void)a; return true; }
static bool held_none(void *c, const fips_transport_addr *a) { (void)c; (void)a; return false; }
static void claims_body(void)
{
    fipsd_ble_config c = fast_cfg(); c.scan = false; c.max_connections = 2;
    tnode *a = tnode_new(1, NULL, &c, true); tnode_start(a); await_free(a, FIPSD_TRANSPORT_BECAME_PRESENT, NULL);
    raw *r1 = raw_new(20, NULL, 70), *r2 = raw_new(21, NULL, 70), *r3 = raw_new(22, NULL, 70), *r4 = raw_new(23, NULL, 71);
    fips_transport_addr a1, a2, a3, a4;
    assert(fipsd_ble_peer_to_addr(&r1->peer, &a1) && fipsd_ble_peer_to_addr(&r2->peer, &a2) &&
        fipsd_ble_peer_to_addr(&r3->peer, &a3) && fipsd_ble_peer_to_addr(&r4->peer, &a4));
    fipsd_ble_stream *s1 = raw_connect(r1, a, 0); raw_hello(r1, s1, 0); await_free(a, FIPSD_TRANSPORT_DISCOVERED, &a1);
    /* an unverified claim declines nothing: the claimed node's next link is admitted beside it */
    fipsd_ble_stream *s2 = raw_connect(r2, a, 0); raw_hello(r2, s2, 0); await_free(a, FIPSD_TRANSPORT_DISCOVERED, &a2);
    assert(fipsd_ble_connected_count(a->t) == 2 && !stats(a).duplicate_node_declines);
    assert(fipsd_ble_is_verified(a->t, &a1) == 0);
    fipsd_ble_mark_verified(a->t, &a1, &r1->node);
    assert(fipsd_ble_is_verified(a->t, &a1) == 1);
    /* a verified link declines a newcomer to its node, announced at the verified address */
    fipsd_ble_stream *s3 = raw_connect(r3, a, 0); raw_hello(r3, s3, 0);
    fipsd_transport_event *e = await(a, FIPSD_TRANSPORT_DISCOVERED, NULL);
    assert(fips_transport_addr_equal(&e->address, &a1) && e->size == 32 && !memcmp(e->bytes, r1->key.bytes, 32)); free(e);
    assert(raw_until_closed(s3, NULL, 0, NULL) && stats(a).duplicate_node_declines == 1); raw_close(s3);
    /* a full pool (verified + one inside its grace) refuses, unannounced */
    fipsd_ble_stream *s4 = raw_connect(r4, a, 0); raw_hello(r4, s4, 0);
    assert(raw_until_closed(s4, NULL, 0, NULL) && stats(a).connections_rejected == 1); raw_close(s4);
    /* close_unproven: a held channel stays, an unheld unproven one goes, a verified one never */
    assert(fipsd_ble_close_unproven(a->t, 60000, held_all, NULL) == 0 && fipsd_ble_connected_count(a->t) == 2);
    assert(a->t->ops->close_unproven(a->t, 0, held_none, NULL) == 1);
    assert(raw_until_closed(s2, NULL, 0, NULL)); raw_close(s2);
    await_pooled(a, 1); assert(fipsd_ble_is_verified(a->t, &a1) == 1);
    /* the node's peer hook: cleared (removed) and verified again */
    a->t->ops->peer_link(a->t, &a1, &r1->node, false, true); assert(fipsd_ble_is_verified(a->t, &a1) == 0);
    a->t->ops->peer_link(a->t, &a1, &r1->node, true, false); assert(fipsd_ble_is_verified(a->t, &a1) == 1);
    /* a deliberate close still sends what was queued, then closes */
    uint8_t want[96]; frame(want, 96, 0x33);
    assert(a->t->ops->send(a->t, &a1, want, sizeof want) == 0);
    a->t->ops->close_connection(a->t, &a1);
    uint8_t got[4096]; size_t len = 0;
    assert(raw_until_closed(s1, got, sizeof got, &len) && len == 33 + 96 && !memcmp(got + 33, want, 96)); raw_close(s1);
    await_pooled(a, 0);
    puts("claims: unverified claims admit, verified declines and re-announces, full pool refuses, close_unproven, drain on close: ok");
}
/* io_radio.rs bridge semantics and io.rs mock tests, against raw radios */
typedef struct dial_arg { raw *from; fipsd_ble_peer to; uint16_t psm; GoChannel *done; int rc; fipsd_ble_stream *s; } dial_arg;
static void dial_fiber_test(void *p)
{
    dial_arg *d = p;
    d->rc = d->from->b.ops->connect(d->from->b.self, &d->to, d->psm, ble_now() + WAIT_MS, &d->s);
    assert(go_channel_send(d->done, d) == 0);
}
static fipsd_ble_air *air2;
static fipsd_ble_radio *stray;
static void radio_body(void)
{
    raw *x = raw_new(1, NULL, 1), *y = raw_new(2, NULL, 2), *z = raw_new(3, NULL, 3);
    uint16_t bound = 0;
    assert(x->b.ops->listen(x->b.self, 0x0085, ble_now() + WAIT_MS, &bound) == 0 && bound == 0x0085);
    assert(z->b.ops->listen(z->b.self, 0x0085, ble_now() + WAIT_MS, &bound) == 0);
    fipsd_ble_adapter ad = x->b.ops->adapter(x->b.self); assert(ad.present && ad.powered);
    fipsd_ble_peer local; assert(x->b.ops->local_addr(x->b.self, &local) && fipsd_ble_peer_equal(&local, &x->peer));
    assert(!strcmp(x->b.ops->adapter_name(x->b.self), FIPSD_BLE_RADIO_ADAPTER));
    /* bytes round trip, the remote address, an unknown MTU falls back */
    fipsd_ble_stream *ys = NULL;
    assert(y->b.ops->connect(y->b.self, &x->peer, 0x0085, ble_now() + WAIT_MS, &ys) == 0 && fipsd_ble_peer_equal(&ys->peer, &x->peer));
    fipsd_ble_event *in = raw_event(x, FIPSD_BLE_INBOUND_STREAM);
    fipsd_ble_stream *xs = in->stream; free(in);
    assert(fipsd_ble_peer_equal(&xs->peer, &y->peer) && xs->ops->fd(xs) == -1);
    uint16_t sm, rm; ble_stream_mtu(xs, 2048, &sm, &rm); assert(sm == 2048 && rm == 2048);
    uint8_t buf[64];
    assert(ys->ops->write(ys, (const uint8_t *)"hello", 5, ble_now() + WAIT_MS) == 0);
    assert(xs->ops->read(xs, buf, sizeof buf, ble_now() + WAIT_MS) == 5 && !memcmp(buf, "hello", 5));
    assert(xs->ops->write(xs, (const uint8_t *)"world", 5, ble_now() + WAIT_MS) == 0);
    assert(ys->ops->read(ys, buf, sizeof buf, ble_now() + WAIT_MS) == 5 && !memcmp(buf, "world", 5));
    /* an idle channel times out; a closed one reads as end of stream */
    assert(xs->ops->read(xs, buf, sizeof buf, ble_now() + 20) == -ETIMEDOUT);
    raw_close(ys);
    assert(xs->ops->read(xs, buf, sizeof buf, ble_now() + WAIT_MS) == 0);
    assert(xs->ops->write(xs, buf, 1, ble_now() + WAIT_MS) == -EPIPE);
    raw_close(xs);
    /* a full outbound queue backpressures rather than growing */
    fipsd_ble_radio_config one = radio_cfg(4); one.queue = 1;
    raw *w = raw_new(4, &one, 4);
    fipsd_ble_stream *ws = raw_connect(w, &(tnode){.peer = x->peer}, 0x0085);
    in = raw_event(x, FIPSD_BLE_INBOUND_STREAM); xs = in->stream; free(in);
    assert(ws->ops->write(ws, buf, 8, ble_now() + WAIT_MS) == 0);
    assert(ws->ops->write(ws, buf, 8, ble_now() + 30) == -ETIMEDOUT);
    assert(xs->ops->read(xs, buf, sizeof buf, ble_now() + WAIT_MS) == 8);
    assert(ws->ops->write(ws, buf, 8, ble_now() + WAIT_MS) == 0);
    /* cancel latches -ECANCELED for pending and later I/O */
    ws->ops->cancel(ws); assert(ws->ops->read(ws, buf, sizeof buf, FIPSD_BLE_NO_DEADLINE) == -ECANCELED);
    raw_close(ws); raw_close(xs);
    /* a refused dial surfaces as an error; concurrent dials resolve to their own peers */
    fipsd_ble_stream *bad = NULL;
    assert(y->b.ops->connect(y->b.self, &x->peer, 0x0099, ble_now() + WAIT_MS, &bad) == -ECONNREFUSED && !bad);
    dial_arg d1 = {y, x->peer, 0x0085, go_channel_create(1), -1, NULL}, d2 = {y, z->peer, 0x0085, go_channel_create(1), -1, NULL};
    assert(!fipsd_runtime_spawn(rt, dial_fiber_test, &d1) && !fipsd_runtime_spawn(rt, dial_fiber_test, &d2));
    assert(take(d1.done, WAIT_MS) == &d1 && take(d2.done, WAIT_MS) == &d2 && !d1.rc && !d2.rc);
    assert(fipsd_ble_peer_equal(&d1.s->peer, &x->peer) && fipsd_ble_peer_equal(&d2.s->peer, &z->peer));
    raw_close(d1.s); raw_close(d2.s); go_channel_unref(d1.done); go_channel_unref(d2.done);
    in = raw_event(x, FIPSD_BLE_INBOUND_STREAM); raw_close(in->stream); free(in);
    in = raw_event(z, FIPSD_BLE_INBOUND_STREAM); raw_close(in->stream); free(in);
    /* a dial the caller abandons is reclaimed; its late answer allocates nothing */
    fipsd_ble_radio_set_stall(y->radio, true);
    assert(y->b.ops->connect(y->b.self, &x->peer, 0x0085, ble_now() + 30, &bad) == -ETIMEDOUT && !bad);
    fipsd_ble_radio_set_stall(y->radio, false);
    assert(y->b.ops->connect(y->b.self, &x->peer, 0x0085, ble_now() + WAIT_MS, &bad) == 0);
    raw_close(bad); in = raw_event(x, FIPSD_BLE_INBOUND_STREAM); raw_close(in->stream); free(in);
    /* a dial abandoned before the radio answers it with a stream: the late
     * answer drops the last reference, closing that stream, off the air lock
     * (closing under it deadlocked the control thread), and the radio serves on */
    size_t queued = 0;
    for (int i = 0; i < 24; ++i) {
        bad = NULL;
        int rc = y->b.ops->connect(y->b.self, &x->peer, 0x0085, ble_now(), &bad);
        if (rc != -EAGAIN) ++queued;
        if (!rc) raw_close(bad);
    }
    assert(y->b.ops->connect(y->b.self, &x->peer, 0x0085, ble_now() + WAIT_MS, &bad) == 0);
    raw_close(bad);
    for (size_t i = 0; i <= queued; ++i) { in = raw_event(x, FIPSD_BLE_INBOUND_STREAM); raw_close(in->stream); free(in); }
    /* scan adverts carry the advertised PSM; the same PSM does not restart the advertiser */
    assert(y->b.ops->scan_start(y->b.self, ble_now() + WAIT_MS) == 0);
    fipsd_ble_advert adv = {.psm = 0x00C1}; fipsd_ble_encode_psm(adv.psm, adv.service_data);
    assert(x->b.ops->advertise(x->b.self, &adv, ble_now() + WAIT_MS) == 0);
    fipsd_ble_event *e = raw_event(y, FIPSD_BLE_PEER_DISCOVERED);
    uint16_t psm = 0;
    assert(fipsd_ble_peer_equal(&e->peer, &x->peer) && fipsd_ble_decode_psm(e->service_data, e->service_data_len, &psm) && psm == 0x00C1 && e->has_rssi);
    free(e);
    assert(x->b.ops->advertise(x->b.self, &adv, ble_now() + WAIT_MS) == 0 && !fipsd_ble_radio_stats_read(x->radio).advertise_restarts);
    /* a listener opened under a live advert moves it onto the bound PSM, passed at once */
    fipsd_ble_radio_set_assigned_psm(x->radio, 0x00D1);
    assert(x->b.ops->listen(x->b.self, 0x0085, ble_now() + WAIT_MS, &bound) == 0 && bound == 0x00D1);
    e = raw_event(y, FIPSD_BLE_PEER_DISCOVERED);
    assert(fipsd_ble_decode_psm(e->service_data, e->service_data_len, &psm) && psm == 0x00D1); free(e);
    assert(fipsd_ble_radio_stats_read(x->radio).advertise_restarts == 1);
    /* an advert withdrawn reaches the scanner as PEER_LOST; an injected one as is */
    assert(x->b.ops->advertise_stop(x->b.self) == 0);
    e = raw_event(y, FIPSD_BLE_PEER_LOST); assert(fipsd_ble_peer_equal(&e->peer, &x->peer)); free(e);
    fipsd_ble_peer ghost = peer_n("ble0", 0x99);
    fipsd_ble_radio_inject_advert(y->radio, &ghost, NULL, 0);
    e = raw_event(y, FIPSD_BLE_PEER_DISCOVERED); assert(fipsd_ble_peer_equal(&e->peer, &ghost) && !e->service_data_len); free(e);
    /* two airs do not interfere: the same device address elsewhere is unreachable from here */
    air2 = fipsd_ble_air_create(); fipsd_ble_radio_config twin = radio_cfg(1);
    stray = fipsd_ble_radio_create(air2, &twin); assert(stray);
    fipsd_ble_backend sb = fipsd_ble_radio_backend(stray);
    fipsd_ble_sink ss = {.events = go_channel_create(8), .adapter_wake = go_channel_create(1)}; atomic_store(&ss.open, true);
    assert(sb.ops->start(sb.self, rt, &ss) == 0);
    assert(sb.ops->connect(sb.self, &y->peer, 0x0085, ble_now() + 30, &bad) == -ETIMEDOUT);
    /* a powered-off adapter fails parked operations and refuses new ones */
    fipsd_ble_radio_set_stall(y->radio, true);
    dial_arg d3 = {y, z->peer, 0x0085, go_channel_create(1), 0, NULL};
    assert(!fipsd_runtime_spawn(rt, dial_fiber_test, &d3));
    fipsd_ble_radio_set_adapter(y->radio, true, false);
    assert(take(d3.done, WAIT_MS) == &d3 && (d3.rc == -ENODEV || d3.rc == -ETIMEDOUT) && !d3.s); go_channel_unref(d3.done);
    assert(y->b.ops->scan_start(y->b.self, ble_now() + WAIT_MS) == -ENODEV);
    /* stop cancels whatever is parked */
    sb.ops->stop(sb.self);
    assert(sb.ops->connect(sb.self, &y->peer, 0x0085, ble_now() + WAIT_MS, &bad) == -ECANCELED);
    sb.ops->destroy(sb.self); stray = NULL;
    void *v; while (go_channel_try_receive(ss.events, &v) == 0) free(v);
    go_channel_unref(ss.events); go_channel_unref(ss.adapter_wake);
    fipsd_ble_air_destroy(air2); air2 = NULL;
    puts("radio: listen/connect/round trip, MTU fallback, timeout/EOF/EPIPE, backpressure, cancel, refusal, concurrent and abandoned dials, adverts, PSM moves, LOST, two airs, power-off, stop: ok");
}
int main(void)
{
    (void)fipsd_test_tmpdir(NULL);
    assert(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    psm_wire(); addresses(); pool_rules(); arbitration(); probe_book(); recall_bounds(); reader_rules(); stats_and_config();
    scenario(radio_body);
    scenario(lifecycle_body);
    scenario(discovery_body);
    scenario(pair_sdu_body);
    scenario(pair_bytes_body);
    scenario(pair_fd_body);
    scenario(reconnect_body);
    scenario(psm_body);
    scenario(exchange_body);
    scenario(flood_body);
    scenario(claims_body);
    fips_identity_destroy_secp(secp);
    puts("ble: ok");
    return 0;
}
