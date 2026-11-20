/* Two-node IPv6 shim proof (nostrc-9nxwk.2): two in-process nodes over the
 * loopback transport, each with a fake TUN device injected through the
 * fipsd_tun_ops seam (a socketpair standing in for the kernel: the test is
 * the host's IPv6 stack) and the real shim between the device's frames and
 * the node's sessions. Proves, end to end and byte for byte: a ping-sized
 * packet across the mesh, an oversize packet answered Packet Too Big with
 * the right MTU, a sub-1280 path carried as IPv6 fragments that reassemble,
 * a SYN clamped to the stored path on the way in, extension headers that
 * refuse fragmentation and ICMPv6 answers, hairpin and off-mesh/unknown
 * answers, and the listening panel's accept/drop verdicts. Every wait is a
 * bounded channel receive or a poll on the fake device; no sleeps. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ipv6/shim.h"
#include "ipv6/firewall_state.h"
#include "ipv6/icmp.h"
#include "ipv6/tcp_mss.h"
#include "tun/internal.h"
#include "tun/lifecycle.h"
#include "node/context.h"
#include "node/handlers/handlers.h"
#include "node/netmon.h"
#include "transport/loopback.h"
#include "config/config.h"
#include "select.h"
#include <fips/proto/framing.h>
#include <fips/identity/address.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WAIT_MS 60000u
typedef struct tn {
    const char *name;
    fipsd_config *config; GoSwap *configs;
    fipsd_node_handlers *h; fipsd_node *node; fipsd_transport *transport;
    fipsd_netmon_source netmon; fips_peer_identity identity; const fips_transport_addr *addr;
    int pair[2]; fipsd_tun *tun; fipsd_tun_service service; fipsd_ipv6_shim *shim; GoChannel *pass;
    uint8_t our[16]; unsigned opened, closed;
} tn;
static const char *yaml_a = "node: {identity: {nsec: '1111111111111111111111111111111111111111111111111111111111111111', persistent: true}, control: {enabled: false}}";
static const char *yaml_b = "node: {identity: {nsec: '2222222222222222222222222222222222222222222222222222222222222222', persistent: true}, control: {enabled: false}}";
static fipsd_network_snapshot *network(void *arg) { (void)arg; fipsd_network_snapshot *s = calloc(1, sizeof *s); assert(s); return s; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
/* The fake kernel: the device is one end of a datagram socketpair. */
static int device_open(fipsd_tun *t)
{
    tn *n = t->ops.user;
    GoRefPtr ref = fipsd_node_status_load(fipsd_node_status_slot(n->node));
    assert(ref.ptr && ((fipsd_node_status *)ref.ptr)->ready); go_refptr_release(&ref);
    assert(fipsd_network_fd_prepare(n->pair[0]) == 0);
    ++n->opened; memcpy(t->name, "fake0", 6); return n->pair[0];
}
static int nothing(fipsd_tun *t, bool up) { (void)t; (void)up; return 0; }
static int mtu_ok(fipsd_tun *t) { (void)t; return 0; }
static int route(fipsd_tun *t, bool up) { t->routed = up; return 0; }
static int close_fd(void *arg, int fd) { tn *n = arg; assert(fd == n->pair[0]); ++n->closed; return close(fd); }
static unsigned index_of(void *arg, const char *name) { (void)arg; assert(!strcmp(name, "fake0")); return 3; }
static const fipsd_tun_platform platform = {.os = FIPSD_TUN_LINUX, .family_header = 0, .open = device_open,
    .address = nothing, .mtu = mtu_ok, .up = nothing, .route = route, .destroy = fipsd_tun_no_destroy};
static void init(tn *n, fipsd_loopback_network *net, const char *name, const char *address, const char *yaml, const fipsd_node_handlers_config *hcfg)
{
    memset(n, 0, sizeof *n); n->name = name;
    n->config = fipsd_config_parse(yaml, strlen(yaml), NULL); assert(n->config);
    n->configs = go_swap_create(n->config, fipsd_config_free); assert(n->configs);
    n->h = fipsd_node_handlers_create(hcfg); assert(n->h);
    n->netmon = (fipsd_netmon_source){.read = network, .injected = true};
    fipsd_node_options options = {.workers = 2, .peer_capacity = hcfg->peer_capacity, .netmon_source = &n->netmon};
    fipsd_node_handlers_install(n->h, &options);
    n->node = fipsd_node_create(n->configs, &options); assert(n->node);
    /* 1357 bytes on the wire carries the IPv6 minimum: the default UDP budget. */
    n->transport = fipsd_loopback_transport_create(net, (fips_transport_id){1}, "loop", address, 1357); assert(n->transport);
    n->addr = fipsd_loopback_transport_address(n->transport);
    assert(fipsd_transport_register(fipsd_node_transports(n->node), n->transport) == 0);
    assert(!socketpair(AF_UNIX, SOCK_DGRAM, 0, n->pair));
    for (int i = 0; i < 2; ++i) { int size = 262144; (void)setsockopt(n->pair[i], SOL_SOCKET, SO_SNDBUF, &size, sizeof size); (void)setsockopt(n->pair[i], SOL_SOCKET, SO_RCVBUF, &size, sizeof size); }
    fipsd_tun_ops ops = *fipsd_tun_system_ops(); ops.user = n; ops.close = close_fd; ops.ifindex = index_of;
    fipsd_tun_config cfg = {.name = "fips0", .transport_mtu = 1357, .mtu = 0};
    n->tun = fipsd_tun_create(&cfg, &ops, &platform); assert(n->tun);
    fipsd_tun_service_init(&n->service, n->tun, n->node);
    n->pass = go_channel_create(256); assert(n->pass);
    fipsd_ipv6_shim_config scfg = {.tun = n->tun, .handlers = n->h, .passthrough = n->pass,
        .payload_limit = fips_framing_payload_limit(1357), .host_queue = 64, .path_capacity = 8, .key_capacity = 8};
    n->shim = fipsd_ipv6_shim_create(&scfg); assert(n->shim);
    fipsd_tun_service_set_shim(&n->service, n->shim);
}
static void identity(tn *n)
{
    const fipsd_node_context *c = fipsd_node_get_context(n->node);
    fips_xonly_pubkey key = fips_identity_pubkey(fipsd_node_identity(c));
    assert(fips_peer_identity_from_pubkey(fipsd_node_secp(c), &key, &n->identity) == FIPS_IDENTITY_OK);
    memcpy(n->our, fips_identity_address(fipsd_node_identity(c)).bytes, 16);
}
/* Notices the shim passed through, until one of kind (and optionally peer). */
static fipsd_node_notice *await(tn *n, fipsd_notice_kind kind, const fips_node_addr *peer)
{
    for (;;) {
        void *value = NULL; GoSelectCase cases[] = {{GO_SELECT_RECEIVE, n->pass, NULL, &value}};
        GoSelectResult r = go_select_timeout(cases, 1, WAIT_MS);
        if (r.selected_case != 0 || !value) { fprintf(stderr, "%s: no notice of kind %d\n", n->name, kind); abort(); }
        fipsd_node_notice *e = value;
        if (e->kind == kind && (!peer || !memcmp(peer->bytes, e->peer.bytes, 16))) return e;
        fipsd_node_notice_free(e);
    }
}
static void drain(tn *n) { void *v; while (go_channel_try_receive(n->pass, &v) == 0) fipsd_node_notice_free(v); }
/* The host's side of the fake device. */
static void host_write(tn *n, const uint8_t *frame, size_t len) { assert(write(n->pair[1], frame, len) == (ssize_t)len); }
static size_t host_read(tn *n, uint8_t *frame, size_t cap)
{
    struct pollfd p = {.fd = n->pair[1], .events = POLLIN};
    int ready = poll(&p, 1, (int)WAIT_MS);
    if (ready <= 0) { fprintf(stderr, "%s: nothing reached the host\n", n->name); abort(); }
    ssize_t got = read(n->pair[1], frame, cap); assert(got > 0); return (size_t)got;
}
static size_t udp(const uint8_t src[16], const uint8_t dst[16], size_t len, uint8_t *p)
{
    memset(p, 0, len); p[0] = 0x60; put16(p + 4, (uint16_t)(len - 40)); p[6] = 17; p[7] = 64; memcpy(p + 8, src, 16); memcpy(p + 24, dst, 16);
    put16(p + 40, 4000); put16(p + 42, 5000); put16(p + 44, (uint16_t)(len - 40));
    for (size_t i = 48; i < len; ++i) p[i] = (uint8_t)(i * 13);
    fipsd_ipv6_recalculate_l4_checksum(p, len);
    return len;
}
static size_t echo(const uint8_t src[16], const uint8_t dst[16], uint8_t type, uint8_t *p)
{
    size_t len = 40 + 8 + 56; memset(p, 0, len); p[0] = 0x60; put16(p + 4, 64); p[6] = 58; p[7] = 64; memcpy(p + 8, src, 16); memcpy(p + 24, dst, 16);
    p[40] = type; put16(p + 44, 0x1234); put16(p + 46, 1);
    for (size_t i = 48; i < len; ++i) p[i] = (uint8_t)i;
    uint16_t c = fipsd_ipv6_icmpv6_checksum(p + 40, 64, src, dst); put16(p + 42, c);
    return len;
}
static uint16_t tcp_sum(const uint8_t *packet, size_t len, size_t at)
{
    uint32_t sum = 0;
    for (size_t i = 8; i < 40; i += 2) sum += be16(packet + i);
    sum += (uint32_t)(len - at) + 6;
    for (size_t i = at; i < len; i += 2) sum += i + 1 < len ? be16(packet + i) : (uint32_t)packet[i] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)sum;
}
static size_t syn(const uint8_t src[16], const uint8_t dst[16], uint16_t mss, uint8_t *p)
{
    memset(p, 0, 64); p[0] = 0x60; put16(p + 4, 24); p[6] = 6; p[7] = 64; memcpy(p + 8, src, 16); memcpy(p + 24, dst, 16);
    put16(p + 40, 40000); put16(p + 42, 22); p[52] = 6 << 4; p[53] = 0x02; put16(p + 54, 64240); p[60] = 2; p[61] = 4; put16(p + 62, mss);
    put16(p + 56, (uint16_t)~tcp_sum(p, 64, 40)); assert(tcp_sum(p, 64, 40) == 0xffff);
    return 64;
}
static void expect_icmp(const uint8_t *r, size_t n, uint8_t type, const uint8_t src[16], const uint8_t dst[16], const uint8_t *quoted, size_t quoted_len)
{
    assert(n >= 48 && r[6] == 58 && r[40] == type && !memcmp(r + 8, src, 16) && !memcmp(r + 24, dst, 16));
    size_t q = quoted_len < 1232 ? quoted_len : 1232; assert(n == 48 + q && !memcmp(r + 48, quoted, q));
    assert(be16(r + 42) == fipsd_ipv6_icmpv6_checksum(r + 40, n - 40, src, dst));
}
int main(void)
{
    fipsd_runtime_config rcfg; fipsd_runtime_config_default(&rcfg); rcfg.workers = 2; rcfg.handle_signals = false; rcfg.grace_ms = 20000;
    fipsd_runtime *rt = fipsd_runtime_create(&rcfg); assert(rt);
    fipsd_loopback_network *net = fipsd_loopback_network_create(); assert(net);
    fipsd_node_handlers_config hcfg; fipsd_node_handlers_config_default(&hcfg);
    hcfg.peer_capacity = 4; hcfg.tree_peer_capacity = 4; hcfg.session_capacity = 4; hcfg.cache_entries = 64; hcfg.notice_capacity = 256;
    hcfg.tick_ms = 200; hcfg.tree_announce_min_interval_ms = 100; hcfg.bloom_debounce_ms = 100; hcfg.link_dead_timeout_ms = 30000;
    tn a, b;
    init(&a, net, "A", "loop:a", yaml_a, &hcfg); init(&b, net, "B", "loop:b", yaml_b, &hcfg);
    a.transport->interface_name = "loop-test"; b.transport->interface_name = "loop-test";
    assert(fipsd_runtime_register(rt, fipsd_node_subsystem(a.node)) == 0 && fipsd_runtime_register(rt, fipsd_node_subsystem(b.node)) == 0);
    assert(fipsd_runtime_register(rt, &a.service.subsystem) == 0 && fipsd_runtime_register(rt, &b.service.subsystem) == 0);
    assert(fipsd_runtime_start(rt) == 0);
    assert(a.opened == 1 && b.opened == 1);
    assert(fipsd_tun_status_read(a.tun).state == FIPSD_TUN_UP && fipsd_tun_status_read(a.tun).mtu == 1280);
    identity(&a); identity(&b);
    /* 1. The IK link, tree and filters, as the two-node proof: the shim learns
     *    B's key from the LINK_UP it passes through, and a name resolution
     *    would teach it too. */
    assert(fipsd_node_handlers_connect(a.h, &b.identity, (fips_transport_id){1}, b.addr) == 0);
    fipsd_node_notice *e = await(&a, FIPSD_NOTICE_LINK_UP, &b.identity.node_addr); fipsd_node_notice_free(e);
    e = await(&b, FIPSD_NOTICE_LINK_UP, &a.identity.node_addr); fipsd_node_notice_free(e);
    e = await(&a, FIPSD_NOTICE_FILTER_ACCEPTED, &b.identity.node_addr); fipsd_node_notice_free(e);
    e = await(&b, FIPSD_NOTICE_FILTER_ACCEPTED, &a.identity.node_addr); fipsd_node_notice_free(e);
    { fips_node_prefix pb = fips_node_addr_prefix(&b.identity.node_addr); fips_node_addr found;
      assert(fipsd_ipv6_mesh_state_key(fipsd_ipv6_shim_state(a.shim), &pb, &found) && !memcmp(found.bytes, b.identity.node_addr.bytes, 16));
      assert(fipsd_ipv6_shim_learn(a.shim, &b.identity.node_addr).seq == 1); }
    assert(!memcmp(a.our, fipsd_ipv6_peer_address(&a.identity.node_addr).bytes, 16));
    static uint8_t frame[2048], got[2048], original[2048];
    /* 2. A ping-sized echo request A -> B crosses the mesh and reaches B's host
     *    byte for byte; the reply crosses back. */
    size_t n = echo(a.our, b.our, 128, frame); host_write(&a, frame, n);
    size_t m = host_read(&b, got, sizeof got); assert(m == n && !memcmp(got, frame, n));
    n = echo(b.our, a.our, 129, frame); host_write(&b, frame, n);
    m = host_read(&a, got, sizeof got); assert(m == n && !memcmp(got, frame, n));
    /* 3. Hairpin: a packet to our own address comes back with its checksum completed. */
    n = udp(a.our, a.our, 100, frame); memcpy(original, frame, n); frame[46] = 0; frame[47] = 0; host_write(&a, frame, n);
    m = host_read(&a, got, sizeof got); assert(m == n && !memcmp(got, original, n));
    /* 4. Off-mesh and unknown-key destinations: Destination Unreachable from A's address. */
    { uint8_t outside[16] = {0x20, 0x01, 0x0d, 0xb8}; outside[15] = 1; n = udp(a.our, outside, 100, frame); host_write(&a, frame, n);
      m = host_read(&a, got, sizeof got); expect_icmp(got, m, 1, a.our, a.our, frame, n); assert(got[41] == 0); }
    { uint8_t unknown[16]; memcpy(unknown, b.our, 16); unknown[15] ^= 0x5a; unknown[14] ^= 0x3c; n = udp(a.our, unknown, 100, frame); host_write(&a, frame, n);
      m = host_read(&a, got, sizeof got); expect_icmp(got, m, 1, a.our, a.our, frame, n); assert(got[41] == 0); }
    /* 5. Oversize: over the node-wide IPv6 MTU (1280) to a known key draws
     *    Packet Too Big reporting 1280, from B's address (the kernel sees a remote router). */
    n = udp(a.our, b.our, 1400, frame); host_write(&a, frame, n);
    m = host_read(&a, got, sizeof got); expect_icmp(got, m, 2, b.our, a.our, frame, n); assert(be32(got + 44) == 1280);
    /* 6. A sub-1280 path to B (payload limit 1090: IPv6 MTU 1123) carries a
     *    1280-byte packet as two IPv6 fragments B's host reassembles. */
    { fipsd_ipv6_host_link link = fipsd_ipv6_shim_host_link(a.shim);
      fipsd_ipv6_from_mesh path = {.kind = FIPSD_IPV6_MESH_PATH_MTU, .peer = b.identity.node_addr, .has_limit = true, .payload_limit = 1090};
      fipsd_ipv6_host_link_send(&link, &path); }
    n = udp(a.our, b.our, 1280, frame); memcpy(original, frame, n); host_write(&a, frame, n);
    { static uint8_t f[2][1300], back[2048]; size_t fl[2], data = 0;
      for (unsigned i = 0; i < 2; ++i) fl[i] = host_read(&b, f[i], 1300);
      /* two crypto workers may complete the fragments in either order */
      if (fl[0] < fl[1]) { uint8_t t[1300]; memcpy(t, f[0], 1300); memcpy(f[0], f[1], 1300); memcpy(f[1], t, 1300); size_t x = fl[0]; fl[0] = fl[1]; fl[1] = x; }
      assert(fl[0] == 1120 && fl[1] == 216);
      for (unsigned i = 0; i < 2; ++i) {
          assert(f[i][6] == 44 && !memcmp(f[i] + 8, original + 8, 32) && f[i][40] == 17 && be32(f[i] + 44) == be32(f[0] + 44));
          uint16_t om = be16(f[i] + 42); assert((om & 0xfff8u) == data && ((om & 1) == 1) == (i == 0));
          memcpy(back + 40 + data, f[i] + 48, fl[i] - 48); data += fl[i] - 48;
      }
      memcpy(back, original, 40); assert(40 + data == 1280 && !memcmp(back, original, 1280)); }
    /* 7. A SYN to B is clamped on the way out to the stored path (tcp_mss(1090)
     *    = 1063); B's writer, with no path for A, leaves 1063 under its 1143 ceiling. */
    n = syn(a.our, b.our, 1440, frame); host_write(&a, frame, n);
    m = host_read(&b, got, sizeof got); assert(m == 64 && be16(got + 62) == 1063 && tcp_sum(got, 64, 40) == 0xffff && !memcmp(got + 40, frame + 40, 16));
    /* 8. Extension headers: a 1280-byte packet with a Hop-by-Hop header first
     *    cannot be fragmented and draws Packet Too Big with the path's 1123; a
     *    SYN sealed behind an Authentication Header reaches B unclamped; an
     *    off-mesh packet whose chain is truncated draws no answer, shown by
     *    the next packet's answer arriving first. */
    n = udp(a.our, b.our, 1280, frame); frame[6] = 0; { const uint8_t hbh[8] = {17, 0, 1, 4, 0, 0, 0, 0}; memcpy(frame + 40, hbh, 8); } host_write(&a, frame, n);
    m = host_read(&a, got, sizeof got); expect_icmp(got, m, 2, b.our, a.our, frame, n); assert(be32(got + 44) == 1123);
    { uint8_t plain[64]; syn(a.our, b.our, 1440, plain); memcpy(frame, plain, 40); const uint8_t ah[24] = {6, 4, 0, 0}; memcpy(frame + 40, ah, 24); memset(frame + 44, 0x11, 20);
      memcpy(frame + 64, plain + 40, 24); put16(frame + 4, 48); frame[6] = 51; host_write(&a, frame, 88);
      m = host_read(&b, got, sizeof got); assert(m == 88 && !memcmp(got, frame, 88) && be16(got + 86) == 1440); }
    { uint8_t outside[16] = {0x20, 0x01, 0x0d, 0xb8}; outside[15] = 1; uint8_t truncated[100]; udp(a.our, outside, 100, truncated); truncated[6] = 60;
      const uint8_t bad[8] = {17, 255, 1, 4, 0, 0, 0, 0}; memcpy(truncated + 40, bad, 8); host_write(&a, truncated, 100);
      n = udp(a.our, outside, 120, frame); host_write(&a, frame, n);
      m = host_read(&a, got, sizeof got); expect_icmp(got, m, 1, a.our, a.our, frame, n); }
    /* 9. The listening panel: a table scoped to the live TUN accepts the opened
     *    port and drops the other listener; scoped elsewhere, it protects nothing. */
    { json_t *nft = json_loads("{\"nftables\":["
          "{\"rule\":{\"family\":\"inet\",\"table\":\"fips\",\"chain\":\"inbound\",\"expr\":[{\"match\":{\"op\":\"!=\",\"left\":{\"meta\":{\"key\":\"iifname\"}},\"right\":\"fake0\"}},{\"return\":null}]}},"
          "{\"rule\":{\"family\":\"inet\",\"table\":\"fips\",\"chain\":\"inbound\",\"expr\":[{\"match\":{\"op\":\"==\",\"left\":{\"payload\":{\"protocol\":\"tcp\",\"field\":\"dport\"}},\"right\":22}},{\"accept\":null}]}},"
          "{\"rule\":{\"family\":\"inet\",\"table\":\"fips\",\"chain\":\"inbound\",\"expr\":[{\"counter\":{\"packets\":0,\"bytes\":0}},{\"drop\":null}]}}]}", 0, NULL);
      assert(nft); fipsd_ipv6_filter_classifier *c = fipsd_ipv6_filter_from_nft_json(nft); json_decref(nft); assert(c && fipsd_ipv6_filter_polices(c, fipsd_tun_name(b.tun)));
      fipsd_ipv6_listening_socket rows[2]; uint8_t any[16] = {0};
      assert(fipsd_ipv6_listening_build_entry(FIPSD_IPV6_PROTO_TCP, true, b.our, 22, 0, NULL, b.our, &rows[0]));
      assert(fipsd_ipv6_listening_build_entry(FIPSD_IPV6_PROTO_TCP, true, any, 8080, 0, NULL, b.our, &rows[1]));
      json_t *panel = fipsd_ipv6_listening_render(c, fipsd_tun_name(b.tun), b.our, rows, 2); assert(panel);
      json_t *sockets = json_object_get(panel, "sockets"); assert(json_array_size(sockets) == 2);
      assert(!strcmp(json_string_value(json_object_get(json_array_get(sockets, 0), "filter")), "accept"));
      assert(!strcmp(json_string_value(json_object_get(json_array_get(sockets, 1), "filter")), "drop"));
      assert(fipsd_ipv6_filter_classify(c, FIPSD_IPV6_PROTO_TCP, 9999) == FIPSD_IPV6_FILTER_DROP); /* a port nothing listens on */
      json_decref(panel);
      panel = fipsd_ipv6_listening_render(c, "mesh0", b.our, rows, 2); assert(panel && json_is_false(json_object_get(panel, "firewall_active")));
      json_decref(panel); fipsd_ipv6_filter_destroy(c); }
    /* 10. Counters and status reflect what happened; nothing was dropped toward the host. */
    { fipsd_ipv6_shim_counters ca = fipsd_ipv6_shim_counters_read(a.shim), cb = fipsd_ipv6_shim_counters_read(b.shim);
      assert(ca.hairpinned == 1 && ca.off_mesh == 2 && ca.unreachable == 3 && ca.too_big == 2 && ca.fragments == 2 && ca.clamped_out == 1);
      assert(ca.forwarded >= 4 && ca.delivered == 1 && cb.delivered >= 4 && cb.forwarded == 1 && ca.send_refused == 0 && cb.send_refused == 0);
      assert(ca.host_drops == 0 && fipsd_tun_status_read(a.tun).host_drops == 0 && fipsd_tun_status_read(b.tun).host_drops == 0);
      printf("A: read=%llu forwarded=%llu fragments=%llu too_big=%llu unreachable=%llu hairpinned=%llu clamped_out=%llu delivered=%llu written=%llu\n",
          (unsigned long long)ca.read, (unsigned long long)ca.forwarded, (unsigned long long)ca.fragments, (unsigned long long)ca.too_big,
          (unsigned long long)ca.unreachable, (unsigned long long)ca.hairpinned, (unsigned long long)ca.clamped_out, (unsigned long long)ca.delivered, (unsigned long long)ca.written);
      printf("B: delivered=%llu written=%llu clamped_in=%llu forwarded=%llu\n", (unsigned long long)cb.delivered, (unsigned long long)cb.written,
          (unsigned long long)cb.clamped_in, (unsigned long long)cb.forwarded); }
    /* 11. Ordered shutdown: the shims cancel and join their fibers before the
     *     devices close; nothing is left parked on a descriptor. */
    fipsd_runtime_shutdown(rt); assert(fipsd_runtime_run(rt) == FIPSD_RUN_OK);
    assert(a.closed == 1 && b.closed == 1 && fipsd_tun_status_read(a.tun).state == FIPSD_TUN_STOPPED);
    drain(&a); drain(&b);
    fipsd_runtime_destroy(rt);
    for (tn *x = &a; x; x = x == &a ? &b : NULL) {
        fipsd_ipv6_shim_destroy(x->shim); fipsd_tun_destroy(x->tun); fipsd_node_destroy(x->node); fipsd_node_handlers_destroy(x->h);
        fipsd_loopback_transport_destroy(x->transport); go_swap_free(x->configs); go_channel_unref(x->pass); assert(!close(x->pair[1]));
    }
    fipsd_loopback_network_destroy(net);
    puts("two nodes over fake TUNs: ping across, PTB 1280, two-fragment path, SYN clamp 1063, extension-header refusals, hairpin/unreachable, listening panel, ordered stop: ok");
    return 0;
}
