/* Resolver tests from upstream src/ipv6tun/dns.rs (unit and screening
 * modules) and src/ipv6tun/tests/dns.rs: resolution, the mesh filter, the
 * reply-path tables for both pin modes, returns_by, and the socket helpers on
 * loopback. Negative delivery checks read the receiver without waiting. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "dns/resolver.h"
#include "host_os.h"
#include <fips/identity/local.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno %d %s)\n", __FILE__, __LINE__, #x, errno, strerror(errno)); abort(); } } while (0)

#define TTL 300u
#define CLIENT_PORT 53000
static fips_secp *secp;
typedef struct identity { char npub[FIPS_NIP19_CAPACITY]; fips_ipv6_address address; fips_node_addr node_addr; } identity;
static identity generate(void)
{
    identity out; fips_identity *id;
    CHECK(fips_identity_generate(secp, fipsd_host_os_entropy(), &id) == FIPS_IDENTITY_OK);
    CHECK(fips_identity_npub(id, out.npub, sizeof out.npub) == FIPS_IDENTITY_OK);
    out.address = fips_identity_address(id); out.node_addr = fips_identity_node_addr(id);
    fips_identity_destroy(id);
    return out;
}
static uint16_t word(const uint8_t *b, size_t off) { return (uint16_t)((b[off] << 8) | b[off + 1]); }
static size_t wire_name(const char *name, uint8_t *out)
{
    size_t at = 0;
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.'); size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len) { out[at++] = (uint8_t)len; memcpy(out + at, p, len); at += len; }
        if (!dot) break;
        p = dot + 1;
    }
    out[at++] = 0; return at;
}
static size_t message(uint16_t id, uint16_t flags, const uint16_t counts[4], const uint8_t *body, size_t body_len, uint8_t *out)
{
    out[0] = (uint8_t)(id >> 8); out[1] = (uint8_t)id; out[2] = (uint8_t)(flags >> 8); out[3] = (uint8_t)flags;
    for (size_t i = 0; i < 4; ++i) { out[4 + 2 * i] = (uint8_t)(counts[i] >> 8); out[5 + 2 * i] = (uint8_t)counts[i]; }
    if (body_len) memcpy(out + 12, body, body_len); /* callers pass NULL, 0 for a header-only message */
    return 12 + body_len;
}
static size_t question(const uint8_t *name, size_t n, uint16_t qtype, uint16_t qclass, uint8_t *out)
{ memcpy(out, name, n); out[n] = (uint8_t)(qtype >> 8); out[n + 1] = (uint8_t)qtype; out[n + 2] = (uint8_t)(qclass >> 8); out[n + 3] = (uint8_t)qclass; return n + 4; }
static size_t query(uint16_t id, uint16_t flags, const char *name, uint16_t qtype, uint16_t qclass, uint8_t *out)
{
    uint8_t wire[300], body[320]; size_t n = wire_name(name, wire); size_t bn = question(wire, n, qtype, qclass, body);
    static const uint16_t one[4] = {1, 0, 0, 0}; return message(id, flags, one, body, bn, out);
}
static size_t aaaa_query(uint16_t id, const char *name, uint8_t *out) { return query(id, 0, name, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, out); }
/* A test query as simple-dns's Packet::new_query builds it: RD set. */
static size_t build_test_query(const char *name, uint16_t qtype, uint8_t *out) { return query(0x1234, 0x0100, name, qtype, FIPSD_DNS_CLASS_IN, out); }
/* An AAAA IN query carrying one EDNS OPT whose padding makes it total bytes. */
static size_t padded_query(uint16_t id, const char *name, size_t total, uint8_t *out)
{
    uint8_t wire[300], body[4300]; size_t n = wire_name(name, wire); size_t bn = question(wire, n, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, body);
    size_t fixed = 12 + bn + 11 + 4; CHECK(total >= fixed); size_t pad = total - fixed;
    body[bn++] = 0; body[bn++] = 0; body[bn++] = 41; body[bn++] = 1232 >> 8; body[bn++] = 1232 & 0xff; memset(body + bn, 0, 4); bn += 4;
    body[bn++] = (uint8_t)((4 + pad) >> 8); body[bn++] = (uint8_t)(4 + pad); body[bn++] = 0; body[bn++] = 12; body[bn++] = (uint8_t)(pad >> 8); body[bn++] = (uint8_t)pad;
    memset(body + bn, 0, pad); bn += pad;
    static const uint16_t counts[4] = {1, 0, 0, 1}; size_t len = message(id, 0, counts, body, bn, out); CHECK(len == total); return len;
}
/* A raw query whose question name is the given wire labels, so a label may hold a dot. */
static size_t wire_query(const uint8_t *const *labels, const size_t *lens, size_t n, uint16_t qtype, uint8_t *out)
{
    static const uint8_t head[12] = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    memcpy(out, head, 12); size_t at = 12;
    for (size_t i = 0; i < n; ++i) { out[at++] = (uint8_t)lens[i]; memcpy(out + at, labels[i], lens[i]); at += lens[i]; }
    out[at++] = 0; out[at++] = (uint8_t)(qtype >> 8); out[at++] = (uint8_t)qtype; out[at++] = 0; out[at++] = 1; return at;
}
static uint8_t rcode(const uint8_t *r) { return r[3] & 0x0F; }
static uint8_t reply[FIPSD_DNS_MAX_REPLY];
static fips_peer_identity resolved_peer; static bool resolved;
/* The reply length for datagram from port, with hosts. */
static size_t answer(const uint8_t *d, size_t len, uint16_t port, const fipsd_hosts_map *hosts)
{ return fipsd_dns_handle_packet(d, len, port, TTL, hosts, secp, reply, sizeof reply, &resolved_peer, &resolved); }
/* The rdata of the first answer record of a one-question reply. */
static const uint8_t *first_rdata(const uint8_t *r, size_t len, size_t *rdlen)
{
    size_t off = 12; while (off < len && r[off]) off += 1 + r[off]; off += 1 + 4; /* question */
    CHECK(off + 12 <= len && r[off] == 0xC0); *rdlen = word(r, off + 10); return r + off + 12;
}
/* The reflection bound: no reply, or one at most one AAAA longer than the
 * query, with at most one question and one answer, no authority or additional. */
static void assert_bounded(const uint8_t *q, size_t qlen, size_t rlen)
{
    (void)q;
    if (!rlen) return;
    CHECK(rlen <= qlen + 28 && word(reply, 4) <= 1 && word(reply, 6) <= 1 && word(reply, 8) == 0 && word(reply, 10) == 0);
    if (word(reply, 6) == 1) { size_t rdlen; first_rdata(reply, rlen, &rdlen); CHECK(rdlen == 16); }
}
static void map_insert(fipsd_hosts_map *m, const char *name, const char *npub)
{ char reason[160]; CHECK(fipsd_hosts_map_insert(m, name, npub, secp, reason, sizeof reason) == 0); }
static struct sockaddr_storage at(const char *ip, uint16_t port)
{
    struct sockaddr_storage s; memset(&s, 0, sizeof s);
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&s; struct sockaddr_in *v4 = (struct sockaddr_in *)&s;
    if (inet_pton(AF_INET6, ip, &v6->sin6_addr) == 1) { v6->sin6_family = AF_INET6; v6->sin6_port = htons(port); }
    else { CHECK(inet_pton(AF_INET, ip, &v4->sin_addr) == 1); v4->sin_family = AF_INET; v4->sin_port = htons(port); }
    return s;
}
static fips_ip_addr ip(const char *text) { struct sockaddr_storage s = at(text, 0); return fipsd_dns_ip_of((struct sockaddr *)&s); }
static struct in6_addr ip6(const char *text) { struct in6_addr a; CHECK(inet_pton(AF_INET6, text, &a) == 1); return a; }
static int bind_loopback(const char *ip_text, struct sockaddr_storage *local, socklen_t *len)
{
    struct sockaddr_storage addr = at(ip_text, 0);
    int fd = fipsd_dns_bind_socket((struct sockaddr *)&addr, addr.ss_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
    CHECK(fd >= 0); *len = sizeof *local; CHECK(getsockname(fd, (struct sockaddr *)local, len) == 0); return fd;
}
/* Receive on a non-blocking socket, waiting at most ms. */
static ssize_t recv_within(int fd, uint8_t *buf, size_t cap, int ms)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, ms) <= 0) return -1;
    return recv(fd, buf, cap, 0);
}
static bool nothing_queued(int fd) { uint8_t b[16]; return recv(fd, b, sizeof b, MSG_DONTWAIT) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK); }

int main(void)
{
    CHECK(fips_identity_init_secp(fipsd_host_os_entropy(), &secp) == FIPS_IDENTITY_OK);
    uint8_t d[4300]; size_t n, rn; fips_peer_identity peer; char name[128];
    fipsd_hosts_map empty; fipsd_hosts_map_init(&empty);

    /* --- resolve_name: direct npub --- */
    { identity id = generate();
      snprintf(name, sizeof name, "%s.fips", id.npub); CHECK(fipsd_dns_resolve_name(name, NULL, secp, &peer));
      CHECK(!memcmp(peer.address.bytes, id.address.bytes, 16) && !memcmp(peer.node_addr.bytes, id.node_addr.bytes, 16));
      snprintf(name, sizeof name, "%s.fips.", id.npub); CHECK(fipsd_dns_resolve_name(name, NULL, secp, &peer) && !memcmp(peer.address.bytes, id.address.bytes, 16));
      snprintf(name, sizeof name, "%s.FIPS", id.npub); CHECK(fipsd_dns_resolve_name(name, NULL, secp, &peer));
      snprintf(name, sizeof name, "%s.Fips", id.npub); CHECK(fipsd_dns_resolve_name(name, NULL, secp, &peer));
      CHECK(!fipsd_dns_resolve_name("not-a-valid-npub.fips", NULL, secp, &peer));
      snprintf(name, sizeof name, "%s.com", id.npub); CHECK(!fipsd_dns_resolve_name(name, NULL, secp, &peer));
      CHECK(!fipsd_dns_resolve_name("", NULL, secp, &peer) && !fipsd_dns_resolve_name(".fips", NULL, secp, &peer) && !fipsd_dns_resolve_name("fips", NULL, secp, &peer)); }
    /* --- resolve_name with hosts --- */
    { identity id = generate(); fipsd_hosts_map hosts; fipsd_hosts_map_init(&hosts); map_insert(&hosts, "gateway", id.npub);
      CHECK(fipsd_dns_resolve_name("gateway.fips", &hosts, secp, &peer) && !memcmp(peer.address.bytes, id.address.bytes, 16));
      CHECK(fipsd_dns_resolve_name("Gateway.FIPS", &hosts, secp, &peer) && fipsd_dns_resolve_name("GATEWAY.fips", &hosts, secp, &peer));
      CHECK(fipsd_dns_resolve_name("gateway.fips.", &hosts, secp, &peer));
      snprintf(name, sizeof name, "%s.fips", id.npub); CHECK(fipsd_dns_resolve_name(name, &empty, secp, &peer) && !memcmp(peer.address.bytes, id.address.bytes, 16));
      CHECK(!fipsd_dns_resolve_name("unknown.fips", &empty, secp, &peer));
      fipsd_hosts_map_clear(&hosts); }
    /* --- handle_packet --- */
    { identity id = generate(); size_t rdlen;
      snprintf(name, sizeof name, "%s.fips", id.npub);
      n = build_test_query(name, FIPSD_DNS_TYPE_AAAA, d); rn = answer(d, n, CLIENT_PORT, &empty);
      CHECK(rn && resolved && word(reply, 6) == 1 && !memcmp(first_rdata(reply, rn, &rdlen), id.address.bytes, 16) && rdlen == 16);
      CHECK(!memcmp(resolved_peer.node_addr.bytes, id.node_addr.bytes, 16));
      fipsd_hosts_map hosts; fipsd_hosts_map_init(&hosts); map_insert(&hosts, "gateway", id.npub);
      n = build_test_query("gateway.fips", FIPSD_DNS_TYPE_AAAA, d); rn = answer(d, n, CLIENT_PORT, &hosts);
      CHECK(rn && resolved && word(reply, 6) == 1 && !memcmp(first_rdata(reply, rn, &rdlen), id.address.bytes, 16));
      n = build_test_query("unknown.fips", FIPSD_DNS_TYPE_AAAA, d); rn = answer(d, n, CLIENT_PORT, &empty);
      CHECK(rn && !resolved && rcode(reply) == FIPSD_DNS_RCODE_NXDOMAIN && word(reply, 6) == 0);
      n = build_test_query(name, 1, d); rn = answer(d, n, CLIENT_PORT, &empty);
      CHECK(rn && !resolved && rcode(reply) == FIPSD_DNS_RCODE_NOERROR && word(reply, 6) == 0);
      /* PTR: a resolvable .fips name gets NODATA like any other type; a
       * reverse name is not under fips and gets NXDOMAIN (upstream parity:
       * the responder serves .fips only). */
      n = build_test_query(name, 12, d); rn = answer(d, n, CLIENT_PORT, &empty);
      CHECK(rn && !resolved && rcode(reply) == FIPSD_DNS_RCODE_NOERROR && word(reply, 6) == 0 && (word(reply, 2) & 0x0400));
      n = build_test_query("1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.d.f.ip6.arpa", 12, d); rn = answer(d, n, CLIENT_PORT, &hosts);
      CHECK(rn && !resolved && rcode(reply) == FIPSD_DNS_RCODE_NXDOMAIN && word(reply, 6) == 0);
      fipsd_hosts_map_clear(&hosts); }
    /* --- mesh-interface filter --- */
    CHECK(fipsd_dns_is_mesh_interface_query(true, 7, true, 7) && !fipsd_dns_is_mesh_interface_query(true, 1, true, 7));
    CHECK(!fipsd_dns_is_mesh_interface_query(false, 0, true, 7) && !fipsd_dns_is_mesh_interface_query(true, 7, false, 0));
    /* --- reply_path, strict pin (Linux) --- */
    { struct row { const char *ip; int arrival; int mesh; bool wildcard; fipsd_dns_reply_kind kind; uint32_t ifindex; };
      static const struct row strict[] = {
        {"fd11:2233::7", 2, 3, true, FIPSD_DNS_REPLY_PINNED, 2}, {"fd11:2233::7", -1, 3, true, FIPSD_DNS_REPLY_ROUTED, 0},
        {"fdab::2", 5, 3, true, FIPSD_DNS_REPLY_PINNED, 5}, {"2001:db8::2", 2, 3, true, FIPSD_DNS_REPLY_ROUTED, 0},
        {"fe80::1", 2, 3, true, FIPSD_DNS_REPLY_ROUTED, 0}, {"::1", 1, 3, true, FIPSD_DNS_REPLY_ROUTED, 0},
        {"::ffff:192.0.2.2", 2, 3, true, FIPSD_DNS_REPLY_ROUTED, 0}, {"::ffff:127.0.0.1", 1, 3, true, FIPSD_DNS_REPLY_ROUTED, 0},
        {"192.0.2.2", 2, 3, true, FIPSD_DNS_REPLY_ROUTED, 0},
        {"fd11:2233::7", 2, -1, true, FIPSD_DNS_REPLY_PINNED, 2}, {"fd11:2233::7", -1, -1, true, FIPSD_DNS_REPLY_ROUTED, 0},
        {"fd11:2233::7", 2, 3, false, FIPSD_DNS_REPLY_WITHHELD, 0}, {"fdab::2", 5, 3, false, FIPSD_DNS_REPLY_WITHHELD, 0},
        {"fd11:2233::7", 1, 3, false, FIPSD_DNS_REPLY_WITHHELD, 0}, {"fd11:2233::7", -1, 3, false, FIPSD_DNS_REPLY_WITHHELD, 0},
        {"fd11:2233::7", 3, 3, false, FIPSD_DNS_REPLY_PINNED, 3}, {"2001:db8::2", 2, 3, false, FIPSD_DNS_REPLY_ROUTED, 0},
        {"fe80::1", 2, 3, false, FIPSD_DNS_REPLY_ROUTED, 0}, {"::1", 1, 3, false, FIPSD_DNS_REPLY_ROUTED, 0},
        {"::ffff:192.0.2.2", 2, 3, false, FIPSD_DNS_REPLY_ROUTED, 0}, {"192.0.2.2", 2, 3, false, FIPSD_DNS_REPLY_ROUTED, 0},
        {"fd11:2233::7", 2, -1, false, FIPSD_DNS_REPLY_RETURNING, 2}, {"fd11:2233::7", -1, -1, false, FIPSD_DNS_REPLY_ROUTED, 0}};
      static const struct row loose[] = {
        {"fd11:2233::7", 2, 3, true, FIPSD_DNS_REPLY_RETURNING, 2}, {"fd11:2233::7", -1, 3, true, FIPSD_DNS_REPLY_ROUTED, 0},
        {"fdab::2", 5, 3, true, FIPSD_DNS_REPLY_RETURNING, 5}, {"2001:db8::2", 2, 3, true, FIPSD_DNS_REPLY_ROUTED, 0},
        {"fe80::1", 2, 3, true, FIPSD_DNS_REPLY_ROUTED, 0}, {"::1", 1, 3, true, FIPSD_DNS_REPLY_ROUTED, 0},
        {"::ffff:192.0.2.2", 2, 3, true, FIPSD_DNS_REPLY_ROUTED, 0}, {"192.0.2.2", 2, 3, true, FIPSD_DNS_REPLY_ROUTED, 0},
        {"fd11:2233::7", 2, -1, true, FIPSD_DNS_REPLY_RETURNING, 2}, {"fd11:2233::7", -1, -1, true, FIPSD_DNS_REPLY_ROUTED, 0},
        {"fd11:2233::7", 2, 3, false, FIPSD_DNS_REPLY_RETURNING, 2}, {"fdab::2", 5, 3, false, FIPSD_DNS_REPLY_RETURNING, 5},
        {"fd11:2233::7", -1, 3, false, FIPSD_DNS_REPLY_WITHHELD, 0}, {"fd11:2233::7", 3, 3, false, FIPSD_DNS_REPLY_RETURNING, 3},
        {"2001:db8::2", 2, 3, false, FIPSD_DNS_REPLY_ROUTED, 0}, {"::ffff:192.0.2.2", 2, 3, false, FIPSD_DNS_REPLY_ROUTED, 0},
        {"192.0.2.2", 2, 3, false, FIPSD_DNS_REPLY_ROUTED, 0},
        {"fd11:2233::7", 2, -1, false, FIPSD_DNS_REPLY_RETURNING, 2}, {"fd11:2233::7", -1, -1, false, FIPSD_DNS_REPLY_ROUTED, 0}};
      for (int mode = 0; mode < 2; ++mode) {
        const struct row *rows = mode ? loose : strict; size_t count = mode ? sizeof loose / sizeof loose[0] : sizeof strict / sizeof strict[0];
        for (size_t i = 0; i < count; ++i) {
            struct sockaddr_storage src = at(rows[i].ip, 4444);
            fipsd_dns_reply_path p = fipsd_dns_reply_path_for((struct sockaddr *)&src, rows[i].arrival >= 0, (uint32_t)rows[i].arrival,
                                                              rows[i].mesh >= 0, (uint32_t)rows[i].mesh, rows[i].wildcard, mode == 0);
            if (p.kind != rows[i].kind || (p.kind != FIPSD_DNS_REPLY_ROUTED && p.kind != FIPSD_DNS_REPLY_WITHHELD && p.ifindex != rows[i].ifindex)) {
                fprintf(stderr, "reply_path mode %d row %zu (%s arrival %d mesh %d wildcard %d): kind %d ifindex %u\n", mode, i, rows[i].ip, rows[i].arrival, rows[i].mesh, rows[i].wildcard, p.kind, p.ifindex); abort();
            }
        }
      } }
    /* --- mesh_query by destination or mesh source when the index is unknown --- */
    { struct in6_addr own = ip6("fd99::1");
      fipsd_dns_mesh_side known = {true, 3, true, own}, unknown = {false, 0, true, own}, no_tun = {0};
      struct row { int ifindex; const char *dst; const char *src; const fipsd_dns_mesh_side *mesh; bool dropped; };
      const struct row rows[] = {
        {3, "fd99::1", "fd11::7", &known, true}, {2, "fd99::1", "fd11::7", &known, false}, {-1, NULL, "fd11::7", &known, false},
        {2, "fd99::1", "fd11::7", &no_tun, false}, {-1, NULL, "fd11::7", &no_tun, false},
        {2, "fd99::1", "fd11::7", &unknown, true}, {2, "fd99::1", "2001:db8::2", &unknown, true}, {1, "fd99::1", "fd99::1", &unknown, false},
        {2, "2001:db8:1::1", "fd11::7", &unknown, false}, {2, "fdab::1", "fdab::2", &unknown, false}, {2, "::ffff:192.0.2.1", "192.0.2.2", &unknown, false},
        {-1, NULL, "fd11::7", &unknown, true}, {-1, NULL, "fdab::2", &unknown, true}, {-1, NULL, "2001:db8::2", &unknown, false},
        {-1, NULL, "::ffff:192.0.2.2", &unknown, false}, {-1, NULL, "192.0.2.2", &unknown, false}, {-1, NULL, "fd99::1", &unknown, false}};
      for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
        fipsd_dns_arrival arrival = {rows[i].ifindex >= 0, (uint32_t)(rows[i].ifindex < 0 ? 0 : rows[i].ifindex), rows[i].dst != NULL, {{{0}}}};
        if (rows[i].dst) arrival.dst = ip6(rows[i].dst);
        struct sockaddr_storage src = at(rows[i].src, 4444);
        if (fipsd_dns_mesh_query(&arrival, (struct sockaddr *)&src, rows[i].mesh) != rows[i].dropped) { fprintf(stderr, "mesh_query row %zu\n", i); abort(); }
      } }
    /* --- wildcard_bind --- */
    { struct sockaddr_storage s;
      s = at("::", 5354); CHECK(fipsd_dns_wildcard_bind((struct sockaddr *)&s)); s = at("0.0.0.0", 5354); CHECK(fipsd_dns_wildcard_bind((struct sockaddr *)&s));
      s = at("::1", 5354); CHECK(!fipsd_dns_wildcard_bind((struct sockaddr *)&s)); s = at("127.0.0.1", 5354); CHECK(!fipsd_dns_wildcard_bind((struct sockaddr *)&s));
      s = at("2001:db8:1::1", 5354); CHECK(!fipsd_dns_wildcard_bind((struct sockaddr *)&s)); s = at("192.0.2.1", 5354); CHECK(!fipsd_dns_wildcard_bind((struct sockaddr *)&s));
      CHECK(!fipsd_dns_wildcard_bind(NULL)); }
    /* --- returns_by --- */
    { fips_onlink_prefix storage[4], entries[3] = {{ip("2001:db8:1::1"), 64, true, 2}, {ip("fdab::1"), 64, true, 2}, {ip("fd99::1"), 128, true, 3}};
      fips_onlink_prefixes prefixes, held, bare, none, failed = fips_onlink_failed();
      fips_onlink_prefix s2[4], s3[4], s4[4];
      CHECK(fips_onlink_from_indexed(&prefixes, storage, 4, entries, 3) == FIPS_OK);
      CHECK(fipsd_dns_returns_by(ip("fdab::1"), ip("fdab::2"), 2, &prefixes, NULL));
      CHECK(fipsd_dns_returns_by(ip("2001:db8:1::1"), ip("fdab::2"), 2, &prefixes, NULL));
      CHECK(!fipsd_dns_returns_by(ip("fd99::1"), ip("fd11:2233::7"), 2, &prefixes, NULL));
      CHECK(fipsd_dns_returns_by(ip("fd99::1"), ip("fd11:2233::7"), 3, &prefixes, NULL));
      CHECK(!fipsd_dns_returns_by(ip("fdab::1"), ip("fdab::2"), 7, &prefixes, NULL));
      /* delivered locally, whatever interface */
      fips_onlink_prefix only[1] = {{ip("fd99::1"), 128, true, 3}};
      CHECK(fips_onlink_from_indexed(&held, s2, 4, only, 1) == FIPS_OK);
      CHECK(fipsd_dns_returns_by(ip("fd99::1"), ip("fd99::1"), 2, &held, NULL));
      CHECK(fips_onlink_from_indexed(&none, s3, 4, NULL, 0) == FIPS_OK);
      CHECK(fipsd_dns_returns_by(ip("fd99::1"), ip("fd99::1"), 2, &none, NULL));
      /* empty or failed set holds nothing */
      CHECK(!fipsd_dns_returns_by(ip("fdab::1"), ip("fdab::2"), 2, &none, NULL) && !fipsd_dns_returns_by(ip("fdab::1"), ip("fdab::2"), 2, &failed, NULL));
      /* an addressless mesh interface returns only to an on-link destination */
      struct in6_addr mesh = ip6("fd99::1");
      CHECK(fips_onlink_from_indexed(&bare, s4, 4, entries, 2) == FIPS_OK);
      CHECK(!fipsd_dns_returns_by(ip("fd99::1"), ip("fd11:2233::7"), 2, &prefixes, &mesh) && fipsd_dns_returns_by(ip("fdab::1"), ip("fdab::2"), 2, &prefixes, &mesh));
      CHECK(fipsd_dns_returns_by(ip("fdab::1"), ip("fdab::2"), 2, &bare, &mesh) && fipsd_dns_returns_by(ip("2001:db8:1::1"), ip("2001:db8:1::2"), 2, &bare, &mesh));
      CHECK(!fipsd_dns_returns_by(ip("fdab::1"), ip("fd11:2233::7"), 2, &bare, &mesh) && !fipsd_dns_returns_by(ip("fdab::1"), ip("fdab:1::9"), 2, &bare, &mesh));
      CHECK(!fipsd_dns_returns_by(ip("fdab::1"), ip("fdab::2"), 7, &bare, &mesh) && fipsd_dns_returns_by(ip("fdab::1"), ip("fdab::1"), 2, &bare, &mesh));
      CHECK(fipsd_dns_returns_by(ip("fdab::1"), ip("fd11:2233::7"), 2, &bare, NULL)); }
    /* --- screening through the resolver --- */
    { /* reflection: an answer record in the query is never echoed */
      n = aaaa_query(0x1111, "example.com", d); CHECK(answer(d, n, CLIENT_PORT, &empty));
      uint8_t forged[64]; size_t fn = wire_name("evil.fips", forged);
      static const uint8_t tail[] = {0, 1, 0, 1, 0, 1, 0x86, 0x9f, 0, 4, 1, 2, 3, 4}; memcpy(forged + fn, tail, sizeof tail); fn += sizeof tail;
      uint8_t wire[64], body[600]; size_t wn = wire_name("example.com", wire); size_t bn = question(wire, wn, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, body);
      memcpy(body + bn, forged, fn); bn += fn;
      static const uint16_t shaped[4] = {1, 1, 0, 0}; n = message(0x1112, 0, shaped, body, bn, d);
      rn = answer(d, n, CLIENT_PORT, &empty);
      if (rn) for (size_t i = 0; i + fn <= rn; ++i) CHECK(memcmp(reply + i, forged, fn) != 0);
      assert_bounded(d, n, rn);
      /* a 511-byte query with twelve pointer SRV records */
      uint8_t lname[300]; size_t at2 = 0; static const size_t lens[4] = {63, 63, 63, 61};
      for (size_t i = 0; i < 4; ++i) { lname[at2++] = (uint8_t)lens[i]; memset(lname + at2, 'a', lens[i]); at2 += lens[i]; } lname[at2++] = 0;
      bn = question(lname, at2, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, body);
      uint8_t big[600]; memcpy(big, body, bn); size_t bl = bn;
      for (int i = 0; i < 12; ++i) { static const uint8_t srv[] = {0xC0, 0x0C, 0, 33, 0, 1, 0, 0, 1, 0x2c, 0, 8, 0, 0, 0, 0, 0, 53, 0xC0, 0x0C}; memcpy(big + bl, srv, sizeof srv); bl += sizeof srv; }
      static const uint16_t twelve[4] = {1, 12, 0, 0}; n = message(0x2222, 0, twelve, big, bl, d); CHECK(n == 511);
      static const uint16_t one[4] = {1, 0, 0, 0}; uint8_t control[400]; size_t cn = message(0x2223, 0, one, body, bn, control);
      CHECK(answer(control, cn, CLIENT_PORT, &empty)); assert_bounded(d, n, answer(d, n, CLIENT_PORT, &empty));
      /* a 277-byte two-question query */
      at2 = 0; for (size_t i = 0; i < 4; ++i) { lname[at2++] = (uint8_t)lens[i]; memset(lname + at2, 0x01, lens[i]); at2 += lens[i]; } lname[at2++] = 0;
      bn = question(lname, at2, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, body);
      static const uint8_t second[] = {0xC0, 0x0D, 0x00, 0x1C, 0x00, 0x01}; memcpy(big, body, bn); memcpy(big + bn, second, sizeof second);
      static const uint16_t two[4] = {2, 0, 0, 0}; n = message(0x3333, 0, two, big, bn + sizeof second, d); CHECK(n == 277);
      cn = message(0x3334, 0, one, body, bn, control); CHECK(answer(control, cn, CLIENT_PORT, &empty));
      assert_bounded(d, n, answer(d, n, CLIENT_PORT, &empty));
      /* loops: a reply fed back gets no reply; chargen port; daytime text */
      n = aaaa_query(0x4444, "unknown.fips", d); rn = answer(d, n, CLIENT_PORT, &empty); CHECK(rn);
      uint8_t echoed[600]; memcpy(echoed, reply, rn); CHECK(answer(echoed, rn, CLIENT_PORT, &empty) == 0);
      identity id = generate(); snprintf(name, sizeof name, "%s.fips", id.npub);
      n = aaaa_query(0x5555, name, d); CHECK(answer(d, n, CLIENT_PORT, &empty) && answer(d, n, 19, &empty) == 0);
      const char *daytime = "Thu Oct  9 12:00:00 2026\r\n";
      CHECK(answer((const uint8_t *)daytime, strlen(daytime), CLIENT_PORT, &empty) == 0 && answer((const uint8_t *)daytime, strlen(daytime), 13, &empty) == 0);
      /* header handling */
      n = query(0x6666, 5 << 11, name, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, d); rn = answer(d, n, CLIENT_PORT, &empty);
      CHECK(rn == 12 && rcode(reply) == FIPSD_DNS_RCODE_NOTIMP && word(reply, 4) == 0 && ((word(reply, 2) >> 11) & 0x0F) == 5 && word(reply, 0) == 0x6666);
      n = query(0x7777, 0, name, 44, FIPSD_DNS_CLASS_IN, d); rn = answer(d, n, CLIENT_PORT, &empty);
      CHECK(rn && rcode(reply) == FIPSD_DNS_RCODE_NOERROR && word(reply, 6) == 0 && (word(reply, 2) & 0x0400) && rn == n && !memcmp(reply + 12, d + 12, n - 12));
      n = query(0x7778, 0, "unknown.fips", 44, FIPSD_DNS_CLASS_IN, d); rn = answer(d, n, CLIENT_PORT, &empty);
      CHECK(rn && rcode(reply) == FIPSD_DNS_RCODE_NXDOMAIN && word(reply, 6) == 0);
      n = query(0x8888, 0, name, FIPSD_DNS_TYPE_AAAA, 3, d); rn = answer(d, n, CLIENT_PORT, &empty);
      CHECK(rn && rcode(reply) == FIPSD_DNS_RCODE_REFUSED && word(reply, 4) == 1 && word(reply, 6) == 0 && rn == n && !memcmp(reply + 12, d + 12, n - 12));
      n = query(0x9999, 0x0110, name, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, d); rn = answer(d, n, CLIENT_PORT, &empty);
      CHECK(rn && (word(reply, 2) & 0x0110) == 0x0110);
      n = query(0xAAAA, 0x0040, name, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, d); rn = answer(d, n, CLIENT_PORT, &empty);
      CHECK(rn && !(word(reply, 2) & 0x0040) && word(reply, 6) == 1);
      static const uint16_t none4[4] = {0, 0, 0, 0}; n = message(0xBBBB, 0, none4, NULL, 0, d); rn = answer(d, n, CLIENT_PORT, &empty);
      CHECK(rn == 12 && rcode(reply) == FIPSD_DNS_RCODE_FORMERR && word(reply, 0) == 0xBBBB);
      /* healthy path */
      n = aaaa_query(0xCCCC, name, d); rn = answer(d, n, CLIENT_PORT, &empty); size_t rdlen;
      CHECK(rn == n + 28 && word(reply, 0) == 0xCCCC && (word(reply, 2) & 0x0400) && word(reply, 6) == 1);
      CHECK(!memcmp(first_rdata(reply, rn, &rdlen), id.address.bytes, 16) && resolved && !memcmp(resolved_peer.node_addr.bytes, id.node_addr.bytes, 16));
      n = padded_query(0xCCCD, name, 439, d); rn = answer(d, n, CLIENT_PORT, &empty);
      CHECK(rn && word(reply, 6) == 1 && word(reply, 10) == 0); assert_bounded(d, n, rn);
      fipsd_hosts_map hosts; fipsd_hosts_map_init(&hosts); map_insert(&hosts, "gateway", id.npub);
      const char *qnames[2] = {name, "gateway.fips"}; static const uint16_t types[2] = {1, 65};
      for (int qi = 0; qi < 2; ++qi) for (int ti = 0; ti < 2; ++ti) {
          n = query(0xCCCE, 0, qnames[qi], types[ti], FIPSD_DNS_CLASS_IN, d); rn = answer(d, n, CLIENT_PORT, &hosts);
          CHECK(rn && rcode(reply) == FIPSD_DNS_RCODE_NOERROR && word(reply, 6) == 0);
      }
      /* only a name whose last label is fips resolves; a dot inside a label does not split it */
      { const uint8_t *l1[2] = {(const uint8_t *)"gateway", (const uint8_t *)"fips"}; size_t s1[2] = {7, 4};
        const uint8_t *l2[2] = {(const uint8_t *)"GateWay", (const uint8_t *)"FiPs"}; size_t s2[2] = {7, 4};
        const uint8_t *l3[2] = {(const uint8_t *)id.npub, (const uint8_t *)"fips"}; size_t s3[2] = {strlen(id.npub), 4};
        const uint8_t *const *good[3] = {l1, l2, l3}; const size_t *gs[3] = {s1, s2, s3};
        for (int i = 0; i < 3; ++i) { n = wire_query(good[i], gs[i], 2, 28, d); rn = answer(d, n, 4444, &hosts); CHECK(rn && rcode(reply) == 0 && word(reply, 6) == 1 && resolved); }
        const uint8_t *b1[1] = {(const uint8_t *)"gateway.fips"}; size_t t1[1] = {12};
        const uint8_t *b2[2] = {(const uint8_t *)"gateway", (const uint8_t *)"fips."}; size_t t2[2] = {7, 5};
        const uint8_t *b3[2] = {(const uint8_t *)id.npub, (const uint8_t *)"fips."}; size_t t3[2] = {strlen(id.npub), 5};
        const uint8_t *b4[3] = {(const uint8_t *)"gateway", (const uint8_t *)"fips", (const uint8_t *)"example"}; size_t t4[3] = {7, 4, 7};
        const uint8_t *const *bad[4] = {b1, b2, b3, b4}; const size_t *bs[4] = {t1, t2, t3, t4}; size_t bc[4] = {1, 2, 2, 3};
        for (int i = 0; i < 4; ++i) for (int t = 0; t < 2; ++t) {
            n = wire_query(bad[i], bs[i], bc[i], t ? 16 : 28, d); rn = answer(d, n, 4444, &hosts);
            CHECK(rn && rcode(reply) == FIPSD_DNS_RCODE_NXDOMAIN && word(reply, 6) == 0 && !resolved);
        }
        /* every AAAA answer's owner is a pointer to the question name */
        const uint8_t *o2[2] = {(const uint8_t *)"GATEWAY", (const uint8_t *)"Fips"}; size_t os2[2] = {7, 4};
        n = wire_query(l1, s1, 2, 28, d); rn = answer(d, n, 4444, &hosts); CHECK(word(reply, 6) == 1 && reply[n] == 0xC0 && reply[n + 1] == 0x0C);
        n = wire_query(o2, os2, 2, 28, d); rn = answer(d, n, 4444, &hosts); CHECK(word(reply, 6) == 1 && reply[n] == 0xC0 && reply[n + 1] == 0x0C); }
      fipsd_hosts_map_clear(&hosts);
      /* an npub .fips name resolves in lower, upper and mixed case, echoing the name as sent */
      { char upper[FIPS_NIP19_CAPACITY], alt[FIPS_NIP19_CAPACITY], ncap[FIPS_NIP19_CAPACITY];
        for (size_t i = 0; i <= strlen(id.npub); ++i) { upper[i] = (char)toupper((unsigned char)id.npub[i]); alt[i] = i % 2 == 0 ? (char)toupper((unsigned char)id.npub[i]) : id.npub[i]; }
        snprintf(ncap, sizeof ncap, "N%s", id.npub + 1);
        const char *labels[4] = {id.npub, upper, alt, ncap};
        for (int i = 0; i < 4; ++i) {
            snprintf(name, sizeof name, "%s.fips", labels[i]);
            n = build_test_query(name, FIPSD_DNS_TYPE_AAAA, d); rn = answer(d, n, CLIENT_PORT, &empty);
            CHECK(rn && rcode(reply) == 0 && word(reply, 6) == 1 && !memcmp(reply + 12, d + 12, n - 12) && !memcmp(first_rdata(reply, rn, &rdlen), id.address.bytes, 16));
        } }
      /* a bech32m or padded npub gets NXDOMAIN; the canonical one resolves */
      { const char *bad[3] = {"npub1lycg5qvjtrp3qjf5f7zl382j9x6nrjz9sdhenvyxq8c3808qxmus05sxlc", "npub1lycg5qvjtrp3qjf5f7zl382j9x6nrjz9sdhenvyxq8c3808qxmu3875l8g", "npub1lycg5qvjtrp3qjf5f7zl382j9x6nrjz9sdhenvyxq8c3808qxmulser6qq"};
        for (int i = 0; i < 3; ++i) { snprintf(name, sizeof name, "%s.fips", bad[i]); n = build_test_query(name, FIPSD_DNS_TYPE_AAAA, d); rn = answer(d, n, CLIENT_PORT, &empty); CHECK(rn && rcode(reply) == FIPSD_DNS_RCODE_NXDOMAIN && word(reply, 6) == 0); }
        n = build_test_query("npub1lycg5qvjtrp3qjf5f7zl382j9x6nrjz9sdhenvyxq8c3808qxmus6gq266.fips", FIPSD_DNS_TYPE_AAAA, d); rn = answer(d, n, CLIENT_PORT, &empty);
        CHECK(rn && rcode(reply) == 0); } }
    /* --- sockets on loopback --- */
    { /* recv_with_pktinfo reports the destination address and an arrival interface */
      struct sockaddr_storage server_addr, client_addr; socklen_t sl, cl;
      int server = bind_loopback("::1", &server_addr, &sl), client = bind_loopback("::1", &client_addr, &cl);
      CHECK(sendto(client, "hello", 5, 0, (struct sockaddr *)&server_addr, sl) == 5);
      uint8_t buf[32]; struct sockaddr_storage src; socklen_t src_len; fipsd_dns_arrival arrival;
      ssize_t got = fipsd_dns_recv_with_pktinfo(server, buf, sizeof buf, &src, &src_len, &arrival, 5000);
      CHECK(got == 5 && !memcmp(buf, "hello", 5) && src.ss_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *)&src)->sin6_addr));
      CHECK(arrival.has_dst && IN6_IS_ADDR_LOOPBACK(&arrival.dst) && arrival.has_ifindex);
      uint32_t lo = fipsd_dns_lookup_ifindex("lo"); if (!lo) lo = fipsd_dns_lookup_ifindex("lo0");
      CHECK(lo && arrival.ifindex == lo);
      /* a routed reply is delivered; a returning reply to a local address is delivered */
      const char *why = NULL;
      CHECK(fipsd_dns_send_reply(server, (const uint8_t *)"routed", 6, (struct sockaddr *)&client_addr, cl, (fipsd_dns_reply_path){FIPSD_DNS_REPLY_ROUTED, 0}, NULL, &why) == 6);
      CHECK(recv_within(client, buf, sizeof buf, 5000) == 6 && !memcmp(buf, "routed", 6));
      CHECK(fipsd_dns_send_reply(server, (const uint8_t *)"returning", 9, (struct sockaddr *)&client_addr, cl, (fipsd_dns_reply_path){FIPSD_DNS_REPLY_RETURNING, 0x7ffffff0}, NULL, &why) == 9);
      CHECK(recv_within(client, buf, sizeof buf, 5000) == 9 && !memcmp(buf, "returning", 9));
      /* a withheld reply is not sent on any target */
      CHECK(fipsd_dns_send_reply(server, (const uint8_t *)"withheld", 8, (struct sockaddr *)&client_addr, cl, (fipsd_dns_reply_path){FIPSD_DNS_REPLY_WITHHELD, 0}, NULL, &why) == 0);
      CHECK(why && !strcmp(why, "mesh-source-off-mesh-interface") && nothing_queued(client));
      /* the probe picks the destination itself for a local address */
      fips_ip_addr source; CHECK(fipsd_dns_probe_source((struct sockaddr *)&client_addr, cl, &source) && source.width == 16 && source.bytes[15] == 1);
#ifdef __linux__
      /* a reply pinned to loopback reaches a loopback client; one pinned to a missing interface fails and is not delivered */
      CHECK(fipsd_dns_send_pinned(server, (const uint8_t *)"pinned", 6, (struct sockaddr_in6 *)&client_addr, lo) == 6);
      CHECK(recv_within(client, buf, sizeof buf, 5000) == 6 && !memcmp(buf, "pinned", 6));
      CHECK(fipsd_dns_send_pinned(server, (const uint8_t *)"pinned", 6, (struct sockaddr_in6 *)&client_addr, 0x7ffffff0) < 0 && nothing_queued(client));
#endif
      close(server); close(client);
      /* an IPv4 loopback bind works too and answers a v4 client with the pure path */
      int v4 = bind_loopback("127.0.0.1", &server_addr, &sl), v4c = bind_loopback("127.0.0.1", &client_addr, &cl);
      n = aaaa_query(0x4242, "unknown.fips", d);
      CHECK(sendto(v4c, d, n, 0, (struct sockaddr *)&server_addr, sl) == (ssize_t)n);
      got = fipsd_dns_recv_with_pktinfo(v4, d, sizeof d, &src, &src_len, &arrival, 5000);
      CHECK(got == (ssize_t)n && src.ss_family == AF_INET && !arrival.has_ifindex);
      close(v4); close(v4c); }
    fipsd_hosts_map_clear(&empty);
    fips_identity_destroy_secp(secp);
    puts("dns resolver: resolution, mesh filter, reply paths, returns_by, screening, loopback sockets: ok");
    return 0;
}
