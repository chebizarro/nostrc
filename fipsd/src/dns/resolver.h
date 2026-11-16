#ifndef FIPSD_DNS_RESOLVER_H
#define FIPSD_DNS_RESOLVER_H
/* The `.fips` resolver, the port of upstream src/ipv6tun/dns.rs
 * (nostrc-9nxwk.3): resolves `<hostname>.fips` (through the host map) and
 * `<npub>.fips` (pure computation from the public key) to node IPv6
 * addresses, judges which datagrams came from the mesh and how a reply may
 * be sent without leaking into it, and owns the socket helpers the service
 * fiber uses. Resolved identities are handed to the node for identity cache
 * priming by the service (dns/service.h). The pure functions here take no
 * locks and do no I/O; the socket functions use the libgo fiber wrappers on a
 * fiber and plain syscalls on a thread, so tests can call them directly. */
#ifdef __APPLE__
#ifndef __APPLE_USE_RFC_3542
#define __APPLE_USE_RFC_3542 1
#endif
#endif
#include "dns/hosts.h"
#include "dns/msg.h"
#include <fips/identity/peer.h>
#include <fips/utils/onlink.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/types.h>

/* ---- resolution (pure) ----------------------------------------------- */
/* The label a `.fips` question carries: the first of exactly two labels whose
 * second is `fips`, ignoring ASCII case, when it is UTF-8 without a NUL. Any
 * other name, a single label holding a dot among them, is not under `fips`. */
bool fipsd_dns_fips_label(const fipsd_dns_query *q, char *label, size_t cap);
/* Resolve the label before `.fips`: a hostname in the host map, or else an
 * npub, in any letter case. */
bool fipsd_dns_resolve_label(const char *label, const fipsd_hosts_map *hosts, const fips_secp *secp, fips_peer_identity *out);
/* Resolve a text name `<label>.fips` (optional trailing dot, case-insensitive
 * suffix); hosts may be NULL for direct npub resolution only. */
bool fipsd_dns_resolve_name(const char *name, const fipsd_hosts_map *hosts, const fips_secp *secp, fips_peer_identity *out);
/* Answer a screened query, authoritatively. A resolvable name gets its
 * address for AAAA (*resolved true, *identity set) and an empty NOERROR
 * (NODATA) for any other type, known or not: NXDOMAIN would tell the client
 * the name does not exist, causing resolvers like nslookup to give up
 * without trying AAAA. An unresolvable name, or one not under `fips`, gets
 * NXDOMAIN. Returns the reply length. */
size_t fipsd_dns_answer(const fipsd_dns_query *q, uint32_t ttl, const fipsd_hosts_map *hosts, const fips_secp *secp,
                        uint8_t *out, size_t cap, fips_peer_identity *identity, bool *resolved);
/* Screen a raw datagram from src_port and answer it: the reply length, or 0
 * when it gets no reply at all. */
size_t fipsd_dns_handle_packet(const uint8_t *datagram, size_t len, uint16_t src_port, uint32_t ttl,
                               const fipsd_hosts_map *hosts, const fips_secp *secp, uint8_t *out, size_t cap,
                               fips_peer_identity *identity, bool *resolved);

/* ---- the mesh side and the reply path (pure) -------------------------- */
/* What the responder knows of this node's mesh side: the mesh interface's
 * index, when it resolved, and the node's own mesh address while a TUN is up. */
typedef struct fipsd_dns_mesh_side {
    bool has_ifindex; uint32_t ifindex;
    bool has_addr; struct in6_addr addr;
} fipsd_dns_mesh_side;
/* Where a received datagram arrived, as far as the socket reports it
 * (IPV6_PKTINFO): the arrival interface and the destination address,
 * v4-mapped for an IPv4 arrival on a dual-stack socket. */
typedef struct fipsd_dns_arrival {
    bool has_ifindex; uint32_t ifindex;
    bool has_dst; struct in6_addr dst;
} fipsd_dns_arrival;
/* True iff the mesh interface's index is known and the packet arrived on it. */
bool fipsd_dns_is_mesh_interface_query(bool has_arrival, uint32_t arrival_ifindex, bool has_mesh, uint32_t mesh_ifindex);
/* Whether a query from src that arrived as arrival came from the mesh and
 * must be dropped unanswered: by interface where the mesh index is known;
 * otherwise, while a TUN is up, by address: addressed to the node's mesh
 * address from any other source, or, where the destination is not reported,
 * from a remote non-v4-mapped fd00::/8 source. */
bool fipsd_dns_mesh_query(const fipsd_dns_arrival *arrival, const struct sockaddr *src, const fipsd_dns_mesh_side *mesh);
typedef enum fipsd_dns_reply_kind {
    FIPSD_DNS_REPLY_ROUTED,     /* to the query's source by the routing table */
    FIPSD_DNS_REPLY_PINNED,     /* only out of the interface with this index */
    FIPSD_DNS_REPLY_WITHHELD,   /* not sent: no path is known to keep it out of the mesh */
    FIPSD_DNS_REPLY_RETURNING   /* routed, but only when the table returns it by this interface or locally */
} fipsd_dns_reply_kind;
typedef struct fipsd_dns_reply_path { fipsd_dns_reply_kind kind; uint32_t ifindex; } fipsd_dns_reply_path;
/* Whether the target holds a reply strictly to a requested outgoing interface
 * (Linux, with an unspecified source on a wildcard socket). */
#ifdef __linux__
#define FIPSD_DNS_STRICT_PIN true
#else
#define FIPSD_DNS_STRICT_PIN false
#endif
/* Choose how to send the reply to a query from src that arrived on the given
 * interface, given the mesh interface's index and whether the socket is bound
 * to the wildcard address. A reply to a mesh source address (fd00::/8, not
 * v4-mapped) is pinned, returned or withheld as upstream's table says;
 * everything else is routed. */
fipsd_dns_reply_path fipsd_dns_reply_path_for(const struct sockaddr *src, bool has_arrival, uint32_t arrival_ifindex,
                                              bool has_mesh, uint32_t mesh_ifindex, bool wildcard, bool strict_pin);
/* Whether a reply whose route the kernel gives source address `source` stays
 * out of the mesh: true when the reply goes back out of interface arrival,
 * or is delivered locally (source == destination). With the node's mesh
 * address held by no interface prefix, the destination is judged instead. */
bool fipsd_dns_returns_by(fips_ip_addr source, fips_ip_addr destination, uint32_t arrival,
                          const fips_onlink_prefixes *prefixes, const struct in6_addr *mesh_addr);
/* Whether a socket with this local address is bound to :: or 0.0.0.0; NULL
 * (unreadable) counts as specific. */
bool fipsd_dns_wildcard_bind(const struct sockaddr *local);
/* The IP of a socket address as an on-link query address (width 4 or 16). */
fips_ip_addr fipsd_dns_ip_of(const struct sockaddr *sa);

/* ---- sockets ---------------------------------------------------------- */
/* Bind the responder's UDP socket. An IPv6 bind (:: included) clears
 * IPV6_V6ONLY so IPv4-mapped clients land on the same socket whatever the
 * host's bindv6only sysctl, and enables IPV6_RECVPKTINFO so the responder
 * learns each datagram's arrival interface. Non-blocking, close-on-exec.
 * Returns the descriptor or -1 with errno. */
int fipsd_dns_bind_socket(const struct sockaddr *addr, socklen_t len);
int fipsd_dns_set_recv_pktinfo_v6(int fd);
/* An interface index by name; 0 when it does not exist. */
uint32_t fipsd_dns_lookup_ifindex(const char *name);
/* Receive one datagram with its arrival information. On a fiber this parks
 * (timeout_ms < 0: until a datagram, cancellation or scheduler stop); on a
 * thread it polls the descriptor for at most timeout_ms (< 0: forever). */
ssize_t fipsd_dns_recv_with_pktinfo(int fd, uint8_t *buf, size_t cap, struct sockaddr_storage *src, socklen_t *src_len,
                                    fipsd_dns_arrival *arrival, int timeout_ms);
/* Send buf to dst by path: routed, pinned to the arrival interface (Linux;
 * elsewhere a pinned path is treated as returning), returning (sent only when
 * the routing table takes it back out of its arrival interface or delivers it
 * locally), or withheld. Returns the bytes sent, 0 when not sent (with
 * *withheld_reason naming why), or -1 with errno. mesh_addr is the node's own
 * mesh address while a TUN is up, which a returning check uses. */
ssize_t fipsd_dns_send_reply(int fd, const uint8_t *buf, size_t len, const struct sockaddr *dst, socklen_t dst_len,
                             fipsd_dns_reply_path path, const struct in6_addr *mesh_addr, const char **withheld_reason);
#ifdef __linux__
/* Send buf to the IPv6 address dst only out of interface ifindex: one sendmsg
 * carrying an IPV6_PKTINFO with the index and an unspecified source, so the
 * kernel picks the source and fails the send (ENETUNREACH) rather than route
 * it by another interface. */
ssize_t fipsd_dns_send_pinned(int fd, const uint8_t *buf, size_t len, const struct sockaddr_in6 *dst, uint32_t ifindex);
#endif
/* The source address the kernel picks for a datagram to dst by the routing
 * table: an unbound UDP socket of dst's family is connected to it (which
 * sends nothing) and its local address read. */
bool fipsd_dns_probe_source(const struct sockaddr *dst, socklen_t len, fips_ip_addr *out);
/* Text of a socket address for logs: [v6]:port or v4:port. */
const char *fipsd_dns_sockaddr_text(const struct sockaddr *sa, char *buf, size_t cap);
#endif
