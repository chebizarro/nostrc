#include "transport/ethernet/internal.h"
#include "control/transport.h"
#include "control/protocol.h"
#include "host_os.h"
#include <fips/identity/local.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static void codec(void)
{
    uint8_t wire[128], data[] = {0xaa,0xbb,0xcc}; const uint8_t *p = NULL; size_t n = 99;
    assert(fipsd_eth_data_encode(wire, sizeof wire, data, 3) == 6);
    assert(!memcmp(wire, (uint8_t[]){0,3,0,0xaa,0xbb,0xcc}, 6));
    memset(wire + 6, 0, 40); assert(fipsd_eth_data_decode(wire, 46, &p, &n) && n == 3 && !memcmp(p, data, n));
    assert(!fipsd_eth_data_decode(wire, 2, &p, &n));
    wire[1] = 50; assert(!fipsd_eth_data_decode(wire, 46, &p, &n));
    wire[0] = 2; assert(!fipsd_eth_data_decode(wire, 100, &p, &n));
    assert(!fipsd_eth_data_encode(wire, 3, data, 3));
    assert(fipsd_eth_data_encode(wire, sizeof wire, NULL, 0) == 3);
    assert(fipsd_eth_data_decode(wire, 3, &p, &n) && !n);
    fips_secp *secp = NULL; fips_identity *id = NULL;
    assert(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    uint8_t secret[32]; memset(secret, 0x42, 32);
    assert(fips_identity_from_secret_bytes(secp, fipsd_host_os_entropy(), secret, &id) == FIPS_IDENTITY_OK);
    fips_xonly_pubkey key = fips_identity_pubkey(id), got;
    fipsd_eth_beacon_encode(wire, &key);
    assert(wire[0] == 1 && wire[1] == 1 && !memcmp(wire + 2, key.bytes, 32));
    assert(fipsd_eth_beacon_decode(secp, wire, 34, &got) && !memcmp(got.bytes, key.bytes, 32));
    assert(fipsd_eth_beacon_decode(secp, wire, 46, &got));
    assert(!fipsd_eth_beacon_decode(secp, wire, 33, &got));
    wire[1] = 2; assert(!fipsd_eth_beacon_decode(secp, wire, 34, &got));
    wire[1] = 1; memset(wire + 2, 255, 32); assert(!fipsd_eth_beacon_decode(secp, wire, 34, &got));
    eth_instance *u = calloc(1, sizeof *u); assert(u);
    fipsd_transport_shared_init(&u->transport, &u->shared, false); fipsd_eth_neighbors_init(&u->neighbors); u->config.listen = true;
    fipsd_eth_beacon_encode(wire, &key);
    uint8_t mac[6] = {1,2,3,4,5,6};
    assert(fipsd_eth_dispatch_frame(u, secp, wire, 34, mac, &p, &n) == 0 && !u->neighbors.count);
    mac[0] = 255; assert(fipsd_eth_dispatch_frame(u, secp, wire, 34, mac, &p, &n) == 0);
    mac[0] = 2; assert(fipsd_eth_dispatch_frame(u, secp, wire, 34, mac, &p, &n) == 2 && u->neighbors.count == 1);
    assert(atomic_load(&u->shared.stats.beacons_recv) == 1 && atomic_load(&u->shared.stats.group_drops) == 2);
    fipsd_eth_data_encode(wire, sizeof wire, data, 3); mac[0] = 1;
    assert(fipsd_eth_dispatch_frame(u, secp, wire, 6, mac, &p, &n) == 0);
    mac[0] = 2; assert(fipsd_eth_dispatch_frame(u, secp, wire, 46, mac, &p, &n) == 1 && n == 3);
    free(u); fips_identity_destroy(id); fips_identity_destroy_secp(secp);
}
static void neighbors(void)
{
    fipsd_eth_neighbors *b = calloc(1, sizeof *b); assert(b); fipsd_eth_neighbors_init(b);
    fips_xonly_pubkey key = {{1}}; fipsd_eth_neighbor out[4];
    uint8_t a[6] = {2}, c[6] = {4}, d[6] = {6};
    assert(fipsd_eth_neighbors_add(b, a, &key)); assert(fipsd_eth_neighbors_add(b, c, &key));
    assert(fipsd_eth_neighbors_add(b, d, &key)); key.bytes[0] = 2; assert(fipsd_eth_neighbors_add(b, a, &key));
    assert(fipsd_eth_neighbors_take(b, out, 4) == 3 && out[0].mac[0] == 4 && out[1].mac[0] == 6 && out[2].key.bytes[0] == 2);
    assert(!fipsd_eth_neighbors_take(b, out, 4));
    for (unsigned i = 0; i < FIPSD_ETH_NEIGHBORS + 7; ++i) {
        uint8_t mac[6] = {2, (uint8_t)(i >> 8), (uint8_t)i};
        assert(fipsd_eth_neighbors_add(b, mac, &key) == (i < FIPSD_ETH_NEIGHBORS));
    }
    assert(b->dropped == 7 && b->count == FIPSD_ETH_NEIGHBORS);
    assert(fipsd_eth_neighbors_add(b, a, &key) && b->count == FIPSD_ETH_NEIGHBORS);
    fipsd_eth_neighbors_clear(b); assert(!b->count && b->dropped == 7); free(b);
}
static size_t record(uint8_t *buf, uint8_t value)
{
    fipsd_eth_bpf_header h = {.caplen = 18, .datalen = 18, .hdrlen = 20}; memcpy(buf, &h, 20);
    memset(buf + 20, value, 18); buf[32] = 0x21; buf[33] = 0x21;
    return 40;
}
static void bpf(void)
{
    uint8_t buf[100] = {0}, mac[6]; size_t at = 0, n; const uint8_t *p;
    size_t total = record(buf, 2); total += record(buf + total, 4);
    assert(fipsd_eth_bpf_next(buf, total, &at, 0x2121, &p, &n, mac) && n == 4 && mac[0] == 2 && at == 40);
    assert(fipsd_eth_bpf_next(buf, total, &at, 0x2121, &p, &n, mac) && mac[0] == 4);
    assert(!fipsd_eth_bpf_next(buf, total, &at, 0x2121, &p, &n, mac));
    at = 0; assert(!fipsd_eth_bpf_next(buf, 19, &at, 0x2121, &p, &n, mac));
    at = 0; assert(!fipsd_eth_bpf_next(buf, 37, &at, 0x2121, &p, &n, mac));
    fipsd_eth_bpf_header h = {.caplen = 18, .datalen = 50, .hdrlen = 20}; memcpy(buf, &h, 20);
    at = 0; assert(!fipsd_eth_bpf_next(buf, 40, &at, 0x2121, &p, &n, mac));
    h.caplen = 13; h.datalen = 13; memcpy(buf, &h, 20); at = 0;
    assert(!fipsd_eth_bpf_next(buf, 40, &at, 0x2121, &p, &n, mac));
}
typedef struct fixture { uint64_t binds, unbinds, gen, now; bool healthy; } fixture;
static uint64_t now(void *p) { return ((fixture *)p)->now; }
static fipsd_bind_result bind_fake(void *p, fipsd_runtime *rt, GoChannel *stop, const fipsd_presence_facts *f, uint64_t gen)
{ (void)rt; (void)stop; assert(f->exists && f->up); fixture *x = p; ++x->binds; x->gen = gen; return FIPSD_BIND_OK; }
static void unbind_fake(void *p, uint64_t gen) { fixture *x = p; assert(x->gen == gen); ++x->unbinds; }
static bool healthy(void *p) { return ((fixture *)p)->healthy; }
static void rebind(void)
{
    fixture x = {.healthy = true}; fipsd_transport t = {.type = "ethernet", .name = "eth-test"}; fipsd_transport_shared s;
    fipsd_transport_shared_init(&t, &s, true);
    fipsd_binder_ops ops = {.bind = bind_fake, .unbind = unbind_fake, .user = &x};
    fipsd_watcher_source source = {.user = &x, .now_ms = now};
    fipsd_watcher *w = fipsd_watcher_create(&t, &ops, &source); assert(w); fipsd_watcher_event_driven(w, healthy);
    fipsd_presence_facts f = {.valid = true, .exists = true, .up = true, .index = 7, .has_mac = true, .mac = {2}, .medium = FIPS_PATH_Wired};
    assert(!fipsd_watcher_step(w, (fipsd_presence_facts){.valid = true}, 0) && !x.binds);
    assert(!fipsd_watcher_step(w, f, 1) && x.binds == 1); /* carrier false does not gate */
    assert(fipsd_transport_medium(&t) == FIPS_PATH_Wired);
    f.medium = FIPS_PATH_Wireless; assert(!fipsd_watcher_step(w, f, 2));
    assert(fipsd_transport_medium(&t) == FIPS_PATH_Wireless && !x.unbinds);
    assert(!fipsd_watcher_step(w, (fipsd_presence_facts){0}, 3) && !x.unbinds);
    f.index = 9; assert(!fipsd_watcher_step(w, f, 11000) && x.unbinds == 1);
    assert(!fipsd_watcher_step(w, f, 11001) && x.binds == 2);
    fipsd_watcher_binding_exited(w, x.gen - 1); assert(!fipsd_watcher_step(w, f, 11002) && x.unbinds == 1);
    /* Kernel unregistered a socket although name, MAC and ifindex returned unchanged. */
    x.healthy = false; assert(!fipsd_watcher_step(w, f, 22000) && x.unbinds == 2);
    x.healthy = true; assert(!fipsd_watcher_step(w, f, 22001) && x.binds == 3);
    assert(!fipsd_watcher_step(w, (fipsd_presence_facts){.valid = true}, 33000) && x.unbinds == 3);
    f.mac[0] = 4; assert(!fipsd_watcher_step(w, f, 33001) && x.binds == 4);
    f.mac[0] = 6; assert(!fipsd_watcher_step(w, f, 44000) && x.unbinds == 4);
    assert(!fipsd_watcher_step(w, f, 44001) && x.binds == 5);
    fipsd_watcher_stop(w); fipsd_watcher_destroy(w);
}
static void projection(void)
{
    fipsd_ethernet_config c; fipsd_ethernet_config_default(&c); c.interface_name = "absent0";
    assert(c.ethertype == 0x2121 && c.listen && !c.announce && !c.auto_connect && !c.accept_connections);
    fipsd_transport *t = fipsd_ethernet_create((fips_transport_id){1}, "ethernet", &c); assert(t);
    fips_transport_addr a; assert(fipsd_transport_parse_address(t, "02:00:00:00:00:01", &a) == FIPS_OK && a.len == 6 && a.mac);
    assert(fipsd_transport_parse_address(t, "multicast", &a) != FIPS_OK);
    assert(t->ops->send(t, &a, (uint8_t[]){1}, 1) == -1);
    fipsd_control_transport_view v; fipsd_control_transport_read(t, 0, &v);
    fipsd_control_snapshot snapshot = {.transports = &v.row, .transports_count = 1};
    json_t *reply = fipsd_control_query(&snapshot, "show_transports", NULL); assert(reply);
    json_t *row = json_array_get(json_object_get(json_object_get(reply, "data"), "transports"), 0);
    json_t *stats = json_object_get(row, "stats"); assert(json_object_size(stats) == 13);
    assert(json_object_get(stats, "group_drops") && json_object_get(stats, "beacons_recv") && !json_object_get(stats, "packets_sent"));
    assert(json_object_size(json_object_get(row, "interface")) == 7);
    json_decref(reply); fipsd_ethernet_destroy(t);
}
int main(void) { codec(); neighbors(); bpf(); rebind(); projection(); puts("Ethernet codec, group drop, neighbors, BPF ABI, rebind and control: PASS"); return 0; }
