#ifndef FIPSD_MDNS_MDNS_H
#define FIPSD_MDNS_MDNS_H
/* The `mdns` runtime subsystem: LAN peer rendezvous over mDNS / DNS-SD
 * (RFC 6762 / RFC 6763), the port of upstream src/mdns/mod.rs and the LAN
 * half of src/node/lifecycle/mod.rs (poll_lan_rendezvous, handle_lan_events,
 * dial_pending_lan) (nostrc-9nxwk.5).
 *
 * main.c creates it when `node.rendezvous.lan.enabled` is true (default
 * false, as upstream). It publishes `_fips._udp.local.` (or
 * `node.rendezvous.lan.service_type`) for this node on every up,
 * non-loopback interface, with the bound port of the lowest-id UDP transport
 * in the SRV record, and browses for the same service type. Discovered peers
 * pass upstream's advert checks (mdns/advert.h), the on-link and known-label
 * checks and the pending-candidate bounds (mdns/lan.h), then enter the dial
 * path the Nostr rendezvous uses: fipsd_node_handlers_learn_identity and
 * fipsd_node_handlers_connect over the lowest-id bound UDP transport of the
 * address's family. The Noise IK handshake authenticates the peer. An advert
 * of a peer already linked (over any transport) is also handed to the path
 * candidate intake (fipsd_node_handlers_path_intake, upstream Feed::Mdns)
 * on the UDP transport bound to the advert's arrival interface, else the
 * lowest-id one of its family (mdns/lan.h fipsd_lan_mdns_lane), so the
 * peer's multipath Bundle gains the LAN path as a Discovery candidate.
 *
 * One engine fiber owns everything: it selects over received datagrams, the
 * node's network monitor's interface/address events (a fipsd_netmon_watch on
 * the daemon's one netmon: kernel notifications, with its backstop), the UDP
 * transports' bind
 * notifications and the stop channel, with the nearest protocol deadline as
 * the select timeout. Nothing is sampled on a timer of its own: probes,
 * announcements, delayed answers, queries, record expiry and dial retries
 * are each a deadline. Two receive fibers (IPv4 224.0.0.251:5353, IPv6
 * [ff02::fb]:5353, multicast loop on so nodes on one host see each other)
 * hand datagrams over a bounded channel; a full channel drops datagrams.
 * Stop sends goodbyes for every record, cancels the receivers, waits for
 * them and closes the sockets. */
#include "runtime.h"
#include "node/context.h"
#include "node/handlers/handlers.h"
#include "node/netmon.h"
#include "mdns/advert.h"
#include <swap.h>
#include <sys/socket.h>

typedef struct fipsd_mdns_service fipsd_mdns_service;
/* Before the runtime starts (it watches the UDP transports' binds). */
fipsd_mdns_service *fipsd_mdns_service_create(GoSwap *configs, fipsd_node *node, fipsd_node_handlers *handlers);
fipsd_subsystem *fipsd_mdns_service_subsystem(fipsd_mdns_service *s);
/* After the runtime that ran it was destroyed. */
void fipsd_mdns_service_destroy(fipsd_mdns_service *s);

/* A datagram handed from a receiver to the engine; owned, free() it. */
typedef struct fipsd_mdns_datagram {
    uint32_t ifindex;          /* arrival interface, 0 when unknown */
    fipsd_mdns_sockaddr src;
    size_t len;
    uint8_t bytes[];
} fipsd_mdns_datagram;
/* The multicast I/O seam. The default is two UDP sockets; tests install an
 * in-memory link. Every call but open runs on the engine fiber. */
typedef struct fipsd_mdns_io {
    void *user;
    /* Open the endpoints and start the receivers, which try-send owned
     * datagrams to rx. Called from the subsystem's start. 0 or -1. */
    int (*open)(void *user, fipsd_runtime *rt, GoChannel *rx);
    /* Join or leave the group on an interface: AF_INET with the interface's
     * IPv4 address v4, AF_INET6 by index. */
    bool (*join)(void *user, uint32_t ifindex, int family, const uint8_t v4[4]);
    void (*leave)(void *user, uint32_t ifindex, int family, const uint8_t v4[4]);
    /* dst NULL: the family's group on the interface. */
    bool (*send)(void *user, uint32_t ifindex, int family, const uint8_t v4[4], const fipsd_mdns_sockaddr *dst,
                 const uint8_t *buf, size_t len);
    /* Stop the receivers, wait for them and release the endpoints. */
    void (*close)(void *user);
} fipsd_mdns_io;

/* Test and embedding seams, before the runtime starts. */
void fipsd_mdns_service_set_io(fipsd_mdns_service *s, const fipsd_mdns_io *io);
typedef enum fipsd_mdns_event_kind {
    FIPSD_MDNS_EVENT_STARTED,     /* announcing began: addr.port is the advertised port */
    FIPSD_MDNS_EVENT_DISCOVERED,  /* an advert passed the browser's checks (upstream LanEvent::Discovered) */
    FIPSD_MDNS_EVENT_SKIPPED,     /* a resolved advert or address was refused; reason says why */
    FIPSD_MDNS_EVENT_DIALLED,     /* a held pair was handed to the dial path */
    FIPSD_MDNS_EVENT_REMOVED      /* a resolved service stopped resolving (goodbye or expiry) */
} fipsd_mdns_event_kind;
typedef struct fipsd_mdns_event {
    fipsd_mdns_event_kind kind;
    char npub[FIPSD_MDNS_TEXT_MAX];
    bool has_scope;
    char scope[FIPSD_MDNS_TEXT_MAX];
    fipsd_mdns_sockaddr addr;
    uint32_t ifaces[FIPSD_MDNS_MAX_IFACES];
    size_t iface_count;
    char reason[48];  /* SKIPPED: own, no-npub, cross-scope, foreign-host, zero-port,
                       * scope-less-link-local, no-udp-transport, off-link,
                       * bad-npub, known-label, per-node-limit, set-full */
    char fullname[FIPSD_MDNS_TEXT_MAX];
} fipsd_mdns_event;
/* Owned fipsd_mdns_event per event on a bounded channel; full drops it. */
void fipsd_mdns_service_observe(fipsd_mdns_service *s, GoChannel *events);
#endif
