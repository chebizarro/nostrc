#ifndef FIPSD_PEER_DISPATCHER_H
#define FIPSD_PEER_DISPATCHER_H
#include "peer/table.h"
#include "context.h"
#include "channel.h"

typedef enum fipsd_peer_command_kind { FIPSD_PEER_COMMAND_INSERT,
    FIPSD_PEER_COMMAND_REMOVE, FIPSD_PEER_COMMAND_EVENT, FIPSD_PEER_COMMAND_REBIND } fipsd_peer_command_kind;
typedef struct fipsd_peer_command {
    fipsd_peer_command_kind kind;
    fips_node_addr node;
    fipsd_active_peer *insert;
    fipsd_peer_event event;
    fips_transport_id transport;
    fips_transport_addr addr;
} fipsd_peer_command;
typedef struct fipsd_peer_dispatcher {
    fipsd_peer_table *table;
    GoChannel *inbox;
    GoContext *context; /* runtime_context or a with_cancel child, not bare background */
    fips_clock clock;
    /* Dispatcher-local effects: execute before returning; don't retain bytes.
     * Must close link through link.h on invalidation, execute/clear outputs.
     * False aborts the dispatcher. A successful callback may update peer data,
     * but must NOT insert/remove/rebind table members while an action runs. */
    bool (*effects)(void *user, fipsd_active_peer *peer, const fipsd_peer_actions *actions);
    /* Called for each command (including refusals), before command cleanup.
     * Does not run for commands drained on cancellation. */
    void (*result)(void *user, const fipsd_peer_command *command, fipsd_peer_admission result);
    void *user;
} fipsd_peer_dispatcher;
/* Spawn from the subsystem with fipsd_runtime_spawn. Sole table/state writer.
 * inbox is bounded (caller chooses capacity); dispatcher owns each malloc'd
 * command after successful send, including insert on failure/shutdown. Commands
 * borrow no wire buffers: EVENT must not contain wire.msg2_payload. Process
 * inbound handshakes in the transport dispatcher before enqueueing metadata.
 * Producers stop on context cancellation; caller joins them AND dispatcher
 * before freeing inbox/table/context. Snapshots are safe for concurrent readers.
 * Returns false on effects/publication failure, true on cancellation/closed inbox.
 * Does not stop the runtime: subsystem wrapper reports false/shuts it down. */
bool fipsd_peer_dispatcher_run(fipsd_peer_dispatcher *dispatcher);
#endif
