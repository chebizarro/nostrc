#ifndef FIPSD_GATEWAY_DNS_H
#define FIPSD_GATEWAY_DNS_H
/* The gateway DNS proxy, the port of upstream src/gateway/dns.rs
 * (nostrc-iq1n9.3): a forwarding proxy that takes `.fips` queries from LAN
 * hosts, forwards them to the FIPS daemon's resolver (whose resolution primes
 * the daemon's identity cache as a side effect, which fips0 routing needs),
 * and answers with a virtual IP from the pool. Every datagram is screened by
 * the daemon's codec (dns/msg.h) and every reply built by it.
 *
 * The socket calls park the fiber when called on one and use plain syscalls
 * with poll deadlines on a thread, so the tests drive the same code. */
#include "dns/msg.h"
#include "dns/resolver.h"
#include "gateway/pool.h"
#include <nsync.h>
#include <stdatomic.h>

/* Timeout for upstream DNS queries. */
#define FIPSD_GW_UPSTREAM_TIMEOUT_MS 5000u
/* The startup probe's transaction ID and name. */
#define FIPSD_GW_PROBE_ID 0x1234u
#define FIPSD_GW_PROBE_NAME "test.fips"

/* The pool, shared by the resolver and the tick under one short lock that is
 * never held across I/O. */
typedef struct fipsd_gw_shared_pool { nsync_mu mu; fipsd_gw_pool *pool; } fipsd_gw_shared_pool;

/* Whether a question name is under `fips`: two labels or more, the last
 * `fips` ignoring ASCII case, read from the wire labels. */
bool fipsd_gw_dns_under_fips(const fipsd_dns_query *query);
/* The node address of a FIPS mesh address (fd00::/8); false otherwise. */
bool fipsd_gw_node_addr_from_mesh(const struct in6_addr *mesh, fips_node_addr *out);

/* What an upstream datagram says, when it parses as a DNS message. */
typedef struct fipsd_gw_dns_message {
    uint16_t id, flags, qdcount, ancount;
    uint8_t rcode;                  /* the header's, read from byte 3 */
    uint8_t qname[FIPSD_DNS_MAX_NAME]; size_t qname_len; /* the first question, decompressed */
    uint16_t qtype, qclass;
    bool has_aaaa; struct in6_addr aaaa; /* the first AAAA answer */
} fipsd_gw_dns_message;
bool fipsd_gw_dns_parse_message(const uint8_t *buf, size_t len, fipsd_gw_dns_message *out);
/* Whether an upstream datagram answers the query sent: a response, the ID
 * sent, one question naming the name sent (ASCII case ignored), AAAA, qclass. */
bool fipsd_gw_dns_response_matches(const fipsd_gw_dns_message *m, uint16_t id, const uint8_t *qname, size_t qname_len,
                                   uint16_t qclass);

/* The service most likely to hold a DNS listen port that is in use. */
const char *fipsd_gw_dns_holder_hint(uint16_t port);
/* The port of a listen address: the digits after its last ':'; false when
 * they do not form a port. */
bool fipsd_gw_dns_port_of(const char *listen, uint16_t *port);
/* Bind the listener (IPv6: arrival-interface reporting on); the descriptor,
 * or -1 with errno and the message upstream's ListenError gives in err. */
int fipsd_gw_dns_bind_listener(const char *listen, char *err, size_t cap);
/* Resolve "host:port" or "[v6]:port" to a socket address. */
bool fipsd_gw_dns_parse_addr(const char *text, struct sockaddr_storage *out, socklen_t *len);

/* Ask upstream the probe's question once from a socket of its family and wait
 * up to wait_ms for its answer; any other datagram is discarded, and a
 * refused port waits out the time. 0, or -1 with why in err. */
int fipsd_gw_dns_probe_upstream(const struct sockaddr *upstream, socklen_t len, unsigned wait_ms, char *err, size_t cap);

/* Hands a pool event to the main loop: 0, or -1 when nobody receives any more. */
typedef int (*fipsd_gw_emit_fn)(void *user, const fipsd_gw_pool_event *event);
typedef struct fipsd_gw_dns_resolver {
    struct sockaddr_storage upstream; socklen_t upstream_len;
    uint32_t ttl;
    fipsd_gw_shared_pool *pool;
    fipsd_gw_emit_fn emit; void *emit_user;
    unsigned upstream_timeout_ms;
    nsync_mu fd_mu; int upstream_fd; /* the query in flight, under fd_mu, for cancellation */
    atomic_bool stopping;
} fipsd_gw_dns_resolver;
void fipsd_gw_dns_resolver_init(fipsd_gw_dns_resolver *r, const struct sockaddr *upstream, socklen_t len, uint32_t ttl,
                                fipsd_gw_shared_pool *pool, fipsd_gw_emit_fn emit, void *user);
/* Cancel the upstream query in flight and any later one. */
void fipsd_gw_dns_resolver_cancel(fipsd_gw_dns_resolver *r);

typedef enum fipsd_gw_dns_outcome {
    FIPSD_GW_DNS_DROP,   /* no reply */
    FIPSD_GW_DNS_REFUSE, /* the screen's error reply */
    FIPSD_GW_DNS_ANSWER  /* resolved */
} fipsd_gw_dns_outcome;
/* Screen a datagram from src_port and, if it is a query, resolve it. Returns
 * the reply length (0 for DROP); *reason or *rcode say why. */
size_t fipsd_gw_dns_respond(fipsd_gw_dns_resolver *r, const uint8_t *datagram, size_t len, uint16_t src_port, uint8_t *out,
                            size_t cap, fipsd_gw_dns_outcome *outcome, fipsd_dns_drop_reason *reason, uint8_t *rcode);

/* Whether a query is dropped as arriving from the mesh: on the mesh
 * interface, or on an IPv6 socket that did not say where it arrived. */
bool fipsd_gw_dns_mesh_arrival(bool has_arrival, uint32_t arrival, bool has_mesh, uint32_t mesh, bool ipv6_socket);

/* What the serve loop did with one datagram, after any reply was sent. */
typedef enum fipsd_gw_dns_seen { FIPSD_GW_DNS_SEEN_MESH, FIPSD_GW_DNS_SEEN_DROPPED, FIPSD_GW_DNS_SEEN_ANSWERED } fipsd_gw_dns_seen;
typedef void (*fipsd_gw_dns_observe_fn)(void *user, fipsd_gw_dns_seen seen);
typedef struct fipsd_gw_dns_server {
    int fd;
    bool ipv6, wildcard;
    char mesh_if[64];
    fipsd_gw_dns_resolver *resolver;
    atomic_bool stopping;
    atomic_ulong answered, dropped;
    fipsd_gw_dns_observe_fn observe; void *observe_user; /* optional, set before serving */
} fipsd_gw_dns_server;
/* The server over a bound listener; the mesh interface is looked up by name
 * per query, so a daemon restart that recreates its TUN is followed. */
int fipsd_gw_dns_server_init(fipsd_gw_dns_server *s, int fd, const char *mesh_if, fipsd_gw_dns_resolver *resolver);
/* Serve until stopped: 0 on shutdown, -1 with errno when the listener fails. */
int fipsd_gw_dns_serve(fipsd_gw_dns_server *s);
/* Wake the server and its upstream query; serve returns soon after. */
void fipsd_gw_dns_server_stop(fipsd_gw_dns_server *s);
#endif
