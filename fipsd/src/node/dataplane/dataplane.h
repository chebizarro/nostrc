#ifndef FIPSD_DATAPLANE_H
#define FIPSD_DATAPLANE_H
#include "node/transport.h"
#include "node/worker_set.h"
#include <fips/node/session.h>
#include <fips/proto/rekey/mod.h>
#include <fips/proto/routing/mod.h>
#include <fips/cache/coord_cache.h>
#include "swap.h"
#include <pthread.h>

#define FIPSD_DP_WIRE 2048u
#define FIPSD_DP_LANES 64u
/* Owned packet slabs: a fixed bound covering in-flight seals, opens, results
 * awaiting the dispatcher and FSP trial retries. The worker queues are sized
 * from it so the pool, not a queue, is the single back-pressure point. */
#define FIPSD_DP_PACKETS 256u
/* Slabs kept free for inbound opens (one transport-event mailbox's worth)
 * while originated sends are being admitted; see fipsd_dp_send_ready. */
#define FIPSD_DP_SEND_RESERVE 64u
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
/* The published coordinate cache (nostrc-x345n): a copy of every resident
 * entry of the core cache, expired or not (a lookup applies the expiry),
 * spread over parts by a keyed hash of the address. Parts and their groups are
 * immutable once built and reference-counted, so a section published after a
 * cache change shares every part none of whose entries changed, and forwarding
 * finds an address in O(1): hash, group, part, a scan of the part's tags.
 * Every pointer below owns or points into memory the section owns. */
#define FIPSD_DP_CACHE_FANOUT 64u /* parts per group */
typedef struct fipsd_dp_cached {
    fips_node_addr addr;
    uint64_t expires; /* cache-relative ms (the snapshot origin's) */
    const fips_cached_coord_entry *coords; /* len entries, inside the part */
    uint32_t slot; /* the core slot the row was copied from */
    uint16_t len;
} fipsd_dp_cached;
typedef struct fipsd_dp_cache_part {
    _Atomic size_t refs;
    uint32_t count;
    const uint32_t *tags; /* count hash tags, scanned before the rows */
    const fipsd_dp_cached *rows;
} fipsd_dp_cache_part;
typedef struct fipsd_dp_cache_group {
    _Atomic size_t refs;
    fipsd_dp_cache_part *parts[FIPSD_DP_CACHE_FANOUT]; /* NULL: no rows */
} fipsd_dp_cache_group;
typedef struct fipsd_dp_cache {
    _Atomic size_t refs;
    size_t count; /* rows */
    uint8_t key[16]; /* SipHash key: addresses are peer-chosen */
    size_t group_mask; /* groups - 1 */
    fipsd_dp_cache_group *groups[]; /* NULL: no rows */
} fipsd_dp_cache;
/* The publisher's side (dispatcher only, never read by packet paths): the
 * section it last built, the core generation it reflects and, per core slot,
 * where that slot's row lives, so an update visits only the slots the core
 * reports changed (fips_coord_cache_changes) and rebuilds only their parts. */
typedef struct fipsd_dp_cache_change {
    const uint8_t *addr; /* NULL: the slot's row is gone */
    const fips_cache_entry *entry;
    uint32_t slot, part, tag;
} fipsd_dp_cache_change;
typedef struct fipsd_dp_cache_view {
    fipsd_dp_cache *section; /* retained; NULL until something is resident */
    uint64_t generation, version; /* version: bumped with each new section */
    uint8_t key[16];
    size_t slots, groups, parts, changes, dirty;
    uint32_t serial; /* this update's mark */
    uint32_t *slot_part, *slot_mark; /* per slot: the part holding its row (UINT32_MAX: none), last mark */
    uint32_t *part_mark, *part_count, *part_start, *dirty_parts, *group_mark; /* per part / group */
    uint32_t *order; /* changes grouped by destination part */
    fipsd_dp_cache_change *change;
    fipsd_dp_cache_part **built;
} fipsd_dp_cache_view;
void fipsd_dp_cache_view_init(fipsd_dp_cache_view *, fips_entropy);
void fipsd_dp_cache_view_free(fipsd_dp_cache_view *);
/* Bring the view to the cache's generation. false: out of memory, the view
 * and its section unchanged. */
bool fipsd_dp_cache_view_update(fipsd_dp_cache_view *, const fips_coord_cache *);
fipsd_dp_cache *fipsd_dp_cache_retain(fipsd_dp_cache *);
void fipsd_dp_cache_release(fipsd_dp_cache *);
/* The row for addr, NULL if absent or expired at now_ms (upstream
 * CacheEntry::is_expired: now_ms > expires). section may be NULL. */
const fipsd_dp_cached *fipsd_dp_cache_find(const fipsd_dp_cache *, const fips_node_addr *, uint64_t now_ms);
typedef bool (*fipsd_dp_cache_visitor)(void *ctx, const fipsd_dp_cached *row);
void fipsd_dp_cache_walk(const fipsd_dp_cache *, fipsd_dp_cache_visitor, void *ctx);
/* Entirely owned. No pointer into mutable handler, peer, cache or Noise state. */
typedef struct fipsd_dp_snapshot {
    uint64_t fingerprint;
    fips_node_addr me;
    fips_instant origin;
    GoRefPtr my_coords;
    size_t peer_count, epoch_count;
    fipsd_dp_peer *peers;
    fipsd_dp_epoch *epochs;
    fipsd_dp_cache *cache; /* retained; shared with the snapshots around it */
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
/* Worker send gate, one per peer slot (nostrc-u19cz.3). A worker sends a
 * sealed frame only while holding mu, seeing the generation captured at submit
 * and no held frame. The dispatcher bumps the generation, under mu, before any
 * change that would make a completion send differently or not at all: an
 * active pair switch, slot release, or an epoch leaving the link's set. So no
 * worker sends on a pair or epoch after the dispatcher has moved on; such a
 * frame comes back unsent and the completion resolves it as before (9keoz.12).
 * Wire order: held counts the slot's earlier data-lane frames that the
 * dispatcher still has to send or drop (completion-sent ones from submit,
 * bounced ones from their bounce, until recycle). While any is held, later
 * frames bounce too, so they reach the wire in completion order behind them
 * rather than overtaking them (a new-epoch frame ahead of old-epoch data
 * would start the peer's drain early, nostrc-3vl7v). */
typedef struct fipsd_dp_gate {
    pthread_mutex_t mu;
    _Atomic uint64_t generation; /* written under mu */
    size_t held; /* mu */
} fipsd_dp_gate;
typedef struct fipsd_dp_packet {
    fipsd_crypto_job job;
    _Atomic bool busy;
    struct fipsd_dataplane *plane;
    GoRefPtr snapshot;
    fips_node_addr peer, carrier;
    fips_transport_id transport;
    fips_transport_addr endpoint;
    /* Worker send: transport and gate captured at submit; the worker sets
     * worker_sent (and send_rc) only if it sent. Read at completion. */
    struct fipsd_transport *via;
    fipsd_dp_gate *gate;
    uint64_t gate_generation;
    int send_rc;
    bool worker_sent, gate_held; /* gate_held: counted in gate->held until recycle */
    /* Inbound fusion (fipsd_dp_worker_preopen): a link open's session frame,
     * pre-opened on the executing thread; plaintext in input. fsp_tag is the
     * last epoch tried. Accepted only at completion (fipsd_dp_preopened). */
    bool fsp_tried, fsp_ok, fsp_k;
    uint64_t fsp_tag, fsp_counter;
    size_t fsp_body, fsp_plain, fsp_frame;
    fips_node_addr fsp_src;
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
    fipsd_dp_cache_view cache_view;
    fipsd_dp_packet *packets;
    size_t packet_count, next;
    _Atomic size_t in_flight; /* taken slabs; released by recycle on any thread */
    fips_tree_coordinate *coord_scratch;
    fips_routing_peer *route_peers;
    struct { bool used; fips_node_addr peer; uint16_t mtu; bool notify_pending; } *clamps;
    size_t clamp_capacity;
    fipsd_dp_stats stats;
    struct fipsd_node_handlers *owner; /* for change-only path MTU notices */
    fipsd_dp_gate *gates; /* per peer_rec slot */
    size_t gate_count;
    fipsd_dp_packet *preopened; /* the link completion being delivered, if pre-opened */
} fipsd_dataplane;

bool fipsd_dp_init(struct fipsd_node_handlers *);
void fipsd_dp_destroy(fipsd_dataplane *);
bool fipsd_dp_publish(struct fipsd_node_handlers *);
fipsd_dp_packet *fipsd_dp_packet_take(fipsd_dataplane *);
/* Worker shard of a peer's link (session=false) or session jobs. */
uint64_t fipsd_dp_shard(const fips_node_addr *peer, bool session);
/* Admission gate for originated sends: false while the pool lacks headroom
 * for FIPSD_DP_SEND_RESERVE inbound opens. The dispatcher leaves the command
 * mailbox unread until completions return slabs, so pressure reaches the
 * producer (a full mailbox, then a full client socket) instead of burning a
 * nonce on a seal the pool would refuse. */
bool fipsd_dp_send_ready(const fipsd_dataplane *);
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
/* Bump the slot's worker send gate (fipsd_dp_gate): call before changing the
 * link's send pair or epoch set, and on releasing the slot. */
void fipsd_dp_link_moved(struct fipsd_node_handlers *, const struct peer_rec *);
/* The jobs' after_batch step for worker-sent link frames, and the same for
 * one job (both exposed for tests). */
void fipsd_dp_worker_send_batch(fipsd_crypto_job **, size_t);
void fipsd_dp_worker_send(fipsd_crypto_job *);
/* Inbound fusion (nostrc-u19cz.3). After-step of a link open, on the executing
 * thread: an established session frame addressed to this node is opened too,
 * with the packet snapshot's session keys. fipsd_dp_preopened, on the
 * dispatcher, admits a successful one (stale-epoch check, replay-marking
 * machine step) in place of a second open job; false: not opened here (also
 * when the pre-open failed: that snapshot may predate the frame's epoch), so
 * open it as before. */
void fipsd_dp_worker_preopen(fipsd_crypto_job *);
bool fipsd_dp_preopened(struct fipsd_node_handlers *, struct peer_rec *carrier, fips_session_entry *,
    const fips_node_addr *src, const uint8_t *frame, size_t len);
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
