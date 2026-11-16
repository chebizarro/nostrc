#ifndef FIPSD_DATAPLANE_H
#define FIPSD_DATAPLANE_H
#include "node/transport.h"
#include "node/worker_set.h"
#include <fips/node/session.h>
#include <fips/proto/rekey/mod.h>
#include <fips/proto/routing/mod.h>
#include <fips/cache/coord_cache.h>
#include "swap.h"

#define FIPSD_DP_WIRE 2048u
#define FIPSD_DP_LANES 64u
struct fipsd_node_handlers;
struct peer_rec;
struct session_ext;
typedef struct fipsd_dp_epoch {
    fips_node_addr peer;
    bool session, k;
    fips_rekey_epoch_tag tag;
    uint32_t index;
    fips_transport_id transport;
    /* Link epochs of a multipath-enabled Bundle resolve by index from any
     * ingress transport, mirroring hs_index_lookup; transport stays provenance. */
    bool multipath;
    fips_cipher_state recv;
} fipsd_dp_epoch;
typedef struct fipsd_dp_peer {
    fips_node_addr addr;
    fips_transport_id transport;
    fips_transport_addr endpoint;
    uint16_t mtu;
    double cost;
    GoRefPtr coords, filter;
} fipsd_dp_peer;
typedef struct fipsd_dp_cached {
    fips_node_addr addr;
    fips_cached_coord coords;
    uint64_t expires;
} fipsd_dp_cached;
/* Entirely owned. No pointer into mutable handler, peer, cache or Noise state. */
typedef struct fipsd_dp_snapshot {
    uint64_t fingerprint;
    fips_node_addr me;
    fips_instant origin;
    GoRefPtr my_coords;
    size_t peer_count, epoch_count, cache_count;
    fipsd_dp_peer *peers;
    fipsd_dp_epoch *epochs;
    fipsd_dp_cached *cache;
} fipsd_dp_snapshot;
typedef struct fipsd_dp_stats {
    _Atomic uint64_t sealed[FIPSD_DP_LANES], opened[FIPSD_DP_LANES];
    uint64_t backpressure, stale, authentication_failed;
    _Atomic uint64_t replay_marks, off_dispatcher_replay_marks;
    uint64_t rejects[8], errors_sent, errors_suppressed, mtu_releases;
} fipsd_dp_stats;
typedef enum fipsd_dp_reject {
    FIPSD_DP_DECODE, FIPSD_DP_TTL, FIPSD_DP_NO_ROUTE, FIPSD_DP_MTU,
    FIPSD_DP_SEND, FIPSD_DP_UNKNOWN_INDEX, FIPSD_DP_AUTH, FIPSD_DP_REPLAY
} fipsd_dp_reject;
typedef struct fipsd_dp_packet {
    fipsd_crypto_job job;
    _Atomic bool busy;
    struct fipsd_dataplane *plane;
    GoRefPtr snapshot;
    fips_node_addr peer, carrier;
    fips_transport_id transport;
    fips_transport_addr endpoint;
    bool session, k, ce, prompt, farewell, path_message, restart; /* restart: tag is the candidate link id */
    uint64_t tag;
    uint32_t timestamp;
    size_t body, wire_size, candidate;
    uint8_t ad[FIPSD_DP_WIRE], input[FIPSD_DP_WIRE], output[FIPSD_DP_WIRE];
} fipsd_dp_packet;
typedef struct fipsd_dataplane {
    void *dispatcher_fiber; /* fixed on first dispatcher machine interaction */
    bool needs_publish; /* control mutations invalidate; packets only retain */
    GoSwap *tables;
    GoRefPtr current; /* publisher's retained generation: load once, not per packet */
    fipsd_dp_packet *packets;
    size_t packet_count, next;
    fips_tree_coordinate *coord_scratch;
    fips_routing_peer *route_peers;
    struct { bool used; fips_node_addr peer; uint16_t mtu; } *clamps;
    size_t clamp_capacity;
    fipsd_dp_stats stats;
} fipsd_dataplane;

bool fipsd_dp_init(struct fipsd_node_handlers *);
void fipsd_dp_destroy(fipsd_dataplane *);
bool fipsd_dp_publish(struct fipsd_node_handlers *);
fipsd_dp_packet *fipsd_dp_packet_take(fipsd_dataplane *);
bool fipsd_dp_open_retry(struct fipsd_node_handlers *, const fipsd_dp_packet *);
fipsd_dp_packet *fipsd_dp_acquire(struct fipsd_node_handlers *);
bool fipsd_dp_submit(struct fipsd_node_handlers *, fipsd_dp_packet *);
bool fipsd_dp_result(struct fipsd_node_handlers *, const fipsd_crypto_job *);
bool fipsd_dp_rx(struct fipsd_node_handlers *, const fipsd_transport_event *);
bool fipsd_dp_link_send_ce(struct fipsd_node_handlers *, struct peer_rec *, fips_transport_id,
    const fips_transport_addr *, const uint8_t *, size_t, bool);
bool fipsd_dp_link_send(struct fipsd_node_handlers *, struct peer_rec *, fips_transport_id,
    const fips_transport_addr *, const uint8_t *, size_t);
bool fipsd_dp_seal(struct fipsd_node_handlers *, struct peer_rec *, fips_session_entry *,
    const fips_rekey_sealed *, const uint8_t *, size_t, bool prompt,
    fips_transport_id, const fips_transport_addr *, bool ce);
bool fipsd_dp_open(struct fipsd_node_handlers *, struct peer_rec *, fips_session_entry *,
    fips_transport_id, const fips_transport_addr *, const uint8_t *, size_t);
void fipsd_dp_link_deliver(struct fipsd_node_handlers *, struct peer_rec *, fipsd_dp_packet *, uint8_t *, size_t);
/* Dispatcher switch/reseed: discard the old path clamp, seed from the active
 * pair, publish the endpoint, and only lower sessions routed through this peer. */
bool fipsd_dp_peer_rebind(struct fipsd_node_handlers *, struct peer_rec *);
void fipsd_dp_tighten_via(struct fipsd_node_handlers *, const fips_node_addr *, uint16_t);
bool fipsd_dp_route(struct fipsd_node_handlers *, const fips_session_datagram *, const fips_node_addr *, bool);
fips_status fipsd_dp_forward(const fipsd_dp_snapshot *, fips_router *, const fips_session_datagram *,
    const fips_node_addr *, bool, fips_instant, fips_tree_coordinate *, fips_routing_peer *,
    fips_writer *, fips_route_outcome *);
void fipsd_dp_reject_frame(struct fipsd_node_handlers *, fipsd_dp_reject);
void fipsd_dp_route_error(struct fipsd_node_handlers *, const fips_session_datagram *, const fips_node_addr *, uint16_t);
uint16_t fipsd_dp_link_mtu(struct fipsd_node_handlers *, const struct peer_rec *, fips_transport_id, const fips_transport_addr *);
void fipsd_dp_mtu_set(fipsd_dataplane *, const fips_node_addr *, uint16_t);
uint16_t fipsd_dp_mtu_get(const fipsd_dataplane *, const fips_node_addr *);
void fipsd_dp_mtu_release(fipsd_dataplane *, const fips_node_addr *);
#endif
