#ifndef FIPSD_MDNS_LAN_H
#define FIPSD_MDNS_LAN_H
/* LAN discovery candidates held between dials, sans-IO: the port of
 * upstream src/node/peering/lan.rs (nostrc-9nxwk.5).
 *
 * An mDNS advert names an npub and an address, and anyone on the link can
 * send one, so adverts are not dialled as they arrive. Each advert naming an
 * address on this node's links becomes a pending (node, address) pair; the
 * dial pacer dials pairs while keeping unconfigured LAN handshakes in flight
 * to at most FIPSD_LAN_MAX_DIALS_IN_FLIGHT, and to at most
 * FIPSD_LAN_MAX_DIALS_PER_ADDR for any one IP address. Pairs not dialled
 * carry over until dialled, connected by another path, off this node's links
 * or FIPSD_LAN_PENDING_MAX_AGE_MS old. The bounds are upstream's, which
 * calls none of them measured.
 *
 * Time (monotonic milliseconds, never wall-clock) and randomness are inputs. */
#include "mdns/advert.h"
#include "mdns/browser.h"
#include <fips/identity/peer.h>
#include <fips/transport/types.h>
#include <fips/utils/onlink.h>

#define FIPSD_LAN_MAX_PENDING 64u
#define FIPSD_LAN_MAX_ADDRS_PER_NODE 4u
#define FIPSD_LAN_PENDING_MAX_AGE_MS 600000u
#define FIPSD_LAN_MAX_DIALS_IN_FLIGHT 8u
#define FIPSD_LAN_MAX_DIALS_PER_ADDR 2u
#define FIPSD_LAN_BOUND_LOG_HOLD_MS 60000u

typedef struct fipsd_lan_pair {
    fips_peer_identity identity;  /* identity.node_addr is the node */
    fips_transport_id transport;  /* the UDP transport to dial over */
    fipsd_mdns_sockaddr addr;
    uint32_t ifaces[FIPSD_MDNS_MAX_IFACES];
    size_t iface_count;
    uint64_t first_seen_ms;       /* set by offer */
} fipsd_lan_pair;
typedef enum fipsd_lan_offer {
    FIPSD_LAN_HELD,
    FIPSD_LAN_DUPLICATE,          /* the same node and address is held */
    FIPSD_LAN_REFUSED_PER_NODE,   /* the node holds its FIPSD_LAN_MAX_ADDRS_PER_NODE */
    FIPSD_LAN_REPLACED,           /* full: took the place of a random unconfigured pair */
    FIPSD_LAN_REFUSED             /* full of configured pairs */
} fipsd_lan_offer;
/* A set of node addresses (configured, connected or connecting peers). */
typedef struct fipsd_lan_nodes { const fips_node_addr *items; size_t count; } fipsd_lan_nodes;
bool fipsd_lan_nodes_contain(fipsd_lan_nodes set, const fips_node_addr *node);

typedef struct fipsd_lan_dialed { fips_node_addr node; fips_ip_addr ip; uint64_t at_ms, at_wall_ms; } fipsd_lan_dialed;
typedef struct fipsd_lan_pending {
    fipsd_lan_pair pairs[FIPSD_LAN_MAX_PENDING];  /* arrival order */
    size_t count;
    fipsd_lan_dialed dialed[FIPSD_LAN_MAX_PENDING];
    size_t dialed_count;
    bool latched;
    uint64_t at_bound;
    bool has_below_since;
    uint64_t below_since_ms;
} fipsd_lan_pending;

/* Upstream lan_target_on_link: an IPv6 link-local address is on-link when it
 * carries a scope; any other must lie in a prefix held by one of the
 * interfaces its record arrived on. With no arrival interface it is not. */
bool fipsd_lan_target_on_link(const fipsd_mdns_sockaddr *addr, const uint32_t *ifaces, size_t n,
                              const fips_onlink_prefixes *on_link);
/* Offer one pair, stamped with now_ms. *displaced receives the pair a
 * REPLACED offer pushed out; *first_at_bound is set on the first offer to
 * find the set full since it last stayed below its bound for a minute. */
fipsd_lan_offer fipsd_lan_offer_pair(fipsd_lan_pending *s, const fipsd_lan_pair *pair, fipsd_lan_nodes configured,
                                     uint64_t now_ms, fipsd_mdns_rng rng, fipsd_lan_pair *displaced, bool *first_at_bound);
/* Drop pairs FIPSD_LAN_PENDING_MAX_AGE_MS old; true with *refused (the offers
 * that found the set full meanwhile) when the set-full latch releases. */
bool fipsd_lan_expire(fipsd_lan_pending *s, uint64_t now_ms, uint64_t *refused);
void fipsd_lan_prune_connected(fipsd_lan_pending *s, fipsd_lan_nodes connected);
/* Drop pairs no longer on this node's links; returns how many went. */
size_t fipsd_lan_retain_on_link(fipsd_lan_pending *s, const fips_onlink_prefixes *on_link);
/* The held pairs in dial order as indexes into s->pairs: configured pairs in
 * arrival order, then the rest shuffled, leaving out, for now, unconfigured
 * pairs whose IP already has FIPSD_LAN_MAX_DIALS_PER_ADDR distinct nodes in
 * flight or listed ahead of them. Returns the count written. */
size_t fipsd_lan_ordered(const fipsd_lan_pending *s, fipsd_lan_nodes configured, fipsd_mdns_rng rng,
                         size_t out[FIPSD_LAN_MAX_PENDING]);
void fipsd_lan_take(fipsd_lan_pending *s, const fips_node_addr *node, const fipsd_mdns_sockaddr *addr);
void fipsd_lan_note_dialed(fipsd_lan_pending *s, const fips_node_addr *node, const fipsd_mdns_sockaddr *addr,
                           uint64_t now_ms, uint64_t wall_ms);
/* Forget dialled nodes for which `done` says the dial is over. */
void fipsd_lan_prune_dialed(fipsd_lan_pending *s, bool (*done)(void *user, const fipsd_lan_dialed *d), void *user);
bool fipsd_lan_is_dialed(const fipsd_lan_pending *s, const fips_node_addr *node);
static inline size_t fipsd_lan_in_flight(const fipsd_lan_pending *s) { return s->dialed_count; }
/* The earliest held pair's expiry; UINT64_MAX when none is held. */
uint64_t fipsd_lan_next_expiry(const fipsd_lan_pending *s);

/* The mDNS host labels of the identities this node knows, each with its
 * owner (upstream KnownLabels). An advert whose label is a known identity's
 * under another npub is a forgery: the label carries 55 bits of the key. */
#define FIPSD_LAN_KNOWN_MAX 512u
typedef struct fipsd_lan_known_labels {
    size_t count;
    char labels[FIPSD_LAN_KNOWN_MAX][FIPSD_MDNS_LABEL_SIZE];
    fips_node_addr owners[FIPSD_LAN_KNOWN_MAX];
} fipsd_lan_known_labels;
/* npub: the identity's canonical (lower-case) npub. */
void fipsd_lan_known_labels_add(fipsd_lan_known_labels *k, const char *npub, const fips_node_addr *owner);
bool fipsd_lan_known_labels_is_impostor(const fipsd_lan_known_labels *k, const char *npub, const fips_node_addr *node);
#endif
