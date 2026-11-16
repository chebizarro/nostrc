#ifndef FIPSD_DNS_SERVICE_H
#define FIPSD_DNS_SERVICE_H
/* The `dns` runtime subsystem (nostrc-9nxwk.3): the `.fips` responder the
 * daemon serves, ported from the DNS child of upstream src/ipv6tun/lifecycle.rs
 * over src/ipv6tun/dns.rs. Reads `dns.bind_addr`, `dns.port` and `dns.ttl`
 * from the configuration snapshot (`dns.enabled` is main.c's decision to
 * create it), binds one UDP socket, and runs one receive fiber that screens
 * each datagram, drops queries from the mesh, answers `<npub>.fips` and
 * `<alias>.fips` with AAAA records, and hands every identity it resolves to
 * the node's dispatcher (fipsd_node_handlers_learn_identity) before the reply
 * is sent, so the identity cache is primed by the time the client connects.
 * The host map is the configured peers' aliases under the hosts file, which
 * is re-read on the next query after its modification time changes; the
 * alias base follows configuration reloads. Register after the TUN service
 * (the mesh interface is read at start) and before the control socket. A
 * bind or address failure is logged and leaves the host up, as upstream. */
#include "runtime.h"
#include "dns/resolver.h"
#include "node/handlers/handlers.h"
#include "tun/tun.h"
#include <swap.h>
#include <channel.h>

typedef struct fipsd_dns_service fipsd_dns_service;
/* tun may be NULL (TUN disabled); its state is read once at start. */
fipsd_dns_service *fipsd_dns_service_create(GoSwap *configs, fipsd_node_handlers *handlers, fipsd_tun *tun);
fipsd_subsystem *fipsd_dns_service_subsystem(fipsd_dns_service *s);
/* After the runtime that ran it was destroyed. */
void fipsd_dns_service_destroy(fipsd_dns_service *s);

/* Embedding and test seams, all set before the runtime starts. */
/* The hosts file; default FIPSD_HOSTS_DEFAULT_PATH. */
int fipsd_dns_service_set_hosts_path(fipsd_dns_service *s, const char *path);
/* Where resolved identities go; default: the node's dispatcher. Called on
 * the responder's fiber, before the reply is sent; must not block. */
typedef void (*fipsd_dns_identity_sink)(void *user, const fips_peer_identity *peer);
void fipsd_dns_service_set_identity_sink(fipsd_dns_service *s, fipsd_dns_identity_sink sink, void *user);
/* The mesh side the responder judges datagrams against, instead of reading
 * the TUN device at start. */
void fipsd_dns_service_set_mesh_side(fipsd_dns_service *s, const fipsd_dns_mesh_side *mesh);
/* Observe the fate of every datagram: an owned fipsd_dns_event per datagram
 * on a bounded channel (a full channel drops the event). NULL uninstalls. */
typedef enum fipsd_dns_event_kind {
    FIPSD_DNS_EVENT_MESH,      /* dropped unanswered before the screen read it */
    FIPSD_DNS_EVENT_DROPPED,   /* dropped by the screen (reason) */
    FIPSD_DNS_EVENT_REFUSED,   /* the screen's error reply sent (rcode) */
    FIPSD_DNS_EVENT_ANSWERED   /* answered (rcode, resolved) */
} fipsd_dns_event_kind;
typedef struct fipsd_dns_event {
    fipsd_dns_event_kind kind;
    fipsd_dns_drop_reason reason;
    uint8_t rcode;
    bool resolved;            /* an AAAA answer that resolved an identity */
    fipsd_dns_reply_kind path;
    ssize_t sent;             /* bytes sent; 0 withheld; -1 send error */
    uint16_t id;              /* the query's ID (REFUSED, ANSWERED) */
} fipsd_dns_event;
void fipsd_dns_service_observe(fipsd_dns_service *s, GoChannel *events);

/* After start: the address the socket is bound to, which a port-0
 * configuration resolves to the kernel-assigned port. -1 when not bound. */
int fipsd_dns_service_local_addr(const fipsd_dns_service *s, struct sockaddr_storage *out, socklen_t *len);
#endif
