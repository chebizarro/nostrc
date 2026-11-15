#ifndef FIPSD_NODE_CONTEXT_H
#define FIPSD_NODE_CONTEXT_H
#include "node/lifecycle.h"
#include "node/transport.h"
#include "node/netmon.h"
#include "node/worker_set.h"
#include <fips/identity/local.h>

typedef struct fipsd_node fipsd_node;
/* Immutable borrowed handle valid until node destruction AFTER runtime join.
 * Config/status/network/peer readers load immutable references, not mutable
 * dispatcher state. Identity access is read-only; signing/other mutation must
 * remain dispatcher-owned. No handler receives the mutable peer table. */
const fips_identity *fipsd_node_identity(const fipsd_node_context *);
const uint8_t *fipsd_node_startup_epoch(const fipsd_node_context *); /* 8 bytes */
GoRefPtr fipsd_node_config(const fipsd_node_context *);
GoRefPtr fipsd_node_network(const fipsd_node_context *);
GoRefPtr fipsd_node_peers(const fipsd_node_context *);
GoChannel *fipsd_node_peer_commands(const fipsd_node_context *);
/* Dispatcher callbacks only. Nonblocking; refusal leaves owned job with caller.
 * Counter reservation still belongs to peer/session owner, never this handle. */
int fipsd_node_dispatch_crypto(const fipsd_node_context *, fipsd_crypto_job *);

typedef struct fipsd_node_options {
    size_t workers, queue_capacity, peer_capacity; /* zero: 2, 64, 256 */
    const fipsd_netmon_source *netmon_source;
    /* Dispatcher-side extension seam for 9keoz.6. Events/jobs are borrowed
     * through callback return and then wiped/freed. No callbacks installed in
     * the bootstrap daemon; a packet/completion then fails visibly. */
    bool (*transport_event)(void *, const fipsd_transport_event *);
    bool (*crypto_result)(void *, const fipsd_crypto_job *);
    void (*network_event)(void *, const fipsd_netmon_event *);
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
