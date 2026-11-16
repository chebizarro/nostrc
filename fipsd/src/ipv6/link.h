#ifndef FIPSD_IPV6_LINK_H
#define FIPSD_IPV6_LINK_H
/* The link between the IPv6 shim and the mesh node (upstream link.rs) and
 * the shim's own copy of what the node tells it over it (upstream core.rs
 * MeshState). Everything the shim learns from the node arrives as an owned
 * message; nothing the node holds is shared with the shim's fibers. Messages
 * carry only integers, flags, byte strings and node addresses.
 *
 * Upstream names a remote end by its x-only public key and derives addresses
 * itself; the C node's datagram API is keyed by node address, so this link
 * names peers by node address (16 bytes) and derives the fd00::/8 address
 * from it. Upstream queues FromMesh messages to an adapter task; here the
 * node's end applies them in line to the mutex-protected state the TUN fibers
 * read, which is where upstream's apply ends too. */
#include "ipv6/mss.h"
#include <fips/identity/mod.h>
#include <fips/identity/node_addr.h>
#include <fips/identity/address.h>
#include <stdatomic.h>

/* The FSP port the IPv6 shim's traffic uses. */
#define FIPSD_IPV6_PORT 256u

typedef enum fipsd_ipv6_from_mesh_kind {
    FIPSD_IPV6_MESH_ATTACHED,   /* who the node is, and the payload limit it last published */
    FIPSD_IPV6_MESH_SYNCED,     /* the end of the state replayed after ATTACHED */
    FIPSD_IPV6_MESH_NODE_MTU,   /* the node's payload limit changed */
    FIPSD_IPV6_MESH_PATH_MTU,   /* the node learned (has_limit) or released a per-peer limit */
    FIPSD_IPV6_MESH_KNOWN_KEY,  /* the node can route to this peer */
    FIPSD_IPV6_MESH_FORGOT_KEY  /* the node can no longer; a key learned again since `learned` is kept */
} fipsd_ipv6_from_mesh_kind;
typedef struct fipsd_ipv6_from_mesh {
    fipsd_ipv6_from_mesh_kind kind;
    fips_node_addr peer;
    bool has_limit;
    uint16_t payload_limit;
    uint64_t learned;
} fipsd_ipv6_from_mesh;
/* A datagram from the host for the node: the IPv6 packet, header compressed. */
typedef struct fipsd_ipv6_datagram { fips_node_addr peer; const uint8_t *payload; size_t len; } fipsd_ipv6_datagram;
/* A key the DNS responder (or the host) handed out, numbered from 1 so a later
 * FORGOT_KEY can say which reports it has seen. */
typedef struct fipsd_ipv6_learned { fips_node_addr peer; uint64_t seq; } fipsd_ipv6_learned;
typedef enum fipsd_ipv6_refusal_kind {
    FIPSD_IPV6_REFUSAL_TOO_BIG,     /* larger than the node carries to peer */
    FIPSD_IPV6_REFUSAL_TABLE_FULL,  /* a session to peer would exceed the table */
    FIPSD_IPV6_REFUSAL_NO_ROUTE     /* nothing is working to reach peer */
} fipsd_ipv6_refusal_kind;
typedef struct fipsd_ipv6_refusal {
    fipsd_ipv6_refusal_kind kind;
    fips_node_addr peer;
    uint16_t payload_limit;          /* TOO_BIG */
    const uint8_t *const *payloads;  /* the refused payloads, in the order the host sent them */
    const size_t *lens;
    size_t count;
} fipsd_ipv6_refusal;
typedef enum fipsd_ipv6_applied { FIPSD_IPV6_APPLIED_ATTACHED, FIPSD_IPV6_APPLIED_SYNCED, FIPSD_IPV6_APPLIED_STATE } fipsd_ipv6_applied;

typedef struct fipsd_ipv6_known {
    bool used;
    fips_node_prefix prefix;
    fips_node_addr peer;
    uint64_t learned;  /* the last report of this key the host made, 0 if none */
    uint64_t order;    /* insertion order, for the bounded table's eviction */
} fipsd_ipv6_known;
/* The shim's copy of the node's payload limits and routable keys, shared
 * between the node's end and the TUN fibers that read it per packet. */
typedef struct fipsd_ipv6_mesh_state {
    _Atomic uint16_t node_limit, ceiling;
    fipsd_ipv6_paths paths;
    nsync_mu keys_mu;
    fipsd_ipv6_known *keys;
    size_t key_capacity, key_count;
    uint64_t next_order;
    _Atomic uint64_t learned;
} fipsd_ipv6_mesh_state;
/* State before any attach: no per-peer limits, no keys, and the payload limit
 * of the IPv6 minimum MTU, where the clamp starts before any transport binds. */
int fipsd_ipv6_mesh_state_init(fipsd_ipv6_mesh_state *state, size_t key_capacity, size_t path_capacity);
void fipsd_ipv6_mesh_state_destroy(fipsd_ipv6_mesh_state *state);
/* Apply one message. ATTACHED drops everything held from an earlier attach and
 * reports our own address in *attached when given. */
fipsd_ipv6_applied fipsd_ipv6_mesh_state_apply(fipsd_ipv6_mesh_state *state, const fipsd_ipv6_from_mesh *message,
                                               fips_ipv6_address *attached);
/* Record that the host was handed peer's address, so its packets to it are
 * forwarded from now on, and return the numbered report for the node. */
fipsd_ipv6_learned fipsd_ipv6_mesh_state_learn(fipsd_ipv6_mesh_state *state, const fips_node_addr *peer);
/* Number a report without recording it (a shim that holds no key map). */
fipsd_ipv6_learned fipsd_ipv6_mesh_state_report(fipsd_ipv6_mesh_state *state, const fips_node_addr *peer);
/* The peer a destination with address prefix `prefix` is named by. */
bool fipsd_ipv6_mesh_state_key(fipsd_ipv6_mesh_state *state, const fips_node_prefix *prefix, fips_node_addr *out);
size_t fipsd_ipv6_mesh_state_keys(fipsd_ipv6_mesh_state *state, fips_node_addr *out, size_t cap);
uint16_t fipsd_ipv6_mesh_state_node_limit(const fipsd_ipv6_mesh_state *state);
uint16_t fipsd_ipv6_mesh_state_ipv6_mtu(const fipsd_ipv6_mesh_state *state);
uint16_t fipsd_ipv6_mesh_state_ceiling(const fipsd_ipv6_mesh_state *state);
fipsd_ipv6_paths *fipsd_ipv6_mesh_state_paths(fipsd_ipv6_mesh_state *state);
/* The fd00::/8 address a peer's packets name it by: fd || node address bytes 0..15. */
fips_ipv6_address fipsd_ipv6_peer_address(const fips_node_addr *peer);
/* The node prefix a packet's destination names: address bytes 1 to 15. The
 * caller has checked the packet holds a whole header; byte 0 is not read. */
fips_node_prefix fipsd_ipv6_destination_prefix(const uint8_t *packet);
/* The node's end of the link. */
typedef struct fipsd_ipv6_host_link { fipsd_ipv6_mesh_state *state; } fipsd_ipv6_host_link;
void fipsd_ipv6_host_link_send(const fipsd_ipv6_host_link *link, const fipsd_ipv6_from_mesh *message);
#endif
