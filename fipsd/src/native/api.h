#ifndef FIPSD_NATIVE_API_H
#define FIPSD_NATIVE_API_H
/* Native datagram API socket (upstream src/native/mod.rs), a runtime
 * subsystem registered after the node and before the control socket. A
 * client connects to a SOCK_STREAM Unix socket at the documented path order
 * (mode 0770, group `fips`, the control socket's conventions), sends
 * line-delimited JSON commands and receives line-delimited replies; the two
 * setup replies carry a descriptor in their SCM_RIGHTS ancillary data: a
 * flow's half of a socket pair for `connect`, a listener's for `listen`.
 *
 * Ownership: the RPC connection owns nothing. A flow lives until its own
 * descriptor reaches end of file, a listener until its own does. One owner
 * fiber per client connection; one reader and one feeder fiber per flow; one
 * reader and one arrivals fiber per listener; one fiber consuming the node's
 * datagram sink. Every mailbox is bounded; nothing here blocks the dispatcher.
 * Shutdown cancels every parked descriptor wait (owner-directed cancellation)
 * so the runtime's grace period is never spent on a client. */
#include "control/control.h"
#include "node/context.h"
#include "node/handlers/handlers.h"
#include "native/registry.h"

#define FIPSD_NATIVE_MAX_CONNECTIONS 64u
/* Send-buffer bytes allowed per unread arrival or held datagram on a pair. */
#define FIPSD_NATIVE_ARRIVAL_ALLOWANCE 4096u
#define FIPSD_NATIVE_SINK_CAPACITY 256u
#define FIPSD_NATIVE_REPLY_TIMEOUT_MS 5000

/* The documented path order: /run/fips, /var/run/fips (macOS, FreeBSD),
 * $XDG_RUNTIME_DIR/fips, /tmp/fips-api.sock. Pure; see control.h. */
int fipsd_native_resolve_path(const char *override, const fipsd_control_path_env *env, char *out, size_t cap);

typedef struct fipsd_native_api fipsd_native_api;
/* Reads node.native_api.* from the current configuration snapshot. The
 * node's identity is taken at start, after the node subsystem has started. */
fipsd_native_api *fipsd_native_api_create(GoSwap *configs, fipsd_node *node, fipsd_node_handlers *handlers);
fipsd_subsystem *fipsd_native_api_subsystem(fipsd_native_api *api);
/* Layers the registry's flows and listeners onto the node's snapshot for
 * show_native_flows; commands pass through to the base provider. */
fipsd_control_provider fipsd_native_api_provider(fipsd_native_api *api, fipsd_control_provider base);
const char *fipsd_native_api_socket_path(const fipsd_native_api *api);
const fipsd_native_limits *fipsd_native_api_limits(const fipsd_native_api *api);
uint16_t fipsd_native_api_max_payload(const fipsd_native_api *api);
/* Test seam: owned events on a bounded channel the test drains; a full
 * channel drops the event. Set before the runtime starts. */
typedef enum fipsd_native_event_kind {
    FIPSD_NATIVE_EVENT_FLOW_RELEASED, FIPSD_NATIVE_EVENT_FLOW_UNPINNED,
    FIPSD_NATIVE_EVENT_LISTENER_RELEASED, FIPSD_NATIVE_EVENT_CONNECTION_CLOSED,
    FIPSD_NATIVE_EVENT_FLOW_READ,     /* one datagram read off a flow's descriptor */
    FIPSD_NATIVE_EVENT_DATAGRAM_COUNTED, /* one inbound datagram dispatched and counted; port is its destination */
    FIPSD_NATIVE_EVENT_FLOW_PARKED,   /* a macOS datagram flow's feeder is about to wait for its client's room */
    FIPSD_NATIVE_EVENT_FLOW_DROPPED   /* a datagram flow's feeder counted a drop_client_buffer_full */
} fipsd_native_event_kind;
typedef struct fipsd_native_event { fipsd_native_event_kind kind; uint64_t flow; uint16_t port; } fipsd_native_event;
void fipsd_native_api_observe(fipsd_native_api *api, GoChannel *events);
/* Test seam, set before start: called outside the registry lock just before
 * the feeder drops its reference. A channel barrier can delay reclamation. */
void fipsd_native_api_before_flow_reap(fipsd_native_api *api, void (*hook)(void *, uint64_t), void *user);
/* Test seam, set before start: after an accepted descriptor is sent, outside
 * the registry lock, before its flow record is published. */
void fipsd_native_api_before_flow_record(fipsd_native_api *api, void (*hook)(void *, uint64_t), void *user);
/* After the runtime has joined every fiber. Removes only the socket inode
 * this subsystem bound. */
void fipsd_native_api_destroy(fipsd_native_api *api);
#endif
