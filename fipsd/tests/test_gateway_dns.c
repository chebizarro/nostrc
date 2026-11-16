/* Gateway DNS proxy tables (nostrc-iq1n9.3), from upstream src/gateway/dns.rs
 * and tests/dns.rs: what each datagram gets (screened by the daemon's codec),
 * what reaches the upstream and what comes back, which pool events a query
 * mints, the startup probe, the listener and the serve loop's mesh filter.
 * The upstream is a fake on [::1] answering from a script on its own thread. */
#include "test_gateway.h"
#include "gateway/dns.h"
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>

#define TTL 60u
#define CLIENT_PORT 53000
#define TYPE_A 1u
#define TYPE_SSHFP 44u
#ifdef __APPLE__
#define LOOPBACK "lo0"
#else
#define LOOPBACK "lo"
#endif

static uint16_t word(const uint8_t *b, size_t off) { return (uint16_t)((b[off] << 8) | b[off + 1]); }
static uint8_t rcode_of(const uint8_t *r) { return (uint8_t)(word(r, 2) & 0x0F); }
static size_t wire_name(const char *name, uint8_t *out)
{
    size_t at = 0;
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        out[at++] = (uint8_t)len; memcpy(out + at, p, len); at += len;
        if (!dot) break;
        p = dot + 1;
    }
    out[at++] = 0;
    return at;
}
/* A query: header with id and flags, one question. */
static size_t query(uint16_t id, uint16_t flags, const char *name, uint16_t qtype, uint16_t qclass, uint8_t *out)
{
    memset(out, 0, 12);
    out[0] = (uint8_t)(id >> 8); out[1] = (uint8_t)id; out[2] = (uint8_t)(flags >> 8); out[3] = (uint8_t)flags; out[5] = 1;
    size_t at = 12 + wire_name(name, out + 12);
    out[at++] = (uint8_t)(qtype >> 8); out[at++] = (uint8_t)qtype; out[at++] = (uint8_t)(qclass >> 8); out[at++] = (uint8_t)qclass;
    return at;
}
static size_t aaaa_query(uint16_t id, const char *name, uint8_t *out) { return query(id, 0, name, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, out); }
/* An upstream answer: the query's header and question with QR, AA, the
 * query's RD and rcode, plus an AAAA for addr when given. */
static size_t upstream_answer(const uint8_t *q, size_t qlen, uint8_t rcode, const char *addr, uint8_t *out)
{
    memcpy(out, q, qlen);
    uint16_t flags = (uint16_t)(0x8400 | (word(q, 2) & 0x0100) | rcode);
    out[2] = (uint8_t)(flags >> 8); out[3] = (uint8_t)flags;
    size_t at = qlen;
    if (addr) {
        out[6] = 0; out[7] = 1;
        const uint8_t rr[] = {0xC0, 0x0C, 0, 28, 0, 1, 0, 0, 0, TTL, 0, 16};
        memcpy(out + at, rr, sizeof rr); at += sizeof rr;
        struct in6_addr a = gw_v6(addr); memcpy(out + at, &a, 16); at += 16;
    }
    return at;
}
/* An upstream answer with a chosen ID and name, for the mismatch tests. */
static size_t build_answer(uint16_t id, const char *qname, const char *addr, uint8_t *out)
{
    uint8_t q[512]; size_t qlen = aaaa_query(id, qname, q);
    return upstream_answer(q, qlen, 0, addr, out);
}

/* ---- the fake upstream ----------------------------------------------------- */
typedef size_t (*script_fn)(void *ctx, const uint8_t *q, size_t qlen, int k, uint8_t *out); /* k-th reply; 0 ends */
typedef struct upstream {
    int fd, foreign, wake[2];
    struct sockaddr_in6 addr;
    pthread_t thread;
    script_fn script; void *ctx;
    nsync_mu mu; int received; bool forge_from_foreign;
} upstream;
static void *upstream_thread(void *arg)
{
    upstream *u = arg;
    uint8_t buf[4096], out[4096];
    for (;;) {
        struct pollfd p[2] = {{.fd = u->fd, .events = POLLIN}, {.fd = u->wake[0], .events = POLLIN}};
        if (poll(p, 2, -1) < 0) continue;
        if (p[1].revents) break;
        struct sockaddr_in6 src; socklen_t slen = sizeof src;
        ssize_t n = recvfrom(u->fd, buf, sizeof buf, 0, (struct sockaddr *)&src, &slen);
        if (n < 0) continue;
        nsync_mu_lock(&u->mu); u->received++; nsync_mu_unlock(&u->mu);
        for (int k = 0;; ++k) {
            size_t len = u->script(u->ctx, buf, (size_t)n, k, out);
            if (!len) break;
            /* Reply 0 comes from the foreign socket when forging. */
            int from = u->forge_from_foreign && k == 0 ? u->foreign : u->fd;
            (void)sendto(from, out, len, 0, (struct sockaddr *)&src, slen);
        }
    }
    return NULL;
}
static int bound_v6_socket(struct sockaddr_in6 *addr)
{
    int fd = socket(AF_INET6, SOCK_DGRAM, 0); CHECK(fd >= 0);
    struct sockaddr_in6 a = {.sin6_family = AF_INET6}; a.sin6_addr = in6addr_loopback;
    CHECK(bind(fd, (struct sockaddr *)&a, sizeof a) == 0);
    socklen_t len = sizeof *addr; CHECK(getsockname(fd, (struct sockaddr *)addr, &len) == 0);
    return fd;
}
static void upstream_start_forging(upstream *u, script_fn script, void *ctx, bool forge)
{
    memset(u, 0, sizeof *u);
    u->forge_from_foreign = forge;
    u->fd = bound_v6_socket(&u->addr);
    struct sockaddr_in6 unused; u->foreign = bound_v6_socket(&unused);
    CHECK(pipe(u->wake) == 0);
    u->script = script; u->ctx = ctx; nsync_mu_init(&u->mu);
    CHECK(pthread_create(&u->thread, NULL, upstream_thread, u) == 0);
}
static void upstream_start(upstream *u, script_fn script, void *ctx) { upstream_start_forging(u, script, ctx, false); }
static int upstream_received(upstream *u) { nsync_mu_lock(&u->mu); int n = u->received; nsync_mu_unlock(&u->mu); return n; }
static void upstream_stop(upstream *u)
{
    CHECK(write(u->wake[1], "x", 1) == 1);
    pthread_join(u->thread, NULL);
    close(u->fd); close(u->foreign); close(u->wake[0]); close(u->wake[1]);
}
/* Scripts. */
static const char *mesh_reply = "fd00::1";
static size_t mesh_script(void *ctx, const uint8_t *q, size_t qlen, int k, uint8_t *out)
{ const char *addr = ctx ? ctx : mesh_reply; return k ? 0 : upstream_answer(q, qlen, 0, addr, out); }
static size_t rcode_script(void *ctx, const uint8_t *q, size_t qlen, int k, uint8_t *out)
{ return k ? 0 : upstream_answer(q, qlen, (uint8_t)(uintptr_t)ctx, NULL, out); }
static size_t id_mismatch_script(void *ctx, const uint8_t *q, size_t qlen, int k, uint8_t *out)
{
    (void)ctx; (void)qlen;
    uint16_t id = word(q, 0);
    if (k == 0) return build_answer((uint16_t)(id + 1), "test.fips", "2001:db8::1", out);
    if (k == 1) return build_answer(id, "test.fips", "fd00::1", out);
    return 0;
}
static size_t question_mismatch_script(void *ctx, const uint8_t *q, size_t qlen, int k, uint8_t *out)
{
    (void)ctx; (void)qlen;
    uint16_t id = word(q, 0);
    if (k == 0) return build_answer(id, "other.fips", "2001:db8::1", out);
    if (k == 1) return build_answer(id, "test.fips", "fd00::1", out);
    return 0;
}
static size_t forged_script(void *ctx, const uint8_t *q, size_t qlen, int k, uint8_t *out)
{
    (void)ctx; (void)qlen;
    uint16_t id = word(q, 0);
    if (k == 0) return build_answer(id, "test.fips", "2001:db8::1", out); /* from the foreign socket */
    if (k == 1) return build_answer(id, "test.fips", "fd00::1", out);
    return 0;
}
static size_t garbage_then_answer_script(void *ctx, const uint8_t *q, size_t qlen, int k, uint8_t *out)
{
    (void)ctx;
    if (k == 0) { memcpy(out, "not dns", 7); return 7; }
    if (k == 1) return upstream_answer(q, qlen, 0, "fd00::1", out);
    return 0;
}

/* ---- the resolver under test ------------------------------------------------ */
typedef struct sink { nsync_mu mu; fipsd_gw_pool_event events[16]; size_t n; } sink;
static int emit(void *user, const fipsd_gw_pool_event *e)
{
    sink *s = user;
    nsync_mu_lock(&s->mu);
    CHECK(s->n < 16); s->events[s->n++] = *e;
    nsync_mu_unlock(&s->mu);
    return 0;
}
typedef struct gw { fipsd_gw_shared_pool pool; sink events; fipsd_gw_dns_resolver r; } gw;
static void gw_init_pool(gw *g, const struct sockaddr_in6 *upstream, fipsd_gw_pool *pool)
{
    memset(g, 0, sizeof *g);
    nsync_mu_init(&g->pool.mu); g->pool.pool = pool;
    nsync_mu_init(&g->events.mu);
    fipsd_gw_dns_resolver_init(&g->r, (const struct sockaddr *)upstream, sizeof *upstream, TTL, &g->pool, emit, &g->events);
    g->r.upstream_timeout_ms = 2000;
}
static void gw_init(gw *g, const struct sockaddr_in6 *upstream)
{
    fipsd_gw_pool_error e;
    gw_init_pool(g, upstream, fipsd_gw_pool_new("fd01::/112", TTL, 30, &e));
}
static void gw_free(gw *g) { fipsd_gw_pool_free(g->pool.pool); }
/* An address no upstream listens on, for queries that never reach one. */
static struct sockaddr_in6 no_upstream(void)
{ struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(9)}; a.sin6_addr = in6addr_loopback; return a; }
/* The reply to datagram from CLIENT_PORT: its length, 0 for no reply. */
static size_t ask(gw *g, const uint8_t *datagram, size_t len, uint8_t *out)
{
    fipsd_gw_dns_outcome outcome; fipsd_dns_drop_reason reason; uint8_t rcode;
    size_t n = fipsd_gw_dns_respond(&g->r, datagram, len, CLIENT_PORT, out, FIPSD_DNS_MAX_REPLY, &outcome, &reason, &rcode);
    if (outcome == FIPSD_GW_DNS_DROP) CHECK(n == 0);
    return n;
}
static size_t ask_alone(const uint8_t *datagram, size_t len, uint8_t *out)
{ struct sockaddr_in6 none = no_upstream(); gw g; gw_init(&g, &none); size_t n = ask(&g, datagram, len, out); gw_free(&g); return n; }
/* The pool address in a reply's one AAAA answer. */
static struct in6_addr pool_address(const uint8_t *reply, size_t len)
{
    fipsd_gw_dns_message m;
    CHECK(fipsd_gw_dns_parse_message(reply, len, &m));
    CHECKM(m.rcode == 0, "rcode %u", m.rcode);
    CHECKM(m.has_aaaa, "an AAAA answer");
    CHECKM(m.aaaa.s6_addr[0] == 0xfd && m.aaaa.s6_addr[1] == 0x01, "a pool address");
    return m.aaaa;
}
/* The owner of a reply's one authority record, an SOA owned by a pointer into the question. */
static void soa_owner(const uint8_t *reply, size_t query_len, char *out)
{
    CHECKM(word(reply, 6) == 0, "no answers"); CHECKM(word(reply, 8) == 1, "one authority record");
    const uint8_t *rec = reply + query_len;
    CHECKM((rec[0] & 0xC0) == 0xC0, "the SOA owner is a pointer");
    CHECKM(word(rec, 2) == 6, "the authority record is an SOA");
    size_t pos = word(rec, 0) & 0x3FFF, at = 0;
    while (reply[pos]) {
        size_t l = reply[pos];
        if (at) out[at++] = '.';
        memcpy(out + at, reply + pos + 1, l); at += l; pos += 1 + l;
    }
    out[at] = 0;
}

static void test_node_addr_from_mesh(void)
{
    struct in6_addr mesh = gw_v6("fd00::1"); fips_node_addr n;
    CHECK(fipsd_gw_node_addr_from_mesh(&mesh, &n));
    CHECK(n.bytes[14] == 1 && n.bytes[0] == 0 && n.bytes[15] == 0);
}
static void test_node_addr_from_mesh_rejects_non_mesh(void)
{ struct in6_addr a = gw_v6("2001:db8::1"); fips_node_addr n; CHECK(!fipsd_gw_node_addr_from_mesh(&a, &n)); }
static void resolve_with(script_fn script, void *ctx, bool forge, const uint8_t *q, size_t qlen, uint8_t *out, size_t *len, gw *g)
{
    upstream u; upstream_start_forging(&u, script, ctx, forge);
    gw_init(g, &u.addr);
    *len = ask(g, q, qlen, out);
    upstream_stop(&u);
}
static void expect_created(gw *g, const char *mesh)
{
    CHECK(g->events.n == 1 && g->events.events[0].kind == FIPSD_GW_EVENT_MAPPING_CREATED);
    struct in6_addr m = gw_v6(mesh); CHECK(fipsd_gw_ip6_eq(&g->events.events[0].mesh_addr, &m));
}
static void test_foreign_source_answer_not_accepted(void)
{
    uint8_t q[512], r[1024]; size_t qlen = aaaa_query(0x1234, "test.fips", q), len; gw g;
    resolve_with(forged_script, NULL, true, q, qlen, r, &len, &g);
    pool_address(r, len); expect_created(&g, "fd00::1"); gw_free(&g);
}
static void test_upstream_id_mismatch_discarded(void)
{
    uint8_t q[512], r[1024]; size_t qlen = aaaa_query(0x1234, "test.fips", q), len; gw g;
    resolve_with(id_mismatch_script, NULL, false, q, qlen, r, &len, &g);
    pool_address(r, len); expect_created(&g, "fd00::1"); gw_free(&g);
}
static void test_upstream_question_mismatch_discarded(void)
{
    uint8_t q[512], r[1024]; size_t qlen = aaaa_query(0x1234, "test.fips", q), len; gw g;
    resolve_with(question_mismatch_script, NULL, false, q, qlen, r, &len, &g);
    pool_address(r, len); expect_created(&g, "fd00::1"); gw_free(&g);
}
static void test_non_mesh_aaaa_rejected(void)
{
    uint8_t q[512], r[1024]; size_t qlen = aaaa_query(0x1234, "test.fips", q), len; gw g;
    resolve_with(mesh_script, "2001:db8::1", false, q, qlen, r, &len, &g);
    CHECK(len && rcode_of(r) == FIPSD_DNS_RCODE_SERVFAIL && g.events.n == 0); gw_free(&g);
}
static void test_healthy_path_resolves(void)
{
    uint8_t q[512], r[1024]; size_t qlen = aaaa_query(0x1234, "test.fips", q), len; gw g;
    resolve_with(mesh_script, NULL, false, q, qlen, r, &len, &g);
    pool_address(r, len); expect_created(&g, "fd00::1");
    CHECKM(word(r, 0) == 0x1234, "the client's own ID");
    gw_free(&g);
}
static void an_a_query_returns_nodata_and_mints_no_mapping(void)
{
    uint8_t q[512], r[1024]; size_t qlen = query(0x1234, 0, "test.fips", TYPE_A, FIPSD_DNS_CLASS_IN, q), len; gw g;
    resolve_with(mesh_script, NULL, false, q, qlen, r, &len, &g);
    CHECK(len && rcode_of(r) == 0 && word(r, 6) == 0);
    CHECKM(g.events.n == 0 && fipsd_gw_pool_len(g.pool.pool) == 0, "an A query minted a mapping");
    gw_free(&g);
}
/* Ask the gateway g for qname/qtype, the daemon answering mesh. */
static size_t ask_upstream(gw *g, uint16_t id, const char *qname, const char *mesh, uint16_t qtype, uint8_t *out)
{
    upstream u; upstream_start(&u, mesh_script, (void *)mesh);
    memcpy(&g->r.upstream, &u.addr, sizeof u.addr); g->r.upstream_len = sizeof u.addr;
    uint8_t q[512]; size_t qlen = query(id, 0, qname, qtype, FIPSD_DNS_CLASS_IN, q);
    size_t n = ask(g, q, qlen, out);
    upstream_stop(&u);
    return n;
}
static void an_a_query_does_not_refresh_a_never_used_mapping(void)
{
    struct sockaddr_in6 none = no_upstream(); gw g; gw_init(&g, &none);
    uint8_t r[1024];
    size_t n = ask_upstream(&g, 0x1234, "test.fips", "fd00::1", FIPSD_DNS_TYPE_AAAA, r);
    struct in6_addr vip = pool_address(r, n);
    CHECK(g.events.n == 1);
    fipsd_gw_instant before = fipsd_gw_pool_lookup_virtual_ip(g.pool.pool, &vip)->last_referenced;
    n = ask_upstream(&g, 0x1235, "test.fips", "fd00::1", TYPE_A, r);
    CHECK(n && rcode_of(r) == 0 && word(r, 6) == 0);
    const fipsd_gw_mapping *m = fipsd_gw_pool_lookup_virtual_ip(g.pool.pool, &vip);
    CHECKM(m, "the A query removed or replaced the mapping");
    CHECKM(m->last_referenced == before, "an A query extended a mapping that has never carried traffic");
    CHECK(g.events.n == 1);
    gw_free(&g);
}
static void an_a_query_refreshes_a_mapping_that_has_carried_traffic(void)
{
    struct sockaddr_in6 none = no_upstream(); gw g; gw_init(&g, &none);
    uint8_t r[1024];
    size_t n = ask_upstream(&g, 0x1234, "test.fips", "fd00::1", FIPSD_DNS_TYPE_AAAA, r);
    struct in6_addr vip = pool_address(r, n), mesh = gw_v6("fd00::1");
    fipsd_gw_ct_snapshot replies; fipsd_gw_ct_snapshot_init_read(&replies);
    CHECK(fipsd_gw_ct_snapshot_record_binding(&replies, &vip, &mesh, true) == 0);
    fipsd_gw_events ev = {0};
    CHECK(fipsd_gw_pool_tick(g.pool.pool, fipsd_gw_now(), &replies, &ev) == 0);
    fipsd_gw_events_clear(&ev); fipsd_gw_ct_snapshot_clear(&replies);
    const fipsd_gw_mapping *m = fipsd_gw_pool_lookup_virtual_ip(g.pool.pool, &vip);
    CHECKM(m->used, "control: the mapping carried traffic");
    fipsd_gw_instant before = m->last_referenced;
    n = ask_upstream(&g, 0x1235, "test.fips", "fd00::1", TYPE_A, r);
    CHECK(n && rcode_of(r) == 0);
    CHECKM(fipsd_gw_pool_lookup_virtual_ip(g.pool.pool, &vip)->last_referenced > before, "the A query did not refresh a mapping that carries traffic");
    CHECKM(g.events.n == 1, "the A query sent a second MappingCreated");
    gw_free(&g);
}
static void an_eviction_sends_the_removal_before_the_creation(void)
{
    struct sockaddr_in6 none = no_upstream(); gw g; fipsd_gw_pool_error e;
    gw_init_pool(&g, &none, fipsd_gw_pool_with_limits("fd01::/112", TTL, 30, 1, 10, 10, &e));
    uint8_t r[1024];
    struct in6_addr first = pool_address(r, ask_upstream(&g, 0x1234, "one.fips", "fd00::1", FIPSD_DNS_TYPE_AAAA, r));
    struct in6_addr second = pool_address(r, ask_upstream(&g, 0x1235, "two.fips", "fd00::2", FIPSD_DNS_TYPE_AAAA, r));
    struct in6_addr m1 = gw_v6("fd00::1");
    CHECK(g.events.n == 3);
    CHECKM(g.events.events[1].kind == FIPSD_GW_EVENT_MAPPING_REMOVED && fipsd_gw_ip6_eq(&g.events.events[1].virtual_ip, &first) &&
           fipsd_gw_ip6_eq(&g.events.events[1].mesh_addr, &m1), "the evicted mapping's removal first");
    CHECK(g.events.events[2].kind == FIPSD_GW_EVENT_MAPPING_CREATED && fipsd_gw_ip6_eq(&g.events.events[2].virtual_ip, &second));
    gw_free(&g);
}
static void the_in_use_hints_name_each_ports_likely_holder(void)
{
    CHECK(strstr(fipsd_gw_dns_holder_hint(5353), "mDNS") && strstr(fipsd_gw_dns_holder_hint(5353), "node.rendezvous.lan") &&
          strstr(fipsd_gw_dns_holder_hint(5353), "avahi-daemon"));
    CHECK(strstr(fipsd_gw_dns_holder_hint(5355), "LLMNR") && !strstr(fipsd_gw_dns_holder_hint(5355), "mDNS"));
    CHECK(strstr(fipsd_gw_dns_holder_hint(5354), "fips daemon's own DNS responder"));
    CHECK(strstr(fipsd_gw_dns_holder_hint(53), "another DNS server") && strstr(fipsd_gw_dns_holder_hint(53), "dnsmasq"));
    CHECK(strstr(fipsd_gw_dns_holder_hint(5365), "another fips-gateway"));
    CHECK(!strcmp(fipsd_gw_dns_holder_hint(40000), "another process holds it"));
    uint16_t p;
    CHECK(fipsd_gw_dns_port_of("[::1]:5353", &p) && p == 5353);
    CHECK(fipsd_gw_dns_port_of("gateway.lan:53", &p) && p == 53);
    CHECK(!fipsd_gw_dns_port_of("[::1]", &p) && !fipsd_gw_dns_port_of("[::1]:99999", &p));
}
static void binding_a_held_port_fails_with_addr_in_use_and_names_the_port_ss_and_netstat(void)
{
    struct sockaddr_in6 held; int holder = bound_v6_socket(&held);
    uint16_t port = ntohs(held.sin6_port);
    char listen[64], err[1024], sport[32];
    snprintf(listen, sizeof listen, "[::1]:%u", port); snprintf(sport, sizeof sport, "sport = :%u", port);
    CHECKM(fipsd_gw_dns_bind_listener(listen, err, sizeof err) < 0 && errno == EADDRINUSE, "binding a held port must fail");
    CHECK(strstr(err, listen) && strstr(err, sport) && strstr(err, "ss -ulpn") && strstr(err, "netstat -ulnp"));
    CHECK(strstr(err, fipsd_gw_dns_holder_hint(port)));
    close(holder);
}
static void binding_a_free_port_returns_a_bound_socket_with_arrival_reporting(void)
{
    char err[512];
    int fd = fipsd_gw_dns_bind_listener("[::1]:0", err, sizeof err);
    CHECKM(fd >= 0, "%s", err);
    struct sockaddr_in6 a; socklen_t len = sizeof a;
    CHECK(getsockname(fd, (struct sockaddr *)&a, &len) == 0 && a.sin6_port != 0);
    int on = 0; socklen_t olen = sizeof on;
    CHECK(getsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, &olen) == 0);
    CHECKM(on, "binding the gateway listener on IPv6 turns on arrival interface reporting");
    close(fd);
    CHECK(fipsd_gw_dns_bind_listener("nonsense", err, sizeof err) < 0 && strstr(err, "nonsense"));
}
static void zone_vectors(void)
{
    static const struct { const char *name; bool fips; } v[] = {
        {"test.fips", true}, {"npub1abc.fips", true}, {"a.b.fips", true}, {"GATEWAY.FIPS", true},
        {"fips", false}, {"example.com", false}, {"test.fips.com", false}, {"fipsx", false}};
    for (size_t i = 0; i < sizeof v / sizeof v[0]; ++i) {
        uint8_t q[512]; size_t qlen = aaaa_query(1, v[i].name, q);
        fipsd_dns_screen *s = malloc(sizeof *s);
        fipsd_dns_screen_datagram(q, qlen, CLIENT_PORT, s);
        CHECK(s->kind == FIPSD_DNS_SCREEN_QUERY);
        CHECKM(fipsd_gw_dns_under_fips(&s->query) == v[i].fips, "%s", v[i].name);
        free(s);
    }
    /* The one label `a\.fips` is not under fips. */
    uint8_t q[64] = {0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 6, 'a', '.', 'f', 'i', 'p', 's', 0, 0, 28, 0, 1};
    fipsd_dns_screen *s = malloc(sizeof *s);
    fipsd_dns_screen_datagram(q, 24, CLIENT_PORT, s);
    CHECK(s->kind == FIPSD_DNS_SCREEN_QUERY && !fipsd_gw_dns_under_fips(&s->query));
    free(s);
}
static void a_name_whose_last_label_is_not_fips_is_refused_and_not_forwarded(void)
{
    upstream u; upstream_start(&u, mesh_script, NULL);
    gw g; gw_init(&g, &u.addr);
    uint8_t q[512], r[1024]; size_t qlen = aaaa_query(0x4001, "test.fips.example", q);
    size_t n = ask(&g, q, qlen, r);
    CHECK(n && rcode_of(r) == FIPSD_DNS_RCODE_REFUSED && !memcmp(r + 12, q + 12, qlen - 12));
    upstream_stop(&u);
    CHECKM(upstream_received(&u) == 0 && g.events.n == 0, "a non-.fips name was forwarded");
    gw_free(&g);
}
static void the_two_labels_gateway_and_fips_in_any_case_are_forwarded_and_mapped(void)
{
    struct sockaddr_in6 none = no_upstream(); gw g; gw_init(&g, &none);
    uint8_t r[1024];
    size_t n = ask_upstream(&g, 0x4101, "GaTeWaY.FiPs", "fd00::7", FIPSD_DNS_TYPE_AAAA, r);
    pool_address(r, n);
    CHECK(g.events.n == 1);
    fipsd_gw_mapping_info *info; size_t count;
    CHECK(fipsd_gw_pool_mapping_info(g.pool.pool, fipsd_gw_now(), &info, &count) == 0 && count == 1);
    CHECKM(!strcmp(info[0].dns_name, "gateway.fips"), "the mapping keeps the lower-case name: %s", info[0].dns_name);
    free(info); gw_free(&g);
}
static void test_build_aaaa_response(void)
{
    struct sockaddr_in6 none = no_upstream(); gw g; gw_init(&g, &none);
    uint8_t r[1024];
    size_t n = ask_upstream(&g, 0x1234, "test.fips", "fd00::1", FIPSD_DNS_TYPE_AAAA, r);
    fipsd_gw_dns_message m;
    CHECK(fipsd_gw_dns_parse_message(r, n, &m) && m.id == 0x1234 && (m.flags & 0x8000) && m.ancount == 1 && m.qdcount == 1);
    /* The answer is the last 28 bytes: owner pointer, type, class, TTL, length, address. */
    CHECK(word(r, n - 26) == FIPSD_DNS_TYPE_AAAA && word(r, n - 24) == FIPSD_DNS_CLASS_IN && word(r, n - 18) == 16);
    uint32_t ttl = ((uint32_t)r[n - 22] << 24) | ((uint32_t)r[n - 21] << 16) | ((uint32_t)r[n - 20] << 8) | r[n - 19];
    CHECKM(ttl == TTL, "a new mapping is answered with the configured TTL (%u)", ttl);
    gw_free(&g);
}
/* --- loops and reflection --- */
static void a_response_for_a_non_fips_name_gets_no_reply(void)
{
    uint8_t q[512], r[1024]; size_t qlen = aaaa_query(0x1001, "example.com", q);
    size_t n = ask_alone(q, qlen, r);
    CHECKM(n && rcode_of(r) == FIPSD_DNS_RCODE_REFUSED, "control: REFUSED");
    qlen = query(0x1002, 0x8000, "example.com", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, q);
    CHECKM(ask_alone(q, qlen, r) == 0, "the response drew a reply");
}
static void a_text_datagram_gets_no_reply(void)
{
    uint8_t q[512], r[1024]; size_t qlen = aaaa_query(0x1101, "example.com", q);
    CHECKM(ask_alone(q, qlen, r) != 0, "control: a clean query is answered");
    const char *daytime = "Mon Nov 16 12:00:00 2026\r\n";
    CHECKM(ask_alone((const uint8_t *)daytime, strlen(daytime), r) == 0, "daytime text was answered");
}
static void a_two_question_query_for_a_non_fips_name_gets_no_reply(void)
{
    uint8_t body[512]; size_t at = 0;
    size_t lens[] = {63, 63, 63, 61};
    for (size_t i = 0; i < 4; ++i) { body[at++] = (uint8_t)lens[i]; memset(body + at, 1, lens[i]); at += lens[i]; }
    body[at++] = 0; body[at++] = 0; body[at++] = 28; body[at++] = 0; body[at++] = 1;
    uint8_t one[512] = {0x12, 0x01, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}, r[1024];
    memcpy(one + 12, body, at);
    CHECKM(ask_alone(one, 12 + at, r) && rcode_of(r) == FIPSD_DNS_RCODE_REFUSED, "control: REFUSED");
    uint8_t two[512] = {0x12, 0x02, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0};
    memcpy(two + 12, body, at);
    const uint8_t second[] = {0xC0, 0x0D, 0x00, 0x1C, 0x00, 0x01};
    memcpy(two + 12 + at, second, sizeof second);
    CHECK(12 + at + sizeof second == 277);
    CHECKM(ask_alone(two, 277, r) == 0, "the 277-byte two-question query drew a reply");
}
static void a_response_for_a_fips_name_gets_no_reply_and_mints_no_mapping(void)
{
    upstream u; upstream_start(&u, mesh_script, NULL);
    gw g; gw_init(&g, &u.addr);
    uint8_t q[512], r[1024]; size_t qlen = query(0x1301, 0x8000, "test.fips", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, q);
    CHECKM(ask(&g, q, qlen, r) == 0, "the response drew a reply");
    CHECKM(upstream_received(&u) == 0, "the response was forwarded upstream");
    CHECKM(g.events.n == 0, "the response minted a mapping");
    qlen = aaaa_query(0x1302, "test.fips", q);
    pool_address(r, ask(&g, q, qlen, r));
    CHECKM(g.events.n == 1 && g.events.events[0].kind == FIPSD_GW_EVENT_MAPPING_CREATED, "control: one MappingCreated");
    upstream_stop(&u); gw_free(&g);
}
/* --- header handling --- */
static void an_sshfp_query_for_a_non_fips_name_gets_refused(void)
{
    uint8_t q[512], r[1024]; size_t qlen = query(0x2001, 0, "example.com", TYPE_SSHFP, FIPSD_DNS_CLASS_IN, q);
    CHECK(ask_alone(q, qlen, r) && rcode_of(r) == FIPSD_DNS_RCODE_REFUSED);
}
static void an_rd_query_for_a_non_fips_name_gets_rd_and_ra_back(void)
{
    uint8_t q[512], r[1024]; size_t qlen = query(0x2101, 0x0100, "example.com", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, q);
    CHECK(ask_alone(q, qlen, r));
    CHECKM((word(r, 2) & 0x0180) == 0x0180, "flags %#06x: RD and RA not both set", word(r, 2));
}
static void an_opcode_4_query_for_a_non_fips_name_gets_a_header_only_notimp(void)
{
    uint8_t q[512], r[1024]; size_t qlen = query(0x2201, 4 << 11, "example.com", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, q);
    size_t n = ask_alone(q, qlen, r);
    CHECK(n == 12 && rcode_of(r) == FIPSD_DNS_RCODE_NOTIMP && ((word(r, 2) >> 11) & 0x0F) == 4 && word(r, 4) == 0);
}
static void a_query_with_no_question_gets_a_header_only_formerr_from_the_gateway(void)
{
    uint8_t q[12] = {0x23, 0x01}, r[1024];
    size_t n = ask_alone(q, 12, r);
    CHECK(n == 12 && rcode_of(r) == FIPSD_DNS_RCODE_FORMERR && word(r, 4) == 0);
}
static void a_class_5_query_gets_refused_from_the_gateway(void)
{
    uint8_t q[512], r[1024]; size_t qlen = query(0x2401, 0, "test.fips", FIPSD_DNS_TYPE_AAAA, 5, q);
    size_t n = ask_alone(q, qlen, r);
    CHECK(n == qlen && rcode_of(r) == FIPSD_DNS_RCODE_REFUSED && !memcmp(r + 12, q + 12, qlen - 12));
}
static void an_sshfp_query_for_a_fips_name_gets_nodata_with_an_soa_and_mints_no_mapping(void)
{
    uint8_t q[512], r[1024]; size_t qlen = query(0x2501, 0, "test.fips", TYPE_SSHFP, FIPSD_DNS_CLASS_IN, q), n; gw g;
    resolve_with(mesh_script, NULL, false, q, qlen, r, &n, &g);
    CHECK(rcode_of(r) == 0 && word(r, 6) == 0);
    char owner[256]; soa_owner(r, qlen, owner);
    CHECK(!strcmp(owner, "fips"));
    CHECKM(n == qlen + 51, "reply length %zu", n);
    CHECKM(g.events.n == 0, "an SSHFP query minted a mapping");
    gw_free(&g);
}
static void an_upstream_nxdomain_for_a_fips_is_relayed_with_an_soa_in_a_75_byte_reply(void)
{
    uint8_t q[512], r[1024]; size_t qlen = aaaa_query(0x2601, "a.fips", q), n; gw g;
    CHECK(qlen == 24);
    resolve_with(rcode_script, (void *)(uintptr_t)FIPSD_DNS_RCODE_NXDOMAIN, false, q, qlen, r, &n, &g);
    CHECK(rcode_of(r) == FIPSD_DNS_RCODE_NXDOMAIN);
    char owner[256]; soa_owner(r, qlen, owner);
    CHECK(!strcmp(owner, "fips"));
    CHECKM(n == 75, "reply length %zu", n);
    gw_free(&g);
}
static void relayed(uint8_t upstream_rcode, uint8_t *r, size_t *n)
{
    uint8_t q[512]; size_t qlen = aaaa_query(0x2611, "a.fips", q); gw g;
    CHECK(qlen == 24);
    resolve_with(rcode_script, (void *)(uintptr_t)upstream_rcode, false, q, qlen, r, n, &g);
    gw_free(&g);
}
static void an_upstream_servfail_is_relayed_as_servfail_with_no_authority_record_in_a_24_byte_reply(void)
{
    uint8_t r[1024]; size_t n; relayed(FIPSD_DNS_RCODE_SERVFAIL, r, &n);
    CHECK(rcode_of(r) == FIPSD_DNS_RCODE_SERVFAIL && word(r, 6) == 0 && word(r, 8) == 0 && n == 24);
}
static void an_upstream_rcode_of_13_is_relayed_unchanged_with_no_authority_record(void)
{
    uint8_t r[1024]; size_t n; relayed(13, r, &n);
    CHECK(rcode_of(r) == 13 && word(r, 8) == 0 && n == 24);
}
static void an_rd_aaaa_query_for_a_fips_name_gets_rd_back_with_its_pool_address(void)
{
    uint8_t q[512], r[1024]; size_t qlen = query(0x2801, 0x0100, "test.fips", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, q), n; gw g;
    resolve_with(mesh_script, NULL, false, q, qlen, r, &n, &g);
    pool_address(r, n);
    CHECKM((word(r, 2) & 0x0180) == 0x0180, "flags %#06x", word(r, 2));
    gw_free(&g);
}
static void a_z_bit_aaaa_query_for_a_fips_name_gets_its_pool_address_with_z_clear(void)
{
    uint8_t q[512], r[1024]; size_t qlen = query(0x2901, 0x0040, "test.fips", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, q), n; gw g;
    resolve_with(mesh_script, NULL, false, q, qlen, r, &n, &g);
    pool_address(r, n);
    CHECKM((word(r, 2) & 0x0040) == 0, "Z echoed");
    gw_free(&g);
}
static void an_a_query_for_a_fips_name_gets_nodata_with_an_soa_of_the_configured_ttl(void)
{
    uint8_t q[512], r[1024]; size_t qlen = query(0x3001, 0x0100, "test.fips", TYPE_A, FIPSD_DNS_CLASS_IN, q), n; gw g;
    resolve_with(mesh_script, NULL, false, q, qlen, r, &n, &g);
    CHECK(rcode_of(r) == 0 && word(r, 6) == 0 && word(r, 8) == 1);
    const uint8_t *rec = r + qlen;
    uint32_t ttl = ((uint32_t)rec[6] << 24) | ((uint32_t)rec[7] << 16) | ((uint32_t)rec[8] << 8) | rec[9];
    CHECKM(ttl == TTL, "SOA record TTL %u", ttl);
    const uint8_t *minimum = r + n - 4;
    CHECKM((((uint32_t)minimum[0] << 24) | ((uint32_t)minimum[1] << 16) | ((uint32_t)minimum[2] << 8) | minimum[3]) == TTL, "SOA minimum");
    gw_free(&g);
}
static void an_unreachable_upstream_gets_servfail(void)
{
    struct sockaddr_in6 dead; int fd = bound_v6_socket(&dead); close(fd);
    gw g; gw_init(&g, &dead); g.r.upstream_timeout_ms = 300;
    uint8_t q[512], r[1024]; size_t qlen = aaaa_query(0x3101, "test.fips", q);
    size_t n = ask(&g, q, qlen, r);
    CHECK(n && rcode_of(r) == FIPSD_DNS_RCODE_SERVFAIL && g.events.n == 0);
    gw_free(&g);
}

/* --- the startup probe --- */
static size_t probe_script(void *ctx, const uint8_t *q, size_t qlen, int k, uint8_t *out)
{ (void)ctx; return k ? 0 : upstream_answer(q, qlen, 0, "fd00::1", out); }
static void the_startup_probe_accepts_the_daemons_answer_to_its_question(void)
{
    upstream u; upstream_start(&u, probe_script, NULL);
    char err[256];
    CHECKM(fipsd_gw_dns_probe_upstream((struct sockaddr *)&u.addr, sizeof u.addr, 2000, err, sizeof err) == 0, "%s", err);
    upstream_stop(&u);
}
static void the_startup_probe_ignores_a_datagram_from_another_source(void)
{
    upstream u; upstream_start_forging(&u, forged_script, NULL, true);
    char err[256];
    CHECKM(fipsd_gw_dns_probe_upstream((struct sockaddr *)&u.addr, sizeof u.addr, 2000, err, sizeof err) == 0, "%s", err);
    upstream_stop(&u);
}
static size_t wrong_question_script(void *ctx, const uint8_t *q, size_t qlen, int k, uint8_t *out)
{ (void)ctx; (void)qlen; return k ? 0 : build_answer(word(q, 0), "other.fips", "fd00::1", out); }
static void the_startup_probe_ignores_an_upstream_datagram_that_does_not_answer_its_question(void)
{
    upstream u; upstream_start(&u, wrong_question_script, NULL);
    char err[256];
    CHECK(fipsd_gw_dns_probe_upstream((struct sockaddr *)&u.addr, sizeof u.addr, 300, err, sizeof err) != 0);
    CHECKM(strstr(err, "no response within"), "%s", err);
    upstream_stop(&u);
    upstream v; upstream_start(&v, garbage_then_answer_script, NULL);
    CHECKM(fipsd_gw_dns_probe_upstream((struct sockaddr *)&v.addr, sizeof v.addr, 2000, err, sizeof err) == 0, "garbage then the answer: %s", err);
    upstream_stop(&v);
}
static void the_startup_probe_waits_out_its_time_when_the_upstream_port_is_closed(void)
{
    struct sockaddr_in6 dead; int fd = bound_v6_socket(&dead); close(fd);
    char err[256];
    fipsd_gw_instant started = fipsd_gw_now();
    CHECK(fipsd_gw_dns_probe_upstream((struct sockaddr *)&dead, sizeof dead, 300, err, sizeof err) != 0);
    CHECKM(fipsd_gw_since(fipsd_gw_now(), started) >= MILLIS(300), "a refused port ended the wait early");
    CHECKM(strstr(err, "connection refused") || strstr(err, "no response"), "%s", err);
}

/* --- the serve loop --- */
static void an_ipv6_arrival_with_no_interface_index_is_dropped_and_an_ipv4_one_is_not(void)
{
    CHECK(fipsd_gw_dns_mesh_arrival(false, 0, true, 7, true));
    CHECK(!fipsd_gw_dns_mesh_arrival(false, 0, true, 7, false));
    CHECK(fipsd_gw_dns_mesh_arrival(true, 7, true, 7, true));
    CHECK(!fipsd_gw_dns_mesh_arrival(true, 3, true, 7, true));
    CHECK(!fipsd_gw_dns_mesh_arrival(true, 3, false, 0, true));
}
typedef struct served {
    fipsd_gw_dns_server s; pthread_t t; int rc; struct sockaddr_in6 addr;
    nsync_mu mu; nsync_cv cv; size_t seen, mesh, dropped, answered;
} served;
static void observe(void *user, fipsd_gw_dns_seen seen)
{
    served *v = user;
    nsync_mu_lock(&v->mu);
    v->seen++;
    if (seen == FIPSD_GW_DNS_SEEN_MESH) v->mesh++; else if (seen == FIPSD_GW_DNS_SEEN_DROPPED) v->dropped++; else v->answered++;
    nsync_cv_broadcast(&v->cv);
    nsync_mu_unlock(&v->mu);
}
/* Wait until the server has dealt with n datagrams in all. */
static void await_seen(served *v, size_t n)
{
    nsync_mu_lock(&v->mu);
    while (v->seen < n) nsync_cv_wait(&v->cv, &v->mu);
    nsync_mu_unlock(&v->mu);
}
static void *serve_thread(void *arg) { served *v = arg; v->rc = fipsd_gw_dns_serve(&v->s); return NULL; }
static void serve_start(served *v, gw *g, const char *listen, const char *mesh_if)
{
    memset(v, 0, sizeof *v);
    nsync_mu_init(&v->mu); nsync_cv_init(&v->cv);
    char err[512];
    int fd = fipsd_gw_dns_bind_listener(listen, err, sizeof err); CHECKM(fd >= 0, "%s", err);
    socklen_t len = sizeof v->addr; CHECK(getsockname(fd, (struct sockaddr *)&v->addr, &len) == 0);
    v->addr.sin6_addr = in6addr_loopback;
    CHECK(fipsd_gw_dns_server_init(&v->s, fd, mesh_if, &g->r) == 0);
    v->s.observe = observe; v->s.observe_user = v;
    CHECK(pthread_create(&v->t, NULL, serve_thread, v) == 0);
}
static void serve_stop(served *v)
{
    fipsd_gw_dns_server_stop(&v->s);
    /* On a thread the receive waits in poll: one datagram lets it see the stop. */
    int c = socket(AF_INET6, SOCK_DGRAM, 0);
    (void)sendto(c, "x", 1, 0, (struct sockaddr *)&v->addr, sizeof v->addr);
    close(c);
    pthread_join(v->t, NULL);
    CHECK(v->rc == 0);
    close(v->s.fd);
}
static int client_socket(void)
{
    int c = socket(AF_INET6, SOCK_DGRAM, 0); CHECK(c >= 0);
    struct sockaddr_in6 a = {.sin6_family = AF_INET6}; a.sin6_addr = in6addr_loopback;
    CHECK(bind(c, (struct sockaddr *)&a, sizeof a) == 0);
    return c;
}
static void send_to(served *v, int c, const uint8_t *d, size_t len)
{ CHECK(sendto(c, d, len, 0, (struct sockaddr *)&v->addr, sizeof v->addr) == (ssize_t)len); }
/* The reply waiting on c, or 0 when none is: the server replies before it
 * reports a datagram dealt with, and loopback delivers at once. */
static size_t pending_reply(int c, uint8_t *out)
{
    ssize_t r = recv(c, out, 1024, MSG_DONTWAIT);
    return r > 0 ? (size_t)r : 0;
}
/* Every datagram is answered or dropped before the next is read; the
 * observer says when each is done, so no reply is a reply not yet sent. */
static void serve_drops_a_datagram_that_fills_its_buffer_and_answers_the_next_query(void)
{
    struct sockaddr_in6 none = no_upstream(); gw g; gw_init(&g, &none);
    served v; serve_start(&v, &g, "[::1]:0", "fipsgw-absent0");
    uint8_t *full = calloc(1, FIPSD_DNS_MAX_DATAGRAM), r[1024], q[512];
    size_t qlen = aaaa_query(0x5001, "example.com", q);
    memcpy(full, q, qlen);
    int c = client_socket();
    send_to(&v, c, full, FIPSD_DNS_MAX_DATAGRAM);
    await_seen(&v, 1);
    CHECKM(v.dropped == 1 && pending_reply(c, r) == 0, "a datagram filling the buffer was answered");
    send_to(&v, c, q, qlen);
    await_seen(&v, 2);
    size_t n = pending_reply(c, r);
    CHECKM(n && rcode_of(r) == FIPSD_DNS_RCODE_REFUSED && word(r, 0) == 0x5001, "the next query is answered");
    close(c); free(full); serve_stop(&v); gw_free(&g);
}
static void serve_drops_a_response_and_answers_the_next_query(void)
{
    struct sockaddr_in6 none = no_upstream(); gw g; gw_init(&g, &none);
    served v; serve_start(&v, &g, "[::1]:0", "fipsgw-absent0");
    uint8_t q[512], r[1024];
    int c = client_socket();
    size_t qlen = query(0x5101, 0x8000, "example.com", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, q);
    send_to(&v, c, q, qlen);
    await_seen(&v, 1);
    CHECK(v.dropped == 1 && pending_reply(c, r) == 0);
    qlen = aaaa_query(0x5102, "example.com", q);
    send_to(&v, c, q, qlen);
    await_seen(&v, 2);
    CHECK(pending_reply(c, r) && word(r, 0) == 0x5102);
    close(c); serve_stop(&v); gw_free(&g);
}
static void serve_answers_an_opcode_4_query_with_a_header_only_notimp_and_then_the_next_query(void)
{
    struct sockaddr_in6 none = no_upstream(); gw g; gw_init(&g, &none);
    served v; serve_start(&v, &g, "[::1]:0", "fipsgw-absent0");
    uint8_t q[512], r[1024];
    int c = client_socket();
    size_t qlen = query(0x5201, 4 << 11, "example.com", FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, q);
    send_to(&v, c, q, qlen);
    await_seen(&v, 1);
    CHECK(pending_reply(c, r) == 12 && rcode_of(r) == FIPSD_DNS_RCODE_NOTIMP);
    qlen = aaaa_query(0x5202, "example.com", q);
    send_to(&v, c, q, qlen);
    await_seen(&v, 2);
    CHECK(pending_reply(c, r) && word(r, 0) == 0x5202);
    close(c); serve_stop(&v); gw_free(&g);
}
static void serve_drops_a_query_arriving_on_the_named_mesh_interface_and_never_forwards_it(void)
{
    upstream u; upstream_start(&u, mesh_script, NULL);
    gw g; gw_init(&g, &u.addr);
    /* The mesh interface is loopback here, so a query to [::1] arrives on it. */
    served v; serve_start(&v, &g, "[::1]:0", LOOPBACK);
    uint8_t q[512], r[1024]; size_t qlen = aaaa_query(0x6001, "test.fips", q);
    int c = client_socket();
    send_to(&v, c, q, qlen);
    await_seen(&v, 1);
    CHECKM(v.mesh == 1 && pending_reply(c, r) == 0, "a query arriving on the mesh interface was answered");
    close(c); serve_stop(&v); upstream_stop(&u);
    CHECKM(upstream_received(&u) == 0 && g.events.n == 0, "a mesh query was forwarded or minted a mapping");
    gw_free(&g);
}
static void a_lan_query_on_a_wildcard_listener_is_still_answered_with_a_pool_address(void)
{
    upstream u; upstream_start(&u, mesh_script, NULL);
    gw g; gw_init(&g, &u.addr);
    served v; serve_start(&v, &g, "[::]:0", "fipsgw-absent0");
    uint8_t q[512], r[1024]; size_t qlen = aaaa_query(0x6101, "test.fips", q);
    int c = client_socket();
    send_to(&v, c, q, qlen);
    await_seen(&v, 1);
    size_t n = pending_reply(c, r);
    CHECKM(v.answered == 1 && n, "a LAN query on a wildcard listener was not answered");
    pool_address(r, n);
    close(c); serve_stop(&v); upstream_stop(&u);
    CHECK(g.events.n == 1);
    gw_free(&g);
}

int gw_suite_dns(void)
{
    RUN(test_node_addr_from_mesh);
    RUN(test_node_addr_from_mesh_rejects_non_mesh);
    RUN(test_foreign_source_answer_not_accepted);
    RUN(test_upstream_id_mismatch_discarded);
    RUN(test_upstream_question_mismatch_discarded);
    RUN(test_non_mesh_aaaa_rejected);
    RUN(test_healthy_path_resolves);
    RUN(an_a_query_returns_nodata_and_mints_no_mapping);
    RUN(an_a_query_does_not_refresh_a_never_used_mapping);
    RUN(an_a_query_refreshes_a_mapping_that_has_carried_traffic);
    RUN(an_eviction_sends_the_removal_before_the_creation);
    RUN(the_in_use_hints_name_each_ports_likely_holder);
    RUN(binding_a_held_port_fails_with_addr_in_use_and_names_the_port_ss_and_netstat);
    RUN(binding_a_free_port_returns_a_bound_socket_with_arrival_reporting);
    RUN(zone_vectors);
    RUN(a_name_whose_last_label_is_not_fips_is_refused_and_not_forwarded);
    RUN(the_two_labels_gateway_and_fips_in_any_case_are_forwarded_and_mapped);
    RUN(test_build_aaaa_response);
    RUN(a_response_for_a_non_fips_name_gets_no_reply);
    RUN(a_text_datagram_gets_no_reply);
    RUN(a_two_question_query_for_a_non_fips_name_gets_no_reply);
    RUN(a_response_for_a_fips_name_gets_no_reply_and_mints_no_mapping);
    RUN(an_sshfp_query_for_a_non_fips_name_gets_refused);
    RUN(an_rd_query_for_a_non_fips_name_gets_rd_and_ra_back);
    RUN(an_opcode_4_query_for_a_non_fips_name_gets_a_header_only_notimp);
    RUN(a_query_with_no_question_gets_a_header_only_formerr_from_the_gateway);
    RUN(a_class_5_query_gets_refused_from_the_gateway);
    RUN(an_sshfp_query_for_a_fips_name_gets_nodata_with_an_soa_and_mints_no_mapping);
    RUN(an_upstream_nxdomain_for_a_fips_is_relayed_with_an_soa_in_a_75_byte_reply);
    RUN(an_upstream_servfail_is_relayed_as_servfail_with_no_authority_record_in_a_24_byte_reply);
    RUN(an_upstream_rcode_of_13_is_relayed_unchanged_with_no_authority_record);
    RUN(an_rd_aaaa_query_for_a_fips_name_gets_rd_back_with_its_pool_address);
    RUN(a_z_bit_aaaa_query_for_a_fips_name_gets_its_pool_address_with_z_clear);
    RUN(an_a_query_for_a_fips_name_gets_nodata_with_an_soa_of_the_configured_ttl);
    RUN(an_unreachable_upstream_gets_servfail);
    RUN(the_startup_probe_accepts_the_daemons_answer_to_its_question);
    RUN(the_startup_probe_ignores_a_datagram_from_another_source);
    RUN(the_startup_probe_ignores_an_upstream_datagram_that_does_not_answer_its_question);
    RUN(the_startup_probe_waits_out_its_time_when_the_upstream_port_is_closed);
    RUN(an_ipv6_arrival_with_no_interface_index_is_dropped_and_an_ipv4_one_is_not);
    RUN(serve_drops_a_datagram_that_fills_its_buffer_and_answers_the_next_query);
    RUN(serve_drops_a_response_and_answers_the_next_query);
    RUN(serve_answers_an_opcode_4_query_with_a_header_only_notimp_and_then_the_next_query);
    RUN(serve_drops_a_query_arriving_on_the_named_mesh_interface_and_never_forwards_it);
    RUN(a_lan_query_on_a_wildcard_listener_is_still_answered_with_a_pool_address);
    return 0;
}
