/* Native API flow registry: port ownership, the delivery rule, the backlog,
 * the node ceiling, the per-peer quarter share, pending-flow expiry and the
 * ephemeral port search, as table tests over the pure registry (upstream
 * src/native/registry.rs inline tests, tests/registry.rs and
 * tests/registry_share.rs). No socket, no node, no clock. */
#include "native/registry.h"
#include "native/protocol.h"
#include "host_os.h"
#include <fips/identity/local.h>
#include <fips/identity/node_addr.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

static fips_secp *secp;
static fipsd_native_limits limits(void) { return (fipsd_native_limits){.per_flow = 4, .backlog = 2, .max_flows = 8}; }
/* A distinct peer key per byte; the address is derived from it. */
static fips_xonly_pubkey key(uint8_t byte)
{
    uint8_t secret[32]; memset(secret, byte, sizeof secret);
    fips_identity *id = NULL;
    CHECK(fips_identity_from_secret_bytes(secp, fipsd_host_os_entropy(), secret, &id) == FIPS_IDENTITY_OK);
    fips_xonly_pubkey k = fips_identity_pubkey(id);
    fips_identity_destroy(id);
    return k;
}
static fips_node_addr peer(uint8_t byte) { fips_xonly_pubkey k = key(byte); return fips_node_addr_from_pubkey(&k); }
static int sink_a, sink_b, arrivals_a; /* opaque sink identities */
static fipsd_native_registry *registry(void) { fipsd_native_registry *r = fipsd_native_registry_create(&(fipsd_native_limits){4, 2, 8}, NULL, NULL); CHECK(r); return r; }
static fipsd_native_registry_error listen_on(fipsd_native_registry *r, uint16_t port, uint16_t *held)
{ uint16_t got; return fipsd_native_registry_listen(r, true, port, &arrivals_a, held ? held : &got, NULL); }
static fipsd_native_registry_error connect_to(fipsd_native_registry *r, uint8_t who, uint16_t remote, bool has_local, uint16_t local, uint16_t *held, uint64_t *id)
{
    fips_xonly_pubkey k = key(who); uint16_t port; uint64_t flow;
    return fipsd_native_registry_connect(r, &k, remote, has_local, local, &sink_a, 0, held ? held : &port, id ? id : &flow, NULL);
}
static fipsd_native_delivery deliver(fipsd_native_registry *r, uint8_t who, uint16_t src, uint16_t dst, uint64_t now)
{ fips_node_addr a = peer(who); fips_xonly_pubkey k = key(who); return fipsd_native_registry_deliver(r, &a, &k, src, dst, now); }
static uint64_t arrive(fipsd_native_registry *r, uint8_t who, uint16_t src, uint16_t dst)
{ fipsd_native_delivery d = deliver(r, who, src, dst, 0); CHECK(d.kind == FIPSD_NATIVE_DELIVER_ARRIVED); return d.flow; }
static fipsd_native_flow_key accept(fipsd_native_registry *r, uint64_t flow)
{
    fipsd_native_flow_key k; fips_xonly_pubkey pk; fipsd_native_datagram **held; size_t n;
    CHECK(fipsd_native_registry_accept(r, flow, &sink_b, 0, &k, &pk, &held, &n) == FIPSD_NATIVE_REG_OK);
    for (size_t i = 0; i < n; ++i) fipsd_native_datagram_free(held[i]);
    free(held);
    return k;
}
static bool hold(fipsd_native_registry *r, uint64_t flow, const char *text)
{
    fipsd_native_datagram *d = fipsd_native_datagram_new((const uint8_t *)text, strlen(text)); CHECK(d);
    if (fipsd_native_registry_hold(r, flow, d)) return true;
    fipsd_native_datagram_free(d); return false;
}

static void test_a_port_has_one_owner(void)
{
    fipsd_native_registry *r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_PORT_TAKEN);
    CHECK(connect_to(r, 1, 5000, true, 4242, NULL, NULL) == FIPSD_NATIVE_REG_PORT_TAKEN);
    /* The command layer refuses reserved tiers too; the registry never binds the shim's port. */
    CHECK(listen_on(r, 256, NULL) == FIPSD_NATIVE_REG_PORT_STANDARD);
    CHECK(connect_to(r, 1, 256, true, 4243, NULL, NULL) == FIPSD_NATIVE_REG_PORT_STANDARD);
    CHECK(connect_to(r, 1, 5000, true, 200, NULL, NULL) == FIPSD_NATIVE_REG_PORT_PROTOCOL);
    fipsd_native_registry_destroy(r);
}
static void test_releasing_a_flow_frees_its_port(void)
{
    fipsd_native_registry *r = registry(); uint16_t port;
    CHECK(connect_to(r, 1, 5000, true, 4242, &port, NULL) == FIPSD_NATIVE_REG_OK && port == 4242);
    CHECK(fipsd_native_registry_port_count(r) == 1);
    fipsd_native_flow_key k = {.peer = peer(1), .remote = 5000, .local = 4242};
    CHECK(fipsd_native_registry_release(r, &k));
    CHECK(fipsd_native_registry_port_count(r) == 0 && fipsd_native_registry_flow_count(r) == 0);
    CHECK(!fipsd_native_registry_release(r, &k));
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    fipsd_native_registry_destroy(r);
}
static void test_ephemeral_ports(void)
{
    fipsd_native_registry *r = registry(); uint16_t seen[4];
    for (size_t i = 0; i < 4; ++i) {
        CHECK(connect_to(r, 1, 5000, false, 0, &seen[i], NULL) == FIPSD_NATIVE_REG_OK);
        CHECK(seen[i] >= FIPSD_NATIVE_PORT_EPHEMERAL_MIN);
        for (size_t j = 0; j < i; ++j) CHECK(seen[j] != seen[i]);
    }
    fipsd_native_registry_destroy(r);
    /* A listener that names no port is given an ephemeral one and told which. */
    r = registry(); uint16_t port;
    CHECK(fipsd_native_registry_listen(r, false, 0, &arrivals_a, &port, NULL) == FIPSD_NATIVE_REG_OK);
    CHECK(port >= FIPSD_NATIVE_PORT_EPHEMERAL_MIN);
    CHECK(listen_on(r, port, NULL) == FIPSD_NATIVE_REG_PORT_TAKEN);
    fipsd_native_registry_destroy(r);
}
static void test_an_established_flow_wins_over_a_listener(void)
{
    fipsd_native_registry *r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    uint64_t flow = arrive(r, 7, 5000, 4242);
    (void)accept(r, flow);
    fipsd_native_delivery d = deliver(r, 7, 5000, 4242, 0);
    CHECK(d.kind == FIPSD_NATIVE_DELIVER_FLOW && d.sink == &sink_b && d.flow == flow);
    fipsd_native_registry_destroy(r);
}
static fipsd_native_registry *accepted_flow_on_a_closed_listener(void)
{
    fipsd_native_registry *r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    (void)accept(r, arrive(r, 7, 5000, 4242));
    fipsd_native_registry_release_listener(r, 4242);
    return r;
}
static void test_a_flow_outlives_its_listener_and_keeps_receiving_after_a_rebind(void)
{
    fipsd_native_registry *r = accepted_flow_on_a_closed_listener();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK); /* the flow never owned the port */
    CHECK(deliver(r, 7, 5000, 4242, 0).kind == FIPSD_NATIVE_DELIVER_FLOW);
    CHECK(deliver(r, 8, 5001, 4242, 0).kind == FIPSD_NATIVE_DELIVER_ARRIVED);
    fipsd_native_registry_destroy(r);
    r = accepted_flow_on_a_closed_listener();
    CHECK(fipsd_native_registry_port_count(r) == 0);
    fipsd_native_refusal why; uint16_t port; uint64_t id; fips_xonly_pubkey k = key(7);
    CHECK(fipsd_native_registry_connect(r, &k, 5000, true, 4242, &sink_a, 0, &port, &id, &why) == FIPSD_NATIVE_REG_FLOW_TAKEN);
    CHECK(why.local == 4242 && why.remote == 5000);
    CHECK(fipsd_native_registry_port_count(r) == 0);
    CHECK(deliver(r, 7, 5000, 4242, 0).kind == FIPSD_NATIVE_DELIVER_FLOW);
    fipsd_native_registry_destroy(r);
}
static void test_arrivals_and_drops(void)
{
    fipsd_native_registry *r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    uint64_t first = arrive(r, 1, 5000, 4242), second = arrive(r, 2, 5000, 4242);
    CHECK(first != second);
    /* The backlog is two, so a third distinct peer is refused. */
    fipsd_native_delivery d = deliver(r, 3, 5000, 4242, 0);
    CHECK(d.kind == FIPSD_NATIVE_DELIVER_DROP && d.cause == FIPSD_NATIVE_DROP_BACKLOG_FULL);
    /* A peer already announced holds rather than announcing again. */
    d = deliver(r, 1, 5000, 4242, 0);
    CHECK(d.kind == FIPSD_NATIVE_DELIVER_PENDING && d.flow == first);
    d = deliver(r, 1, 5000, 9999, 0);
    CHECK(d.kind == FIPSD_NATIVE_DELIVER_DROP && d.cause == FIPSD_NATIVE_DROP_NO_PORT);
    fipsd_native_listener_view lv[2];
    CHECK(fipsd_native_registry_listeners(r, lv, 2) == 1 && lv[0].port == 4242 && lv[0].backlog == 2);
    fipsd_native_registry_destroy(r);
}
static void test_accepting_returns_what_the_flow_held(void)
{
    fipsd_native_registry *r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    uint64_t flow = arrive(r, 1, 5000, 4242);
    CHECK(hold(r, flow, "first"));
    fipsd_native_flow_key k; fips_xonly_pubkey pk; fipsd_native_datagram **held; size_t n;
    CHECK(fipsd_native_registry_accept(r, flow, &sink_b, 0, &k, &pk, &held, &n) == FIPSD_NATIVE_REG_OK);
    fips_node_addr p1 = peer(1); fips_xonly_pubkey k1 = key(1);
    CHECK(!memcmp(k.peer.bytes, p1.bytes, 16) && !memcmp(pk.bytes, k1.bytes, 32));
    CHECK(n == 1 && held[0]->len == 5 && !memcmp(held[0]->bytes, "first", 5));
    fipsd_native_datagram_free(held[0]); free(held);
    CHECK(fipsd_native_registry_accept(r, flow, &sink_b, 0, &k, &pk, &held, &n) == FIPSD_NATIVE_REG_NO_PENDING);
    fipsd_native_registry_destroy(r);
}
static void test_a_pending_flow_holds_no_more_than_its_share(void)
{
    fipsd_native_registry *r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    uint64_t flow = arrive(r, 1, 5000, 4242);
    for (size_t i = 0; i < limits().per_flow; ++i) CHECK(hold(r, flow, "x"));
    CHECK(!hold(r, flow, "x"));
    CHECK(!hold(r, 999, "x"));
    fipsd_native_flow_view v[8];
    CHECK(fipsd_native_registry_flows(r, v, 8, NULL, NULL) == 1 && !v[0].established && v[0].queued == 4);
    fipsd_native_registry_destroy(r);
}
static void test_rejecting_frees_the_backlog_slot(void)
{
    fipsd_native_registry *r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    uint64_t flow = arrive(r, 1, 5000, 4242);
    fipsd_native_flow_key k;
    CHECK(fipsd_native_registry_reject(r, flow, &k) == FIPSD_NATIVE_REG_OK && k.remote == 5000);
    CHECK(fipsd_native_registry_flow_count(r) == 0);
    CHECK(fipsd_native_registry_reject(r, flow, &k) == FIPSD_NATIVE_REG_NO_PENDING);
    fipsd_native_registry_destroy(r);
}
static void test_a_pending_flow_expires_and_a_fresh_one_does_not(void)
{
    fipsd_native_registry *r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    fipsd_native_delivery d = deliver(r, 1, 5000, 4242, 1000);
    CHECK(d.kind == FIPSD_NATIVE_DELIVER_ARRIVED);
    uint64_t expired[4];
    CHECK(fipsd_native_registry_expire(r, 1000 + FIPSD_NATIVE_PENDING_DEADLINE_MS - 1, expired, 4) == 0);
    CHECK(fipsd_native_registry_expire(r, 1000 + FIPSD_NATIVE_PENDING_DEADLINE_MS, expired, 4) == 1 && expired[0] == d.flow);
    CHECK(fipsd_native_registry_flow_count(r) == 0);
    fipsd_native_listener_view lv; CHECK(fipsd_native_registry_listeners(r, &lv, 1) == 1 && lv.backlog == 0);
    fipsd_native_registry_destroy(r);
}
static void test_releasing_a_listener_takes_its_pending_flows_with_it(void)
{
    fipsd_native_registry *r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    uint64_t flow = arrive(r, 1, 5000, 4242);
    CHECK(hold(r, flow, "held"));
    CHECK(fipsd_native_registry_flow_count(r) == 1);
    fipsd_native_registry_release_listener(r, 4242);
    CHECK(fipsd_native_registry_flow_count(r) == 0 && fipsd_native_registry_port_count(r) == 0);
    fipsd_native_registry_destroy(r);
}
static void test_the_node_flow_ceiling_holds(void)
{
    fipsd_native_registry *r = registry();
    for (size_t i = 0; i < limits().max_flows; ++i) CHECK(connect_to(r, 1, 5000, false, 0, NULL, NULL) == FIPSD_NATIVE_REG_OK);
    fipsd_native_refusal why; uint16_t port; uint64_t id; fips_xonly_pubkey k = key(1);
    CHECK(fipsd_native_registry_connect(r, &k, 5000, false, 0, &sink_a, 0, &port, &id, &why) == FIPSD_NATIVE_REG_TOO_MANY_FLOWS);
    CHECK(why.max == 8);
    fipsd_native_registry_destroy(r);
}
static size_t queued_of(void *sink, void *user) { (void)user; return sink == &sink_a ? 1 : 0; }
static void test_the_flow_view(void)
{
    fipsd_native_registry *r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    uint64_t opened; fips_xonly_pubkey k1 = key(1); uint16_t port;
    CHECK(fipsd_native_registry_connect(r, &k1, 5000, true, 6000, &sink_a, 1000, &port, &opened, NULL) == FIPSD_NATIVE_REG_OK);
    fipsd_native_delivery d = deliver(r, 2, 5001, 4242, 2000);
    CHECK(d.kind == FIPSD_NATIVE_DELIVER_ARRIVED);
    CHECK(hold(r, d.flow, "held"));
    fipsd_native_flow_view v[4];
    CHECK(fipsd_native_registry_flows(r, v, 4, queued_of, NULL) == 2);
    CHECK(v[0].flow == opened && v[0].established && v[0].key.local == 6000 && v[0].key.remote == 5000 && v[0].queued == 1 && v[0].at == 1000);
    fips_node_addr p2 = peer(2); fips_xonly_pubkey k2 = key(2);
    CHECK(v[1].flow == d.flow && !v[1].established && !memcmp(v[1].key.peer.bytes, p2.bytes, 16) && v[1].queued == 1 && v[1].at == 2000);
    CHECK(!memcmp(v[0].pubkey.bytes, k1.bytes, 32) && !memcmp(v[1].pubkey.bytes, k2.bytes, 32));
    fipsd_native_flow_key key_of; CHECK(fipsd_native_registry_key_of(r, opened, &key_of) && key_of.local == 6000);
    CHECK(!fipsd_native_registry_key_of(r, d.flow, &key_of));
    CHECK(listen_on(r, 4243, NULL) == FIPSD_NATIVE_REG_OK);
    fipsd_native_listener_view lv[2];
    CHECK(fipsd_native_registry_listeners(r, lv, 2) == 2 && lv[0].port == 4242 && lv[0].backlog == 1 && lv[1].port == 4243 && lv[1].backlog == 0);
    fipsd_native_registry_destroy(r);
}
/* tests/registry_share.rs: one peer cannot take the whole flow table. */
static bool arrive_and_accept(fipsd_native_registry *r, uint8_t who, uint16_t src, fipsd_native_flow_key *out)
{
    fipsd_native_delivery d = deliver(r, who, src, 4242, 0);
    if (d.kind != FIPSD_NATIVE_DELIVER_ARRIVED) return false;
    fipsd_native_flow_key k = accept(r, d.flow);
    if (out) *out = k;
    return true;
}
static void test_peer_share(void)
{
    const size_t share = 2;
    CHECK(fipsd_native_limits_per_peer(&(fipsd_native_limits){4, 2, 8}) == share);
    CHECK(fipsd_native_limits_per_peer(&(fipsd_native_limits){4, 2, 2}) == 1);
    CHECK(fipsd_native_limits_per_peer(&(fipsd_native_limits){16, 16, 256}) == 64);
    /* One peer flooding leaves the node able to connect and to hear others. */
    fipsd_native_registry *r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    size_t accepted = 0;
    for (uint16_t src = 5000; src < 5000 + 8; ++src) if (arrive_and_accept(r, 7, src, NULL)) ++accepted;
    CHECK(accepted == share);
    CHECK(connect_to(r, 1, 6000, false, 0, NULL, NULL) == FIPSD_NATIVE_REG_OK);
    CHECK(deliver(r, 8, 5000, 4242, 0).kind == FIPSD_NATIVE_DELIVER_ARRIVED);
    fipsd_native_registry_destroy(r);
    /* A pending arrival counts against its peer's share, with a backlog as large as the table. */
    r = fipsd_native_registry_create(&(fipsd_native_limits){4, 8, 8}, NULL, NULL); CHECK(r);
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    for (uint16_t src = 5000; src < 5000 + share; ++src) CHECK(deliver(r, 7, src, 4242, 0).kind == FIPSD_NATIVE_DELIVER_ARRIVED);
    fipsd_native_delivery over = deliver(r, 7, 5000 + share, 4242, 0);
    CHECK(over.kind == FIPSD_NATIVE_DELIVER_DROP && over.cause == FIPSD_NATIVE_DROP_PEER_SHARE);
    fipsd_native_registry_destroy(r);
    /* A peer at its share is refused until one of its flows is released. */
    r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    fipsd_native_flow_key held[2];
    for (size_t i = 0; i < share; ++i) CHECK(arrive_and_accept(r, 7, (uint16_t)(5000 + i), &held[i]));
    over = deliver(r, 7, 6000, 4242, 0);
    CHECK(over.kind == FIPSD_NATIVE_DELIVER_DROP && over.cause == FIPSD_NATIVE_DROP_PEER_SHARE);
    CHECK(fipsd_native_registry_release(r, &held[0]));
    CHECK(deliver(r, 7, 6000, 4242, 0).kind == FIPSD_NATIVE_DELIVER_ARRIVED);
    fipsd_native_registry_destroy(r);
    /* Flows this node opened to a peer do not count against that peer's inbound share. */
    r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    for (size_t i = 0; i <= share; ++i) CHECK(connect_to(r, 7, 5000, false, 0, NULL, NULL) == FIPSD_NATIVE_REG_OK);
    CHECK(deliver(r, 7, 5000, 4242, 0).kind == FIPSD_NATIVE_DELIVER_ARRIVED);
    fipsd_native_registry_destroy(r);
    /* Over its share is dropped for its share and not for the node ceiling. */
    r = registry();
    CHECK(listen_on(r, 4242, NULL) == FIPSD_NATIVE_REG_OK);
    for (size_t i = 0; i < share; ++i) CHECK(arrive_and_accept(r, 7, (uint16_t)(5000 + i), NULL));
    over = deliver(r, 7, 6000, 4242, 0);
    CHECK(over.kind == FIPSD_NATIVE_DELIVER_DROP && over.cause == FIPSD_NATIVE_DROP_PEER_SHARE);
    uint8_t who = 8;
    while (fipsd_native_registry_flow_count(r) < limits().max_flows) { CHECK(arrive_and_accept(r, who, 5000, NULL)); ++who; }
    over = deliver(r, who, 5000, 4242, 0);
    CHECK(over.kind == FIPSD_NATIVE_DELIVER_DROP && over.cause == FIPSD_NATIVE_DROP_TOO_MANY_FLOWS);
    fipsd_native_registry_destroy(r);
}
/* tests/registry.rs: ephemeral allocation does not tell a peer what else the
 * node did. The first two are statistical with a 1/16384 chance per trial. */
#define SPAN (65536u - FIPSD_NATIVE_PORT_EPHEMERAL_MIN)
static uint32_t gap(uint16_t a, uint16_t b) { return ((uint32_t)b + SPAN - (uint32_t)a) % SPAN; }
static void test_ephemeral_ports_leak_no_count(void)
{
    fipsd_native_registry *r = registry();
    unsigned matches = 0;
    for (unsigned trial = 0; trial < 32; ++trial) {
        uint16_t a, b;
        CHECK(connect_to(r, 1, 5000, false, 0, &a, NULL) == FIPSD_NATIVE_REG_OK);
        for (unsigned i = 0; i < 10; ++i) {
            uint16_t other;
            CHECK(connect_to(r, 2, 5000, false, 0, &other, NULL) == FIPSD_NATIVE_REG_OK);
            fipsd_native_flow_key k = {.peer = peer(2), .remote = 5000, .local = other};
            CHECK(fipsd_native_registry_release(r, &k));
        }
        CHECK(connect_to(r, 1, 5001, false, 0, &b, NULL) == FIPSD_NATIVE_REG_OK);
        if (gap(a, b) == 11) ++matches;
        fipsd_native_flow_key ka = {.peer = peer(1), .remote = 5000, .local = a}, kb = {.peer = peer(1), .remote = 5001, .local = b};
        CHECK(fipsd_native_registry_release(r, &ka) && fipsd_native_registry_release(r, &kb));
    }
    CHECK(matches < 4);
    uint16_t ports[32]; unsigned successive = 0;
    for (unsigned i = 0; i < 32; ++i) {
        CHECK(fipsd_native_registry_listen(r, false, 0, &arrivals_a, &ports[i], NULL) == FIPSD_NATIVE_REG_OK);
        fipsd_native_registry_release_listener(r, ports[i]);
        if (i && gap(ports[i - 1], ports[i]) == 1) ++successive;
    }
    CHECK(successive < 4);
    fipsd_native_registry_destroy(r);
}
static void test_the_one_free_port_is_found_and_then_none_is(void)
{
    fipsd_native_registry *r = registry();
    const uint16_t FREE = FIPSD_NATIVE_PORT_EPHEMERAL_MIN;
    for (uint32_t port = FIPSD_NATIVE_PORT_EPHEMERAL_MIN; port <= 65535; ++port)
        if (port != FREE) CHECK(listen_on(r, (uint16_t)port, NULL) == FIPSD_NATIVE_REG_OK);
    for (unsigned round = 0; round < 64; ++round) {
        uint16_t got;
        CHECK(fipsd_native_registry_listen(r, false, 0, &arrivals_a, &got, NULL) == FIPSD_NATIVE_REG_OK && got == FREE);
        fipsd_native_registry_release_listener(r, FREE);
    }
    uint16_t got;
    CHECK(fipsd_native_registry_listen(r, false, 0, &arrivals_a, &got, NULL) == FIPSD_NATIVE_REG_OK && got == FREE);
    fipsd_native_refusal why;
    CHECK(fipsd_native_registry_listen(r, false, 0, &arrivals_a, &got, &why) == FIPSD_NATIVE_REG_NO_PORT);
    CHECK(fipsd_native_registry_port_count(r) == SPAN);
    fipsd_native_registry_destroy(r);
}
int main(void)
{
    CHECK(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    test_a_port_has_one_owner();
    test_releasing_a_flow_frees_its_port();
    test_ephemeral_ports();
    test_an_established_flow_wins_over_a_listener();
    test_a_flow_outlives_its_listener_and_keeps_receiving_after_a_rebind();
    test_arrivals_and_drops();
    test_accepting_returns_what_the_flow_held();
    test_a_pending_flow_holds_no_more_than_its_share();
    test_rejecting_frees_the_backlog_slot();
    test_a_pending_flow_expires_and_a_fresh_one_does_not();
    test_releasing_a_listener_takes_its_pending_flows_with_it();
    test_the_node_flow_ceiling_holds();
    test_the_flow_view();
    test_peer_share();
    test_ephemeral_ports_leak_no_count();
    test_the_one_free_port_is_found_and_then_none_is();
    fips_identity_destroy_secp(secp);
    puts("native registry: ports, delivery rule, backlog, ceiling, quarter share, expiry, ephemeral search: ok");
    return 0;
}
