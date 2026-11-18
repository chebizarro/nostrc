/* Port of upstream src/ipv6tun/dns.rs: resolution, the mesh filter, the
 * reply path and the socket helpers. */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE 1
#include "fiber_errno.h"
#include "dns/resolver.h"
#include "transport/udp/io.h"
#include "node/netmon.h"
#include "log.h"
#include <libgo/fiber.h>
#include <blocking_executor.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MODULE "dns"

/* ---- resolution ------------------------------------------------------- */
static bool utf8_label(const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        uint8_t b = s[i]; size_t need; uint8_t lo = 0x80, hi = 0xBF;
        if (!b) return false;
        if (b < 0x80) { ++i; continue; }
        if (b >= 0xC2 && b <= 0xDF) need = 1;
        else if (b == 0xE0) { need = 2; lo = 0xA0; }
        else if ((b >= 0xE1 && b <= 0xEC) || b == 0xEE || b == 0xEF) need = 2;
        else if (b == 0xED) { need = 2; hi = 0x9F; }
        else if (b == 0xF0) { need = 3; lo = 0x90; }
        else if (b >= 0xF1 && b <= 0xF3) need = 3;
        else if (b == 0xF4) { need = 3; hi = 0x8F; }
        else return false;
        for (size_t k = 1; k <= need; ++k)
            if (i + k >= n || s[i + k] < (k == 1 ? lo : 0x80) || s[i + k] > (k == 1 ? hi : 0xBF)) return false;
        i += need + 1;
    }
    return true;
}
bool fipsd_dns_fips_label(const fipsd_dns_query *q, char *label, size_t cap)
{
    size_t pos = 0, first_len, last_len, extra_len;
    const uint8_t *first, *last, *extra;
    if (!fipsd_dns_query_next_label(q, &pos, &first, &first_len)) return false;
    if (!fipsd_dns_query_next_label(q, &pos, &last, &last_len)) return false;
    if (fipsd_dns_query_next_label(q, &pos, &extra, &extra_len)) return false;
    if (last_len != 4 || strncasecmp((const char *)last, "fips", 4)) return false;
    if (first_len >= cap || !utf8_label(first, first_len)) return false;
    memcpy(label, first, first_len); label[first_len] = 0;
    return true;
}
bool fipsd_dns_resolve_label(const char *label, const fipsd_hosts_map *hosts, const fips_secp *secp, fips_peer_identity *out)
{
    const char *mapped = hosts ? fipsd_hosts_map_lookup_npub(hosts, label) : NULL;
    const char *npub = mapped ? mapped : label;
    size_t n = strlen(npub);
    if (n >= FIPS_NIP19_CAPACITY) return false;
    /* The NIP-19 npub in any letter case (a 0x20 resolver randomises the
     * question's case), and no other spelling. */
    char lower[FIPS_NIP19_CAPACITY];
    for (size_t i = 0; i <= n; ++i) lower[i] = (char)tolower((unsigned char)npub[i]);
    return fips_peer_identity_from_npub(secp, lower, n, out) == FIPS_IDENTITY_OK;
}
bool fipsd_dns_resolve_name(const char *name, const fipsd_hosts_map *hosts, const fips_secp *secp, fips_peer_identity *out)
{
    size_t n = strlen(name);
    if (n && name[n - 1] == '.') --n;
    if (n < 5 || strncasecmp(name + n - 5, ".fips", 5)) return false;
    char label[256];
    if (n - 5 >= sizeof label) return false;
    memcpy(label, name, n - 5); label[n - 5] = 0;
    return fipsd_dns_resolve_label(label, hosts, secp, out);
}
size_t fipsd_dns_answer(const fipsd_dns_query *q, uint32_t ttl, const fipsd_hosts_map *hosts, const fips_secp *secp,
                        uint8_t *out, size_t cap, fips_peer_identity *identity, bool *resolved)
{
    char label[64]; fips_peer_identity peer;
    *resolved = false;
    if (fipsd_dns_fips_label(q, label, sizeof label) && fipsd_dns_resolve_label(label, hosts, secp, &peer)) {
        if (q->qtype == FIPSD_DNS_TYPE_AAAA) {
            fipsd_dns_record record = fipsd_dns_aaaa(peer.address.bytes, ttl);
            *identity = peer; *resolved = true;
            return fipsd_dns_reply(q, FIPSD_DNS_RCODE_NOERROR, FIPSD_DNS_AA, &record, 1, NULL, 0, out, cap);
        }
        return fipsd_dns_reply(q, FIPSD_DNS_RCODE_NOERROR, FIPSD_DNS_AA, NULL, 0, NULL, 0, out, cap);
    }
    return fipsd_dns_reply(q, FIPSD_DNS_RCODE_NXDOMAIN, FIPSD_DNS_AA, NULL, 0, NULL, 0, out, cap);
}
size_t fipsd_dns_handle_packet(const uint8_t *datagram, size_t len, uint16_t src_port, uint32_t ttl,
                               const fipsd_hosts_map *hosts, const fips_secp *secp, uint8_t *out, size_t cap,
                               fips_peer_identity *identity, bool *resolved)
{
    fipsd_dns_screen *s = malloc(sizeof *s); /* large: off the stack */
    if (!s) return 0;
    *resolved = false;
    fipsd_dns_screen_datagram(datagram, len, src_port, s);
    size_t n = 0;
    switch (s->kind) {
    case FIPSD_DNS_SCREEN_DROP: break;
    case FIPSD_DNS_SCREEN_REPLY:
        if (s->reply_len <= cap) { memcpy(out, s->reply, s->reply_len); n = s->reply_len; }
        break;
    case FIPSD_DNS_SCREEN_QUERY: n = fipsd_dns_answer(&s->query, ttl, hosts, secp, out, cap, identity, resolved); break;
    }
    free(s);
    return n;
}

/* ---- the mesh side and the reply path --------------------------------- */
static bool v4_mapped(const struct in6_addr *a)
{
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return !memcmp(a->s6_addr, prefix, sizeof prefix);
}
static bool mesh_source(const struct in6_addr *a) { return a->s6_addr[0] == FIPS_IPV6_ADDRESS_PREFIX && !v4_mapped(a); }
bool fipsd_dns_is_mesh_interface_query(bool has_arrival, uint32_t arrival_ifindex, bool has_mesh, uint32_t mesh_ifindex)
{ return has_arrival && has_mesh && arrival_ifindex == mesh_ifindex; }
bool fipsd_dns_mesh_query(const fipsd_dns_arrival *arrival, const struct sockaddr *src, const fipsd_dns_mesh_side *mesh)
{
    if (fipsd_dns_is_mesh_interface_query(arrival->has_ifindex, arrival->ifindex, mesh->has_ifindex, mesh->ifindex)) return true;
    if (mesh->has_ifindex || !mesh->has_addr) return false;
    const struct in6_addr *ip = src->sa_family == AF_INET6 ? &((const struct sockaddr_in6 *)src)->sin6_addr : NULL;
    if (ip && !memcmp(ip, &mesh->addr, sizeof *ip)) return false;
    if (arrival->has_dst) return !memcmp(&arrival->dst, &mesh->addr, sizeof mesh->addr);
    return ip && mesh_source(ip);
}
fipsd_dns_reply_path fipsd_dns_reply_path_for(const struct sockaddr *src, bool has_arrival, uint32_t arrival_ifindex,
                                              bool has_mesh, uint32_t mesh_ifindex, bool wildcard, bool strict_pin)
{
    fipsd_dns_reply_path routed = {FIPSD_DNS_REPLY_ROUTED, 0};
    if (src->sa_family != AF_INET6) return routed;
    const struct in6_addr *ip = &((const struct sockaddr_in6 *)src)->sin6_addr;
    if (!mesh_source(ip)) return routed;
    if (!has_arrival) {
        if (has_mesh && !wildcard) return (fipsd_dns_reply_path){FIPSD_DNS_REPLY_WITHHELD, 0};
        return routed;
    }
    if (!strict_pin) return (fipsd_dns_reply_path){FIPSD_DNS_REPLY_RETURNING, arrival_ifindex};
    if (has_mesh && !wildcard && arrival_ifindex != mesh_ifindex) return (fipsd_dns_reply_path){FIPSD_DNS_REPLY_WITHHELD, 0};
    if (!has_mesh && !wildcard) return (fipsd_dns_reply_path){FIPSD_DNS_REPLY_RETURNING, arrival_ifindex};
    return (fipsd_dns_reply_path){FIPSD_DNS_REPLY_PINNED, arrival_ifindex};
}
static bool same_ip(fips_ip_addr a, fips_ip_addr b)
{ return a.width == b.width && !memcmp(a.bytes, b.bytes, a.width == 4 ? 4 : 16); }
bool fipsd_dns_returns_by(fips_ip_addr source, fips_ip_addr destination, uint32_t arrival,
                          const fips_onlink_prefixes *prefixes, const struct in6_addr *mesh_addr)
{
    if (same_ip(source, destination)) return true;
    if (mesh_addr) {
        fips_ip_addr mesh = {.width = 16};
        memcpy(mesh.bytes, mesh_addr->s6_addr, 16);
        if (!fips_onlink_contains(prefixes, mesh)) return fips_onlink_contains_on(prefixes, destination, &arrival, 1);
    }
    return fips_onlink_contains_on(prefixes, source, &arrival, 1);
}
bool fipsd_dns_wildcard_bind(const struct sockaddr *local)
{
    if (!local) return false;
    if (local->sa_family == AF_INET6) return IN6_IS_ADDR_UNSPECIFIED(&((const struct sockaddr_in6 *)local)->sin6_addr);
    if (local->sa_family == AF_INET) return ((const struct sockaddr_in *)local)->sin_addr.s_addr == 0;
    return false;
}
fips_ip_addr fipsd_dns_ip_of(const struct sockaddr *sa)
{
    fips_ip_addr ip = {.width = 16};
    if (sa->sa_family == AF_INET6) memcpy(ip.bytes, ((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr, 16);
    else if (sa->sa_family == AF_INET) { ip.width = 4; memcpy(ip.bytes, &((const struct sockaddr_in *)sa)->sin_addr, 4); }
    return ip;
}
const char *fipsd_dns_sockaddr_text(const struct sockaddr *sa, char *buf, size_t cap)
{
    char host[INET6_ADDRSTRLEN] = "?";
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)sa;
        inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof host);
        snprintf(buf, cap, "[%s]:%u", host, (unsigned)ntohs(a->sin6_port));
    } else if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &a->sin_addr, host, sizeof host);
        snprintf(buf, cap, "%s:%u", host, (unsigned)ntohs(a->sin_port));
    } else snprintf(buf, cap, "?");
    return buf;
}

/* ---- sockets ---------------------------------------------------------- */
int fipsd_dns_set_recv_pktinfo_v6(int fd)
{
    int one = 1;
    return setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof one);
}
int fipsd_dns_bind_socket(const struct sockaddr *addr, socklen_t len)
{
    int fd = socket(addr->sa_family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    int zero = 0;
    if ((addr->sa_family == AF_INET6 &&
         (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero) != 0 || fipsd_dns_set_recv_pktinfo_v6(fd) != 0)) ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || bind(fd, addr, len) != 0) {
        int e = errno; close(fd); errno = e; return -1;
    }
    return fd;
}
uint32_t fipsd_dns_lookup_ifindex(const char *name) { return name && *name ? if_nametoindex(name) : 0; }

/* recvmsg/sendmsg that park the fiber, or poll the descriptor on a thread. */
static ssize_t dns_recvmsg(int fd, struct msghdr *msg, int timeout_ms)
{
    if (gof_in_fiber()) return gof_recvmsg(fd, msg, 0, timeout_ms);
    for (;;) {
        ssize_t n = recvmsg(fd, msg, 0);
        if (n >= 0) return n;
        if (errno == EINTR) continue; /* errno-after-park: allow -- thread-only fallback; fiber branch cannot reach this read */
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1; /* errno-after-park: allow -- thread-only fallback; fiber branch cannot reach this read */
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int r = poll(&p, 1, timeout_ms);
        if (r == 0) { errno = ETIMEDOUT; return -1; }
        if (r < 0 && errno != EINTR) return -1; /* errno-after-park: allow -- thread-only fallback; fiber branch cannot reach this read */
    }
}
static ssize_t dns_sendmsg(int fd, const struct msghdr *msg)
{
    if (gof_in_fiber()) { ssize_t n; do n = gof_sendmsg(fd, msg, 0, -1); while (n < 0 && fipsd_io_errno() == EINTR); return n; }
    for (;;) {
        ssize_t n = sendmsg(fd, msg, 0);
        if (n >= 0) return n;
        if (errno == EINTR) continue; /* errno-after-park: allow -- thread-only fallback; fiber branch cannot reach this read */
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1; /* errno-after-park: allow -- thread-only fallback; fiber branch cannot reach this read */
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        if (poll(&p, 1, 1000) == 0) { errno = ETIMEDOUT; return -1; }
    }
}
ssize_t fipsd_dns_recv_with_pktinfo(int fd, uint8_t *buf, size_t cap, struct sockaddr_storage *src, socklen_t *src_len,
                                    fipsd_dns_arrival *arrival, int timeout_ms)
{
    struct iovec iov = {.iov_base = buf, .iov_len = cap};
    alignas(struct cmsghdr) uint8_t control[256];
    memset(src, 0, sizeof *src);
    struct msghdr msg = {.msg_name = src, .msg_namelen = sizeof *src, .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = control, .msg_controllen = sizeof control};
    ssize_t n = dns_recvmsg(fd, &msg, timeout_ms);
    if (n < 0) return -1;
    *src_len = msg.msg_namelen;
    *arrival = (fipsd_dns_arrival){0};
    fipsd_udp_packet_info info;
    if (fipsd_udp_decode_control(&msg, &info) && info.has_destination && info.family == AF_INET6) {
        arrival->has_ifindex = true; arrival->ifindex = info.interface_index;
        arrival->has_dst = true; arrival->dst = info.destination.v6;
    }
    return n;
}
static ssize_t send_routed(int fd, const uint8_t *buf, size_t len, const struct sockaddr *dst, socklen_t dst_len)
{
    struct iovec iov = {.iov_base = (void *)buf, .iov_len = len};
    struct msghdr msg = {.msg_name = (void *)dst, .msg_namelen = dst_len, .msg_iov = &iov, .msg_iovlen = 1};
    return dns_sendmsg(fd, &msg);
}
#ifdef __linux__
ssize_t fipsd_dns_send_pinned(int fd, const uint8_t *buf, size_t len, const struct sockaddr_in6 *dst, uint32_t ifindex)
{
    struct iovec iov = {.iov_base = (void *)buf, .iov_len = len};
    alignas(struct cmsghdr) uint8_t control[CMSG_SPACE(sizeof(struct in6_pktinfo))];
    memset(control, 0, sizeof control);
    struct msghdr msg = {.msg_name = (void *)dst, .msg_namelen = sizeof *dst, .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = control, .msg_controllen = sizeof control};
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = IPPROTO_IPV6; c->cmsg_type = IPV6_PKTINFO; c->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
    struct in6_pktinfo info = {.ipi6_ifindex = ifindex}; /* unspecified source: the kernel picks it */
    memcpy(CMSG_DATA(c), &info, sizeof info);
    return dns_sendmsg(fd, &msg);
}
#endif
bool fipsd_dns_probe_source(const struct sockaddr *dst, socklen_t len, fips_ip_addr *out)
{
    int fd = socket(dst->sa_family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;
    struct sockaddr_storage local; socklen_t local_len = sizeof local;
    bool ok = connect(fd, dst, len) == 0 && getsockname(fd, (struct sockaddr *)&local, &local_len) == 0;
    close(fd);
    if (ok) *out = fipsd_dns_ip_of((struct sockaddr *)&local);
    return ok;
}
static void *read_system(void *arg) { (void)arg; return fipsd_onlink_read_system(); }
/* Whether a returning reply to dst, whose query arrived on interface arrival,
 * may be sent; NULL when it may, else the reason it is withheld. */
static const char *check_returning(const struct sockaddr *dst, socklen_t dst_len, uint32_t arrival, const struct in6_addr *mesh_addr)
{
    fips_ip_addr source;
    if (!fipsd_dns_probe_source(dst, dst_len, &source)) return "reply-route-probe-failed";
    fipsd_network_snapshot *net = gof_in_fiber() ? go_blocking_submit(read_system, NULL) : fipsd_onlink_read_system();
    if (!net) return "interface-read-failed";
    const char *reason = NULL;
    if (net->failed || fips_onlink_read_failed(&net->onlink)) reason = "interface-read-failed";
    else if (!fipsd_dns_returns_by(source, fipsd_dns_ip_of(dst), arrival, &net->onlink, mesh_addr))
        reason = "reply-route-leaves-by-another-interface";
    free(net);
    return reason;
}
static ssize_t send_returning(int fd, const uint8_t *buf, size_t len, const struct sockaddr *dst, socklen_t dst_len,
                              uint32_t arrival, const struct in6_addr *mesh_addr, const char **withheld)
{
    const char *reason = check_returning(dst, dst_len, arrival, mesh_addr);
    if (!reason) return send_routed(fd, buf, len, dst, dst_len);
    *withheld = reason;
    return 0;
}
ssize_t fipsd_dns_send_reply(int fd, const uint8_t *buf, size_t len, const struct sockaddr *dst, socklen_t dst_len,
                             fipsd_dns_reply_path path, const struct in6_addr *mesh_addr, const char **withheld_reason)
{
    *withheld_reason = NULL;
    switch (path.kind) {
    case FIPSD_DNS_REPLY_ROUTED: return send_routed(fd, buf, len, dst, dst_len);
    case FIPSD_DNS_REPLY_RETURNING: return send_returning(fd, buf, len, dst, dst_len, path.ifindex, mesh_addr, withheld_reason);
    case FIPSD_DNS_REPLY_PINNED:
#ifdef __linux__
        if (dst->sa_family != AF_INET6) { errno = EINVAL; return -1; }
        return fipsd_dns_send_pinned(fd, buf, len, (const struct sockaddr_in6 *)dst, path.ifindex);
#else
        return send_returning(fd, buf, len, dst, dst_len, path.ifindex, mesh_addr, withheld_reason);
#endif
    case FIPSD_DNS_REPLY_WITHHELD: break;
    }
    *withheld_reason = "mesh-source-off-mesh-interface";
    return 0;
}
