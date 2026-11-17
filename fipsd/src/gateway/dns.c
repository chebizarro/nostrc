#ifdef __linux__
#define _GNU_SOURCE 1
#else
#define _DARWIN_C_SOURCE 1
#endif
#include "gateway/dns.h"
#include "log.h"
#include <fips/identity/address.h>
#include <fips/identity/node_addr.h>
#include <libgo/fiber.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MODULE "gateway"

/* A deadline wait_ms after now on the gateway's nanosecond clock. */
static fipsd_gw_instant deadline_after(unsigned wait_ms) { return fipsd_gw_now() + (uint64_t)wait_ms * 1000000u; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

bool fipsd_gw_dns_under_fips(const fipsd_dns_query *q)
{
    size_t pos = 0, count = 0, last_len = 0;
    const uint8_t *label, *last = NULL;
    size_t len;
    while (fipsd_dns_query_next_label(q, &pos, &label, &len)) { count++; last = label; last_len = len; }
    if (count < 2 || last_len != 4) return false;
    for (size_t i = 0; i < 4; ++i) if (lower(last[i]) != "fips"[i]) return false;
    return true;
}
bool fipsd_gw_node_addr_from_mesh(const struct in6_addr *mesh, fips_node_addr *out)
{
    /* The address is [0xfd, node prefix]; the prefix is the node address's
     * first 15 bytes, and its last byte is not in the address, so it is zero. */
    if (!fipsd_gw_ip6_is_mesh(mesh)) return false;
    fips_ipv6_address a;
    memcpy(a.bytes, mesh, 16);
    *out = fips_node_addr_from_prefix(fips_ipv6_address_prefix(&a));
    return true;
}

/* ---- parsing upstream datagrams ---------------------------------------- */
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
/* Read the name at *pos, following compression pointers, into out (wire
 * form, no pointers) when out is given; *pos moves past the name as it
 * appears in place. */
static bool read_name(const uint8_t *buf, size_t len, size_t *pos, uint8_t *out, size_t *out_len)
{
    size_t at = *pos, written = 0, hops = 0;
    bool jumped = false;
    for (;;) {
        if (at >= len) return false;
        uint8_t l = buf[at];
        if ((l & 0xC0) == 0xC0) {
            if (at + 1 >= len || ++hops > 64) return false;
            size_t target = (size_t)((l & 0x3F) << 8) | buf[at + 1];
            if (!jumped) *pos = at + 2;
            jumped = true;
            at = target;
            continue;
        }
        if (l & 0xC0) return false; /* extended label types */
        if (at + 1 + l > len || written + 1 + l > FIPSD_DNS_MAX_NAME) return false;
        if (out) memcpy(out + written, buf + at, (size_t)l + 1);
        written += (size_t)l + 1;
        at += (size_t)l + 1;
        if (!l) break;
    }
    if (!jumped) *pos = at;
    if (out_len) *out_len = written;
    return true;
}
bool fipsd_gw_dns_parse_message(const uint8_t *buf, size_t len, fipsd_gw_dns_message *m)
{
    memset(m, 0, sizeof *m);
    if (len < FIPSD_DNS_HEADER_LEN) return false;
    m->id = be16(buf); m->flags = be16(buf + 2); m->rcode = fipsd_dns_rcode_from_header(buf[3]);
    m->qdcount = be16(buf + 4); m->ancount = be16(buf + 6);
    size_t ns = be16(buf + 8), ar = be16(buf + 10), pos = FIPSD_DNS_HEADER_LEN;
    for (size_t i = 0; i < m->qdcount; ++i) {
        if (!read_name(buf, len, &pos, i ? NULL : m->qname, i ? NULL : &m->qname_len) || pos + 4 > len) return false;
        if (!i) { m->qtype = be16(buf + pos); m->qclass = be16(buf + pos + 2); }
        pos += 4;
    }
    size_t records = (size_t)m->ancount + ns + ar;
    for (size_t i = 0; i < records; ++i) {
        if (!read_name(buf, len, &pos, NULL, NULL) || pos + 10 > len) return false;
        uint16_t type = be16(buf + pos), rdlen = be16(buf + pos + 8);
        pos += 10;
        if (pos + rdlen > len) return false;
        if (i < m->ancount && type == FIPSD_DNS_TYPE_AAAA && rdlen == 16 && !m->has_aaaa) {
            m->has_aaaa = true; memcpy(&m->aaaa, buf + pos, 16);
        }
        pos += rdlen;
    }
    return true;
}
bool fipsd_gw_dns_response_matches(const fipsd_gw_dns_message *m, uint16_t id, const uint8_t *qname, size_t qname_len,
                                   uint16_t qclass)
{
    if (!(m->flags & 0x8000) || m->id != id || m->qdcount != 1) return false;
    if (m->qtype != FIPSD_DNS_TYPE_AAAA || m->qclass != qclass || m->qname_len != qname_len) return false;
    for (size_t i = 0; i < qname_len; ++i) if (lower(m->qname[i]) != lower(qname[i])) return false;
    return true;
}

/* ---- the listener -------------------------------------------------------- */
const char *fipsd_gw_dns_holder_hint(uint16_t port)
{
    switch (port) {
    case 53: return "another DNS server holds port 53: dnsmasq, systemd-resolved's stub listener, unbound or BIND";
    case 5353: return "port 5353 is mDNS: the fips daemon's LAN rendezvous (node.rendezvous.lan), "
                      "avahi-daemon or systemd-resolved's MulticastDNS may hold it";
    case 5354: return "the fips daemon's own DNS responder listens on 5354 by default; "
                      "gateway.dns.listen must not be the daemon's DNS port";
    case 5355: return "port 5355 is LLMNR, held by systemd-resolved unless LLMNR=no";
    case 5365: return "another fips-gateway may already be running";
    default: return "another process holds it";
    }
}
bool fipsd_gw_dns_port_of(const char *listen, uint16_t *port)
{
    const char *colon = listen ? strrchr(listen, ':') : NULL;
    if (!colon || !colon[1]) return false;
    unsigned long v = 0;
    const char *p = colon + 1;
    if (*p == '+') ++p;
    if (!*p) return false;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (unsigned long)(*p - '0');
        if (v > 65535) return false;
    }
    *port = (uint16_t)v;
    return true;
}
bool fipsd_gw_dns_parse_addr(const char *text, struct sockaddr_storage *out, socklen_t *len)
{
    if (!text) return false;
    const char *colon = strrchr(text, ':');
    if (!colon || !colon[1]) return false;
    char host[256];
    size_t n = (size_t)(colon - text);
    if (text[0] == '[') {
        if (n < 2 || text[n - 1] != ']') return false;
        if (n - 2 >= sizeof host) return false;
        memcpy(host, text + 1, n - 2); host[n - 2] = 0;
    } else {
        if (!n || n >= sizeof host) return false;
        memcpy(host, text, n); host[n] = 0;
        if (strchr(host, ':')) return false; /* a bare IPv6 address needs brackets */
    }
    uint16_t port;
    if (!fipsd_gw_dns_port_of(text, &port)) return false;
    char service[8]; snprintf(service, sizeof service, "%u", port);
    struct addrinfo hints = {.ai_socktype = SOCK_DGRAM, .ai_flags = AI_NUMERICSERV}, *res = NULL;
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) return false;
    memcpy(out, res->ai_addr, res->ai_addrlen); *len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    return true;
}
static int udp_socket(int family)
{
    int fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}
int fipsd_gw_dns_bind_listener(const char *listen, char *err, size_t cap)
{
    struct sockaddr_storage addr; socklen_t len;
    if (!fipsd_gw_dns_parse_addr(listen, &addr, &len)) {
        snprintf(err, cap, "cannot bind the gateway DNS listener on %s: invalid socket address syntax", listen);
        errno = EINVAL; return -1;
    }
    int fd = udp_socket(addr.ss_family), e = 0;
    if (fd < 0) e = errno;
    else if ((addr.ss_family == AF_INET6 && fipsd_dns_set_recv_pktinfo_v6(fd) != 0) || bind(fd, (struct sockaddr *)&addr, len) != 0) {
        e = errno; close(fd); fd = -1;
    }
    if (fd >= 0) return fd;
    char hint[512] = "";
    if (e == EADDRINUSE) {
        uint16_t port = 0;
        bool known = fipsd_gw_dns_port_of(listen, &port);
        char p[8]; snprintf(p, sizeof p, "%u", port);
        snprintf(hint, sizeof hint, "; %s; find the holder with `ss -ulpn 'sport = :%s'` or `netstat -ulnp`, "
                 "or set gateway.dns.listen to a free port and point the resolver that forwards .fips at it",
                 fipsd_gw_dns_holder_hint(known ? port : 0), known ? p : "<port>");
    }
    snprintf(err, cap, "cannot bind the gateway DNS listener on %s: %s%s", listen, strerror(e), hint);
    errno = e;
    return -1;
}

/* ---- datagram I/O on a fiber or a thread --------------------------------- */
/* Times out only once the deadline has passed: the remaining wait rounds up to
 * whole milliseconds, and a timer that fires early (poll may, on macOS) just
 * re-checks the deadline. */
static ssize_t recv_until(int fd, void *buf, size_t cap, fipsd_gw_instant deadline)
{
    for (;;) {
        uint64_t rest = fipsd_gw_since(deadline, fipsd_gw_now());
        uint64_t rest_ms = (rest + 999999u) / 1000000u;
        int left = rest_ms > INT_MAX ? INT_MAX : (int)rest_ms;
        if (fipsd_gw_on_fiber()) {
            ssize_t n = gof_recvfrom(fd, buf, cap, 0, NULL, NULL, left);
            if (n < 0 && !left && (errno == EAGAIN || errno == EWOULDBLOCK)) errno = ETIMEDOUT;
            if (n < 0 && (errno == EINTR || (errno == ETIMEDOUT && left))) continue;
            return n;
        }
        ssize_t n = recv(fd, buf, cap, 0);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        if (!left) { errno = ETIMEDOUT; return -1; }
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int r = poll(&p, 1, left);
        if (r < 0 && errno != EINTR) return -1;
    }
}
static ssize_t send_connected(int fd, const void *buf, size_t len)
{
    if (fipsd_gw_on_fiber()) return gof_sendto(fd, buf, len, 0, NULL, 0, 1000);
    for (;;) {
        ssize_t n = send(fd, buf, len, 0);
        if (n >= 0 || errno != EINTR) return n;
    }
}

/* The startup probe's query: test.fips AAAA, ID 0x1234, recursion desired. */
static const uint8_t PROBE_QUERY[27] = {
    0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 't', 'e', 's', 't', 0x04, 'f', 'i', 'p', 's', 0x00, 0x00, 0x1C, 0x00, 0x01};
int fipsd_gw_dns_probe_upstream(const struct sockaddr *upstream, socklen_t len, unsigned wait_ms, char *err, size_t cap)
{
    int fd = udp_socket(upstream->sa_family);
    if (fd < 0) { snprintf(err, cap, "socket failed: %s", strerror(errno)); return -1; }
    /* Connected, so the kernel drops datagrams from any other source. */
    if (connect(fd, upstream, len) != 0) { snprintf(err, cap, "connect failed: %s", strerror(errno)); close(fd); return -1; }
    if (send_connected(fd, PROBE_QUERY, sizeof PROBE_QUERY) < 0) { snprintf(err, cap, "send failed: %s", strerror(errno)); close(fd); return -1; }
    fipsd_gw_instant deadline = deadline_after(wait_ms);
    uint8_t buf[FIPSD_DNS_MAX_DATAGRAM];
    bool refused = false;
    int rc = -1;
    for (;;) {
        ssize_t n = recv_until(fd, buf, sizeof buf, deadline);
        if (n < 0 && errno == ECONNREFUSED) { refused = true; continue; }
        if (n < 0 && errno == ETIMEDOUT) {
            if (refused) snprintf(err, cap, "upstream port closed (connection refused), waited %us", wait_ms / 1000u);
            else snprintf(err, cap, "no response within %us", wait_ms / 1000u);
            break;
        }
        if (n < 0) { snprintf(err, cap, "recv failed: %s", strerror(errno)); break; }
        fipsd_gw_dns_message m;
        if (fipsd_gw_dns_parse_message(buf, (size_t)n, &m) &&
            fipsd_gw_dns_response_matches(&m, FIPSD_GW_PROBE_ID, PROBE_QUERY + 12, 11, FIPSD_DNS_CLASS_IN)) { rc = 0; break; }
        FIPSD_DEBUG(MODULE, "Discarding a datagram that does not answer the probe: len=%zd", n);
    }
    close(fd);
    return rc;
}

/* ---- resolving ----------------------------------------------------------- */
void fipsd_gw_dns_resolver_init(fipsd_gw_dns_resolver *r, const struct sockaddr *upstream, socklen_t len, uint32_t ttl,
                                fipsd_gw_shared_pool *pool, fipsd_gw_emit_fn emit, void *user)
{
    memset(r, 0, sizeof *r);
    memcpy(&r->upstream, upstream, len); r->upstream_len = len;
    r->ttl = ttl; r->pool = pool; r->emit = emit; r->emit_user = user;
    r->upstream_timeout_ms = FIPSD_GW_UPSTREAM_TIMEOUT_MS;
    nsync_mu_init(&r->fd_mu); r->upstream_fd = -1; atomic_init(&r->stopping, false);
    (void)sodium_init();
}
void fipsd_gw_dns_resolver_cancel(fipsd_gw_dns_resolver *r)
{
    nsync_mu_lock(&r->fd_mu);
    atomic_store(&r->stopping, true);
    if (r->upstream_fd >= 0) { (void)gof_io_cancel_fd(r->upstream_fd); (void)shutdown(r->upstream_fd, SHUT_RDWR); }
    nsync_mu_unlock(&r->fd_mu);
}
static size_t reply(const fipsd_dns_query *q, uint8_t rcode, const fipsd_dns_record *answers, size_t na,
                    const fipsd_dns_record *authority, size_t nauth, uint8_t *out, size_t cap)
{ return fipsd_dns_reply(q, rcode, FIPSD_DNS_RA, answers, na, authority, nauth, out, cap); }
static size_t servfail(const fipsd_dns_query *q, uint8_t *out, size_t cap) { return reply(q, FIPSD_DNS_RCODE_SERVFAIL, NULL, 0, NULL, 0, out, cap); }
/* The SOA for a negative answer (RFC 2308 Section 2.2), which tells the
 * client how long to cache it. */
static fipsd_dns_record negative_soa(const fipsd_dns_query *q, uint32_t ttl)
{
    time_t now = time(NULL);
    return fipsd_dns_soa(q, "fips", "gateway.fips", "nobody.fips", now > 0 ? (uint32_t)now : 1u, ttl);
}
/* Ask upstream for the AAAA of the query's name: 0 with the matching answer
 * in m, or -1 when no answer came. */
static int exchange(fipsd_gw_dns_resolver *r, const fipsd_dns_query *q, const char *name, fipsd_gw_dns_message *m)
{
    uint16_t id;
    randombytes_buf(&id, sizeof id);
    size_t qlen;
    const uint8_t *qname = fipsd_dns_query_qname(q, &qlen);
    uint8_t query[FIPSD_DNS_MAX_REPLY];
    size_t query_len = fipsd_dns_encode_query(id, qname, qlen, FIPSD_DNS_TYPE_AAAA, FIPSD_DNS_CLASS_IN, query, sizeof query);
    if (!query_len) return -1;
    /* Of the upstream's family, to avoid dual-stack trouble (OpenWrt often
     * has bindv6only=1). */
    int fd = udp_socket(r->upstream.ss_family);
    if (fd < 0) { FIPSD_DEBUG(MODULE, "Failed to bind upstream socket: error=%s", strerror(errno)); return -1; }
    (void)gof_io_reset_fd(fd);
    nsync_mu_lock(&r->fd_mu);
    bool stopping = atomic_load(&r->stopping);
    if (!stopping) r->upstream_fd = fd;
    nsync_mu_unlock(&r->fd_mu);
    int rc = -1;
    char text[64];
    if (stopping) goto out;
    /* Connected, so the kernel drops datagrams from any other source. */
    if (connect(fd, (struct sockaddr *)&r->upstream, r->upstream_len) != 0) {
        FIPSD_DEBUG(MODULE, "Failed to connect upstream socket: error=%s upstream=%s", strerror(errno),
                    fipsd_dns_sockaddr_text((struct sockaddr *)&r->upstream, text, sizeof text));
        goto out;
    }
    if (send_connected(fd, query, query_len) < 0) { FIPSD_DEBUG(MODULE, "Failed to forward query to daemon: error=%s", strerror(errno)); goto out; }
    /* Read until a datagram matches the query sent, or the deadline passes. */
    fipsd_gw_instant deadline = deadline_after(r->upstream_timeout_ms);
    uint8_t buf[FIPSD_DNS_MAX_DATAGRAM];
    for (;;) {
        ssize_t n = recv_until(fd, buf, sizeof buf, deadline);
        if (n < 0 && errno == ETIMEDOUT) { FIPSD_DEBUG(MODULE, "Upstream DNS timeout: upstream=%s", fipsd_dns_sockaddr_text((struct sockaddr *)&r->upstream, text, sizeof text)); break; }
        if (n < 0) { FIPSD_DEBUG(MODULE, "Upstream recv error: error=%s", strerror(errno)); break; }
        if (n == 0 && atomic_load(&r->stopping)) break;
        if (!fipsd_gw_dns_parse_message(buf, (size_t)n, m)) { FIPSD_DEBUG(MODULE, "Discarding unparseable upstream datagram: name=%s", name); continue; }
        if (fipsd_gw_dns_response_matches(m, id, qname, qlen, FIPSD_DNS_CLASS_IN)) { rc = 0; break; }
        FIPSD_DEBUG(MODULE, "Discarding unsolicited upstream datagram: name=%s", name);
    }
out:
    nsync_mu_lock(&r->fd_mu);
    r->upstream_fd = -1;
    nsync_mu_unlock(&r->fd_mu);
    (void)gof_io_reset_fd(fd);
    close(fd);
    return rc;
}
static size_t resolve_query(fipsd_gw_dns_resolver *r, const fipsd_dns_query *q, uint8_t *out, size_t cap)
{
    if (!fipsd_gw_dns_under_fips(q)) {
        FIPSD_TRACE(MODULE, "Non-.fips query, returning REFUSED: id=%u", q->id);
        return reply(q, FIPSD_DNS_RCODE_REFUSED, NULL, 0, NULL, 0, out, cap);
    }
    char name[sizeof q->name];
    size_t i = 0;
    for (; q->name[i] && i + 1 < sizeof name; ++i) name[i] = (char)lower((unsigned char)q->name[i]);
    name[i] = 0;
    FIPSD_DEBUG(MODULE, "Forwarding .fips query to daemon: name=%s id=%u", name, q->id);
    /* An AAAA query whatever the client asked: mesh addresses are IPv6. */
    fipsd_gw_dns_message m;
    if (exchange(r, q, name, &m) != 0) return servfail(q, out, cap);
    char rcode_text[8], a[48];
    if (m.rcode != FIPSD_DNS_RCODE_NOERROR) {
        /* Relayed with the client's own question. An NXDOMAIN carries the
         * same SOA as NODATA, so the client may cache it (RFC 2308 Section 5). */
        FIPSD_DEBUG(MODULE, "Upstream returned non-success: name=%s rcode=%s", name, fipsd_dns_rcode_name(m.rcode, rcode_text, sizeof rcode_text));
        if (m.rcode == FIPSD_DNS_RCODE_NXDOMAIN) {
            fipsd_dns_record soa = negative_soa(q, r->ttl);
            return reply(q, m.rcode, NULL, 0, &soa, 1, out, cap);
        }
        return reply(q, m.rcode, NULL, 0, NULL, 0, out, cap);
    }
    if (!m.has_aaaa) { FIPSD_DEBUG(MODULE, "No AAAA record in upstream response: name=%s", name); return servfail(q, out, cap); }
    fips_node_addr node;
    /* An answer outside fd00::/8 must never reach the NAT mapping path. */
    if (!fipsd_gw_node_addr_from_mesh(&m.aaaa, &node)) {
        FIPSD_DEBUG(MODULE, "Upstream AAAA is not a FIPS mesh address, rejecting: name=%s mesh_addr=%s", name, fipsd_gw_ip6_text(&m.aaaa, a, sizeof a));
        return servfail(q, out, cap);
    }
    fipsd_gw_instant now = fipsd_gw_now();
    if (q->qtype != FIPSD_DNS_TYPE_AAAA && q->qtype != FIPSD_DNS_TYPE_ANY) {
        /* Only AAAA and ANY may mint a mapping; another type refreshes a
         * mapping that has carried traffic and creates nothing. */
        nsync_mu_lock(&r->pool->mu);
        bool refreshed = fipsd_gw_pool_refresh_at(r->pool->pool, &node, now);
        nsync_mu_unlock(&r->pool->mu);
        FIPSD_DEBUG(MODULE, "Non-AAAA .fips query, returning NODATA: name=%s mesh_addr=%s refreshed=%d", name,
                    fipsd_gw_ip6_text(&m.aaaa, a, sizeof a), refreshed);
        fipsd_dns_record soa = negative_soa(q, r->ttl);
        return reply(q, FIPSD_DNS_RCODE_NOERROR, NULL, 0, &soa, 1, out, cap);
    }
    fipsd_gw_allocation allocation;
    nsync_mu_lock(&r->pool->mu);
    fipsd_gw_pool_error error = fipsd_gw_pool_allocate_at(r->pool->pool, &node, &m.aaaa, name, now, &allocation);
    size_t in_use = error ? fipsd_gw_pool_len(r->pool->pool) : 0;
    nsync_mu_unlock(&r->pool->mu);
    if (error) {
        /* Per query and driven by LAN hosts, so not a warning: the pool warns
         * once when it starts refusing at a bound. */
        char text[160];
        FIPSD_DEBUG(MODULE, "Pool allocation failed: reason=%s error=%s name=%s", fipsd_gw_pool_error_reason(error),
                    fipsd_gw_pool_error_text(error, in_use, &m.aaaa, NULL, text, sizeof text), name);
        return servfail(q, out, cap);
    }
    /* A mapping replaced at the ceiling loses its rules before the new one
     * gains its own. */
    if (allocation.evicted.present) {
        fipsd_gw_pool_event e = {FIPSD_GW_EVENT_MAPPING_REMOVED, allocation.evicted.virtual_ip, allocation.evicted.mesh_addr};
        if (r->emit(r->emit_user, &e) != 0) FIPSD_WARN(MODULE, "Failed to send pool event: the main loop has stopped");
    }
    if (allocation.is_new) {
        fipsd_gw_pool_event e = {FIPSD_GW_EVENT_MAPPING_CREATED, allocation.virtual_ip, m.aaaa};
        if (r->emit(r->emit_user, &e) != 0) FIPSD_WARN(MODULE, "Failed to send pool event: the main loop has stopped");
    }
    char v[48];
    FIPSD_DEBUG(MODULE, "Resolved .fips query: name=%s virtual_ip=%s mesh_addr=%s is_new=%d ttl=%u", name,
                fipsd_gw_ip6_text(&allocation.virtual_ip, v, sizeof v), fipsd_gw_ip6_text(&m.aaaa, a, sizeof a),
                allocation.is_new, allocation.ttl);
    fipsd_dns_record aaaa = fipsd_dns_aaaa(allocation.virtual_ip.s6_addr, allocation.ttl);
    return reply(q, FIPSD_DNS_RCODE_NOERROR, &aaaa, 1, NULL, 0, out, cap);
}
size_t fipsd_gw_dns_respond(fipsd_gw_dns_resolver *r, const uint8_t *datagram, size_t len, uint16_t src_port, uint8_t *out,
                            size_t cap, fipsd_gw_dns_outcome *outcome, fipsd_dns_drop_reason *reason, uint8_t *rcode)
{
    fipsd_dns_screen *screen = malloc(sizeof *screen);
    if (!screen) { *outcome = FIPSD_GW_DNS_DROP; if (reason) *reason = FIPSD_DNS_DROP_SHORT; return 0; }
    fipsd_dns_screen_datagram(datagram, len, src_port, screen);
    size_t n = 0;
    switch (screen->kind) {
    case FIPSD_DNS_SCREEN_DROP: *outcome = FIPSD_GW_DNS_DROP; if (reason) *reason = screen->reason; break;
    case FIPSD_DNS_SCREEN_REPLY:
        *outcome = FIPSD_GW_DNS_REFUSE; if (rcode) *rcode = screen->rcode;
        n = screen->reply_len <= cap ? screen->reply_len : 0;
        memcpy(out, screen->reply, n);
        break;
    case FIPSD_DNS_SCREEN_QUERY:
        *outcome = FIPSD_GW_DNS_ANSWER;
        n = resolve_query(r, &screen->query, out, cap);
        if (rcode && n >= 4) *rcode = fipsd_dns_rcode_from_header(out[3]);
        break;
    }
    free(screen);
    return n;
}

/* ---- serving ------------------------------------------------------------- */
bool fipsd_gw_dns_mesh_arrival(bool has_arrival, uint32_t arrival, bool has_mesh, uint32_t mesh, bool ipv6_socket)
{
    /* On Linux an IPv6 socket with IPV6_RECVPKTINFO always gets the arrival
     * index, IPv4 arrivals included, so a missing one is treated as the
     * mesh. An IPv4 socket never gets one and the mesh is IPv6-only. */
    return fipsd_dns_is_mesh_interface_query(has_arrival, arrival, has_mesh, mesh) || (ipv6_socket && !has_arrival);
}
int fipsd_gw_dns_server_init(fipsd_gw_dns_server *s, int fd, const char *mesh_if, fipsd_gw_dns_resolver *resolver)
{
    memset(s, 0, sizeof *s);
    s->fd = fd; s->resolver = resolver;
    snprintf(s->mesh_if, sizeof s->mesh_if, "%s", mesh_if ? mesh_if : "");
    struct sockaddr_storage local; socklen_t len = sizeof local;
    /* An unreadable local address counts as IPv6, the form that drops more. */
    bool readable = getsockname(fd, (struct sockaddr *)&local, &len) == 0;
    s->ipv6 = !readable || local.ss_family == AF_INET6;
    s->wildcard = readable && fipsd_dns_wildcard_bind((struct sockaddr *)&local);
    atomic_init(&s->stopping, false); atomic_init(&s->answered, 0); atomic_init(&s->dropped, 0);
    return gof_io_reset_fd(fd);
}
int fipsd_gw_dns_serve(fipsd_gw_dns_server *s)
{
    uint8_t *buf = malloc(FIPSD_DNS_MAX_DATAGRAM), *out = malloc(FIPSD_DNS_MAX_REPLY);
    int rc = 0;
    if (!buf || !out) { free(buf); free(out); errno = ENOMEM; return -1; }
    char text[64], rcode_text[8];
    while (!atomic_load(&s->stopping)) {
        struct sockaddr_storage src; socklen_t src_len = 0; fipsd_dns_arrival arrival;
        ssize_t n = fipsd_dns_recv_with_pktinfo(s->fd, buf, FIPSD_DNS_MAX_DATAGRAM, &src, &src_len, &arrival, -1);
        if (atomic_load(&s->stopping)) break;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == ECANCELED) break;
            rc = -1; break;
        }
        const struct sockaddr *from = (const struct sockaddr *)&src;
        fipsd_dns_sockaddr_text(from, text, sizeof text);
        /* By name per query: a daemon restarted under a running gateway
         * recreates its TUN with a new index. */
        uint32_t mesh = fipsd_dns_lookup_ifindex(s->mesh_if);
        if (fipsd_gw_dns_mesh_arrival(arrival.has_ifindex, arrival.ifindex, mesh != 0, mesh, s->ipv6)) {
            FIPSD_DEBUG(MODULE, "DNS query dropped: src=%s ifindex=%u reason=mesh-interface", text, arrival.has_ifindex ? arrival.ifindex : 0);
            atomic_fetch_add(&s->dropped, 1);
            if (s->observe) s->observe(s->observe_user, FIPSD_GW_DNS_SEEN_MESH);
            continue;
        }
        uint16_t port = from->sa_family == AF_INET6 ? ntohs(((const struct sockaddr_in6 *)from)->sin6_port)
                      : from->sa_family == AF_INET ? ntohs(((const struct sockaddr_in *)from)->sin_port) : 0;
        fipsd_gw_dns_outcome outcome; fipsd_dns_drop_reason reason = FIPSD_DNS_DROP_SHORT; uint8_t rcode = 0;
        size_t len = fipsd_gw_dns_respond(s->resolver, buf, (size_t)n, port, out, FIPSD_DNS_MAX_REPLY, &outcome, &reason, &rcode);
        if (outcome == FIPSD_GW_DNS_DROP || !len) {
            FIPSD_DEBUG(MODULE, "DNS datagram dropped: src=%s len=%zd reason=%s", text, n, fipsd_dns_drop_reason_str(reason));
            atomic_fetch_add(&s->dropped, 1);
            if (s->observe) s->observe(s->observe_user, FIPSD_GW_DNS_SEEN_DROPPED);
            continue;
        }
        if (outcome == FIPSD_GW_DNS_REFUSE)
            FIPSD_DEBUG(MODULE, "DNS query refused: src=%s len=%zd rcode=%s", text, n, fipsd_dns_rcode_name(rcode, rcode_text, sizeof rcode_text));
        /* A reply to a mesh source must not be drawn into the mesh by the
         * TUN's route: pinned to the arrival interface on a wildcard bind,
         * sent only when routed back out of it on a specific one. The mesh
         * index is not passed, or a specific bind would withhold every LAN
         * client holding a ULA address. */
        fipsd_dns_reply_path path = fipsd_dns_reply_path_for(from, arrival.has_ifindex, arrival.ifindex, false, 0,
                                                             s->wildcard, FIPSD_DNS_STRICT_PIN);
        const char *withheld = NULL;
        ssize_t sent = fipsd_dns_send_reply(s->fd, out, len, from, src_len, path, NULL, &withheld);
        if (sent < 0) FIPSD_DEBUG(MODULE, "Failed to send DNS response: error=%s", strerror(errno));
        else if (withheld) FIPSD_DEBUG(MODULE, "DNS reply to %s withheld: reason=%s", text, withheld);
        atomic_fetch_add(&s->answered, 1);
        if (s->observe) s->observe(s->observe_user, FIPSD_GW_DNS_SEEN_ANSWERED);
    }
    if (rc == 0) FIPSD_INFO(MODULE, "DNS resolver shutting down");
    int saved = errno;
    free(buf); free(out);
    errno = saved;
    return rc;
}
void fipsd_gw_dns_server_stop(fipsd_gw_dns_server *s)
{
    atomic_store(&s->stopping, true);
    (void)gof_io_cancel_fd(s->fd);
    if (s->resolver) fipsd_gw_dns_resolver_cancel(s->resolver);
}
