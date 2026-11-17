#include "node/perf_profile.h"
#ifndef FIPSD_NODE_HANDLERS_H
#define FIPSD_NODE_HANDLERS_H
#include "node/context.h"
#include "control/snapshot.h"
#include "config/config.h"
#include "peer/machine.h"
#include <fips/identity/peer.h>

/* Node protocol handlers (nostrc-9keoz.6), mirroring upstream src/node/handlers:
 * handshake, session, tree, bloom, lookup, mmp, rekey, path, probe, timeout,
 * native/host_link/netmon seams, plus the control provider projection. All
 * state is owned by the node's single dispatcher fiber (node/context.c). Every
 * inbound frame is decoded by libfips-core gates, fed to the sans-IO machine
 * with the current instant, and the returned action list is executed in order:
 * transport sends, timer deadlines, table updates, control-visible status.
 * Nothing here blocks: channels, select and the dispatcher's deadline only. */
typedef struct fipsd_node_handlers fipsd_node_handlers;
typedef struct fipsd_node_handlers_config {
    fips_clock clock;     /* now == NULL: host monotonic clock */
    fips_entropy entropy; /* fill == NULL: host libsodium entropy */
    size_t peer_capacity, tree_peer_capacity, session_capacity, cache_entries;
    size_t notice_capacity, command_capacity, pending_per_destination;
    uint64_t tick_ms, tree_announce_min_interval_ms, tree_reeval_ms, bloom_debounce_ms;
    uint64_t link_dead_timeout_ms, session_idle_timeout_ms, lookup_recent_expiry_ms;
    uint64_t lookup_attempt_timeouts_secs[8];
    size_t lookup_attempt_count;
    /* Core parameters (rekey, rate limits, cache, MMP). has_core false: derived
     * from the node configuration at attach (fipsd_config_core_params). */
    bool has_core;
    fipsd_config_core core;
    fipsd_peer_timing timing; /* pending handshake legs */
    bool path_enabled, leaf_only;
    double parent_hysteresis;
} fipsd_node_handlers_config;
void fipsd_node_handlers_config_default(fipsd_node_handlers_config *cfg);
fipsd_node_handlers *fipsd_node_handlers_create(const fipsd_node_handlers_config *cfg);
/* After the node that attached them was destroyed. */
void fipsd_node_handlers_destroy(fipsd_node_handlers *h);
/* Installs the dispatcher callbacks and user pointer into the node options. */
void fipsd_node_handlers_install(fipsd_node_handlers *h, fipsd_node_options *options);
/* Control provider: retained immutable snapshots published by the dispatcher
 * after state changes and on every tick; live commands enqueue to it. */
fipsd_control_provider fipsd_node_handlers_provider(fipsd_node_handlers *h);

/* Observable node events for hosts and tests: owned malloc'd notices on a
 * bounded channel; a full channel drops the notice and counts it. Never
 * block the dispatcher on an observer. */
typedef enum fipsd_notice_kind {
    FIPSD_NOTICE_LINK_UP, FIPSD_NOTICE_LINK_DOWN, FIPSD_NOTICE_HANDSHAKE_FAILED,
    FIPSD_NOTICE_TREE_CHANGED, FIPSD_NOTICE_FILTER_ACCEPTED, FIPSD_NOTICE_FILTER_SENT,
    FIPSD_NOTICE_SESSION_UP, FIPSD_NOTICE_SESSION_DOWN, FIPSD_NOTICE_DATAGRAM,
    FIPSD_NOTICE_LINK_REKEYED, FIPSD_NOTICE_SESSION_REKEYED, FIPSD_NOTICE_SEND_FAILED,
    FIPSD_NOTICE_LOOKUP_RESOLVED, FIPSD_NOTICE_LOOKUP_FAILED, FIPSD_NOTICE_PATH_EVENT,
    FIPSD_NOTICE_PATH_MTU /* a per-destination path MTU changed: value, 0 when released */
} fipsd_notice_kind;
typedef struct fipsd_node_notice {
    FIPSD_PERF_QUEUE_FIELD
    fipsd_notice_kind kind;
    fips_node_addr peer, root; /* root: TREE_CHANGED; LOOKUP: target */
    bool k, is_root;
    uint64_t depth, value;
    uint16_t src_port, dst_port;
    /* DATAGRAM: the peer's authenticated x-only key when the dispatcher can
     * name it (peer table, learned identities). The node address above is a
     * truncated hash and does not invert; the native API names peers by key. */
    bool has_pubkey;
    fips_xonly_pubkey pubkey;
    size_t len;
    uint8_t payload[];
} fipsd_node_notice;
GoChannel *fipsd_node_handlers_notices(fipsd_node_handlers *h);
/* Release a notice taken from that channel. The consumer owns the plaintext of
 * a FIPSD_NOTICE_DATAGRAM payload; this wipes it before freeing the storage. */
void fipsd_node_notice_free(fipsd_node_notice *n);

/* Commands from any thread or fiber: owned, nonblocking enqueue to the
 * dispatcher (-1 when full or not attached). Results arrive as notices. */
int fipsd_node_handlers_connect(fipsd_node_handlers *h, const fips_peer_identity *peer,
                                fips_transport_id transport, const fips_transport_addr *addr);
int fipsd_node_handlers_disconnect(fipsd_node_handlers *h, const fips_node_addr *peer);
/* Native seam (Tier B4 host_link/native owners): originate an application
 * datagram. Session setup, lookup and queueing happen on the dispatcher. */
int fipsd_node_handlers_send(fipsd_node_handlers *h, const fips_node_addr *dest,
                             uint16_t src_port, uint16_t dst_port, const uint8_t *payload, size_t len);
/* The same, but a full mailbox parks the caller (fiber or thread) until the
 * dispatcher takes the datagram or `cancel` (a done channel, may be NULL)
 * closes: back-pressure for owners that must not drop, such as the native
 * API's flow readers. -1 only when cancelled, closed or oversize. */
int fipsd_node_handlers_send_wait(fipsd_node_handlers *h, const fips_node_addr *dest, uint16_t src_port,
                                  uint16_t dst_port, const uint8_t *payload, size_t len, GoChannel *cancel);
/* Seed the identity cache (configured peers, rendezvous, native API). */
int fipsd_node_handlers_learn_identity(fipsd_node_handlers *h, const fips_peer_identity *peer);
/* Native datagram API sink (nostrc-9nxwk.4). With a sink installed, an
 * authenticated inbound datagram for an application port (above 1023) is
 * handed to it as an owned FIPSD_NOTICE_DATAGRAM (release it with
 * fipsd_node_notice_free) instead of the notice channel, and the sink's owner
 * counts the `native` received/drop outcomes exactly as upstream's rx_loop
 * does. Datagrams for the protocol and standard-service tiers (port 256, the
 * IPv6 shim, among them) and every other notice keep the notice channel. A
 * full sink drops the datagram and counts `drop_flow_queue_full`. Install
 * before the sink's consumer starts; NULL uninstalls (after its consumer has
 * joined). Callable from any thread. */
void fipsd_node_handlers_set_native_sink(fipsd_node_handlers *h, GoChannel *sink);
/* The node's counter registry (relaxed atomics) for owners that count off
 * the dispatcher, such as the native API's own fibers. */
struct fipsd_metrics;
struct fipsd_metrics *fipsd_node_handlers_metrics(fipsd_node_handlers *h);
/* Multiple handler sets per process are fine; this is the configured peer list
 * seam: dial each configured peer once the dispatcher is attached. */
int fipsd_node_handlers_dial_configured(fipsd_node_handlers *h);
#endif
