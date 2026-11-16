#ifndef FIPSD_NODE_CONTEXT_H
#define FIPSD_NODE_CONTEXT_H
#include "node/lifecycle.h"
#include "node/transport.h"
#include "node/netmon.h"
#include "node/worker_set.h"
#include "peer/dispatcher.h"
#include <fips/identity/local.h>

typedef struct fipsd_node fipsd_node;
/* Immutable borrowed handle valid until node destruction AFTER runtime join.
 * Config/status/network/peer readers load immutable references, not mutable
 * dispatcher state. Identity access is read-only; signing/other mutation must
 * remain dispatcher-owned. Only the dispatcher's own attached handler extension
 * (fipsd_node_attach below) receives the mutable peer table; other actors send
 * commands through fipsd_node_peer_commands. */
const fips_identity *fipsd_node_identity(const fipsd_node_context *);
const fips_secp *fipsd_node_secp(const fipsd_node_context *); /* identity's own context */
const uint8_t *fipsd_node_startup_epoch(const fipsd_node_context *); /* 8 bytes */
GoRefPtr fipsd_node_config(const fipsd_node_context *);
GoRefPtr fipsd_node_network(const fipsd_node_context *);
GoRefPtr fipsd_node_peers(const fipsd_node_context *);
GoChannel *fipsd_node_peer_commands(const fipsd_node_context *);
/* Dispatcher callbacks only. Nonblocking; refusal leaves owned job with caller.
 * Counter reservation still belongs to peer/session owner, never this handle. */
int fipsd_node_dispatch_crypto(const fipsd_node_context *, fipsd_crypto_job *);

/* Handed to the attached handler extension on the dispatcher fiber, before the
 * first event. Everything here is owned by the node and outlives the handlers'
 * use; peers (table, inbox, clock) is mutated by that fiber only. */
typedef struct fipsd_node_attach {
    const fipsd_node_context *context;
    fipsd_peer_dispatcher *peers;
    const fipsd_transport_registry *registry;
    fipsd_runtime *runtime;
    GoContext *cancel; /* dispatcher child context: producers select against it */
} fipsd_node_attach;
typedef struct fipsd_node_options {
    size_t workers, queue_capacity, peer_capacity; /* zero: 2, 64, 256 */
    const fipsd_netmon_source *netmon_source;
    /* Dispatcher-side extension seam (nostrc-9keoz.6). One fiber owns the peer
     * table, the transport/netmon/crypto event sources and the peer command
     * mailbox; these callbacks run on it. Events/jobs are borrowed through
     * callback return and then wiped/freed. Without callbacks (bootstrap/tests)
     * a packet or completion fails visibly; peer commands still apply. */
    bool (*transport_event)(void *, const fipsd_transport_event *);
    bool (*crypto_result)(void *, const fipsd_crypto_job *);
    void (*network_event)(void *, const fipsd_netmon_event *);
    /* attach once before the first event (false fails dispatcher start); detach
     * once after the last event, still on the dispatcher fiber, so attached
     * links/sessions can be closed and their release outputs executed before
     * the table is destroyed. wait_ms is the handlers' nearest deadline relative
     * to now (UINT64_MAX: none); poll runs after every event/timeout and
     * services due deadlines. False from poll fails the node. */
    bool (*attach)(void *, const fipsd_node_attach *);
    void (*detach)(void *);
    uint64_t (*wait_ms)(void *, fips_instant now);
    bool (*poll)(void *, fips_instant now);
    /* Peer-machine effect executor and command result sink (peer/dispatcher.h). */
    bool (*peer_effects)(void *, fipsd_active_peer *, const fipsd_peer_actions *);
    void (*peer_result)(void *, const fipsd_peer_command *, fipsd_peer_admission);
    /* Optional owned-message mailbox multiplexed into the same select; each
     * received message is handed to command (which owns it). */
    GoChannel *commands;
    bool (*command)(void *, void *message);
    fips_clock clock; /* now == NULL: host monotonic clock */
    void *user;
} fipsd_node_options;
fipsd_node *fipsd_node_create(GoSwap *configs, const fipsd_node_options *);
fipsd_transport_registry *fipsd_node_transports(fipsd_node *); /* pre-start only */
/* Call once AFTER registration; builds the ordered lifecycle children. */
fipsd_subsystem *fipsd_node_subsystem(fipsd_node *);
GoSwap *fipsd_node_status_slot(fipsd_node *);
const fipsd_node_context *fipsd_node_get_context(const fipsd_node *);
void fipsd_node_destroy(fipsd_node *); /* runtime joined, all producers gone */
#endif
