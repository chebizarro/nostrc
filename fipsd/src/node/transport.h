#include "node/perf_profile.h"
#ifndef FIPSD_TRANSPORT_H
#define FIPSD_TRANSPORT_H
#include "runtime.h"
#include <fips/transport/types.h>

#define FIPSD_TRANSPORT_MAX 32u
typedef struct fipsd_node_context fipsd_node_context;
typedef enum fipsd_transport_state {
    FIPSD_TRANSPORT_STOPPED, FIPSD_TRANSPORT_ABSENT,
    FIPSD_TRANSPORT_BOUND, FIPSD_TRANSPORT_FAILED
} fipsd_transport_state;
typedef enum fipsd_transport_event_kind {
    FIPSD_TRANSPORT_PACKET, FIPSD_TRANSPORT_DISCOVERED,
    FIPSD_TRANSPORT_CONNECTED, FIPSD_TRANSPORT_DISCONNECTED,
    FIPSD_TRANSPORT_BECAME_ABSENT, FIPSD_TRANSPORT_BECAME_PRESENT,
    FIPSD_TRANSPORT_EXITED, FIPSD_TRANSPORT_MEDIUM_CHANGED
} fipsd_transport_event_kind;
typedef struct fipsd_transport_event {
    FIPSD_PERF_QUEUE_FIELD
    fipsd_transport_event_kind kind;
    fips_transport_id id;
    fips_transport_addr address;
    size_t size;
    uint8_t bytes[];
} fipsd_transport_event;

/* Which of a connection's send queues a frame joins (upstream
 * transport::Lane). A transport that queues frames for a writer keeps CONTROL
 * apart from DATA, so a queue full of the node's data neither refuses nor
 * holds back path messages and link handshakes (the only CONTROL frames,
 * nostrc-3vl7v), and its writer sends CONTROL first. The node
 * marks the lane; a transport cannot tell from ciphertext. Transports that
 * send at once, with no queue of their own, ignore it. */
typedef enum fipsd_transport_lane {
    FIPSD_TRANSPORT_LANE_DATA, FIPSD_TRANSPORT_LANE_CONTROL
} fipsd_transport_lane;
typedef struct fipsd_transport fipsd_transport;
/* One frame of a batched send (nostrc-u19cz.4); everything is borrowed. */
typedef struct fipsd_transport_frame {
    const fips_transport_addr *to;
    const uint8_t *bytes;
    size_t len;
} fipsd_transport_frame;
struct fipsd_netmon_event;
struct fips_node_addr;
typedef struct fipsd_transport_ops {
    /* On a fiber. Nonblocking start: launch binder/receive tasks through rt.
     * Missing interface is success with state ABSENT, not a startup failure.
     * Binder owns detach/rebind; supervisor never restarts a transport.
     * Send malloc'd events via events with root cancellation; failed send
     * keeps ownership. No borrowed packets. All tasks must stop on root done.
     * Partial failure must unwind itself, including any spawned tasks. */
    int (*start_async)(fipsd_transport *, fipsd_runtime *,
                       const fipsd_node_context *, GoChannel *events);
    /* Prompt, idempotent cancellation/unbind request; no blocking join.
     * Runtime's tracked-fiber grace is the join boundary. */
    void (*stop)(fipsd_transport *);
    fipsd_transport_state (*state)(const fipsd_transport *);
    uint16_t (*mtu)(const fipsd_transport *, const fips_transport_addr *);
    /* bytes/to are borrowed through return ONLY. An async/buffered transport
     * must copy or take its own owned message before returning (packet slabs
     * can be wiped/reused immediately). No retained caller buffer pointers. */
    int (*send)(fipsd_transport *, const fips_transport_addr *, const uint8_t *, size_t);
    int (*discover)(fipsd_transport *);
    int (*connect_async)(fipsd_transport *, const fips_transport_addr *);
    void (*close_connection)(fipsd_transport *, const fips_transport_addr *);
    /* Dispatcher notification to binder; callback must enqueue/coalesce work,
     * not race binder-owned descriptor state. Snapshot is loaded from context. */
    void (*network_changed)(fipsd_transport *, const struct fipsd_netmon_event *);
    /* Dispatcher-only, AFTER Noise/AEAD authentication. Datagram transports
     * may promote a numeric endpoint into a connected socket; never called
     * merely because an unauthenticated datagram arrived. Best effort. */
    void (*authenticated)(fipsd_transport *, const fips_transport_addr *, fips_link_direction);
    bool (*local_addr)(const fipsd_transport *, fips_transport_addr *);
    /* Optional, dispatcher-only (upstream TransportHandle::mark_verified /
     * clear_verified; BLE only, which keys links by a claim made before any
     * handshake): the active peer `node` now sends over the link at the
     * address (uses), or no longer does (removed: the peer itself is gone,
     * rather than moved to another link). Must not block. */
    void (*peer_link)(fipsd_transport *, const fips_transport_addr *, const struct fips_node_addr *,
                      bool uses, bool removed);
    /* Optional, the node's maintenance tick (FRC-1301 6.5.4, upstream
     * close_unproven_ble_links): close every channel that never carried a
     * handshake and that the node has not held (`held`: a leg or link on the
     * address) for timeout_ms. Returns the number closed. Must not block. */
    size_t (*close_unproven)(fipsd_transport *, uint64_t timeout_ms,
                             bool (*held)(void *, const fips_transport_addr *), void *);
    /* Optional (upstream TransportHandle::send_lane): send's contract, on the
     * connection's queue for `lane`. NULL: the transport has one queue (or
     * none), and fipsd_transport_send_lane falls back to send. */
    int (*send_lane)(fipsd_transport *, fipsd_transport_lane, const fips_transport_addr *,
                     const uint8_t *, size_t);
    /* Optional (nostrc-u19cz.3): true when send(to) may run on any OS thread,
     * concurrently with the dispatcher and other senders, and returns without
     * parking or waiting on name resolution. Crypto workers then send sealed
     * data-lane frames to `to` themselves. NULL: dispatcher-only sends. */
    bool (*send_concurrent)(const fipsd_transport *, const fips_transport_addr *to);
    /* Optional (nostrc-u19cz.4): n data-lane sends with send's contract, in
     * order, as n send calls would make them, in fewer syscalls (Linux UDP:
     * sendmmsg, and UDP GSO for runs of equal-size datagrams). rc[i] is frame
     * i's send result (0 or -1); one frame's failure never skips the frames
     * after it. Returns how many were sent. Thread rules are send's for each
     * frame's `to` (see send_concurrent). NULL: fipsd_transport_send_batch
     * makes the n sends. */
    size_t (*send_batch)(fipsd_transport *, const fipsd_transport_frame *, size_t n, int *rc);
    /* Optional: 0 when send_lane(lane, to) would currently accept a frame,
     * -1 when it would refuse one as full, accounted as that refusal. The node
     * asks before sealing a data-lane frame, so saturation is not paid for in
     * AEAD work the lane then discards. Advisory: send_lane stays the verdict.
     * Never parks. NULL: always admitted. */
    int (*lane_admit)(fipsd_transport *, fipsd_transport_lane, const fips_transport_addr *);
} fipsd_transport_ops;
struct fipsd_transport {
    const fipsd_transport_ops *ops;
    fips_transport_id id;
    const char *type, *name; /* borrowed stable metadata */
    bool auto_connect, accept_connections;
    void *user;
    /* Optional interface binding, NULL for unbound transports (including
     * loopback by default). Borrowed immutable name, set before registration
     * and valid through runtime join; binder still owns descriptors/rebinding.
     * Loopback tests may supply a fake name without touching host interfaces. */
    const char *interface_name;
    /* Append-only shared instance; NULL for legacy/custom implementations. */
    struct fipsd_transport_shared *shared;
};
typedef struct fipsd_transport_registry {
    fipsd_transport *items[FIPSD_TRANSPORT_MAX];
    size_t count;
    bool sealed;
} fipsd_transport_registry;
/* Registration is single-threaded, before lifecycle start. Borrowed objects
 * outlive runtime join. Duplicate ids/names, incomplete vtables and late adds
 * are refused without mutation. No hidden default/network transports. */
fips_status fipsd_transport_parse_address(const fipsd_transport *, const char *, fips_transport_addr *);
int fipsd_transport_register(fipsd_transport_registry *, fipsd_transport *);
/* The node's send seam: lane-aware where the transport supports it. */
static inline int fipsd_transport_send_lane(fipsd_transport *t, fipsd_transport_lane lane,
    const fips_transport_addr *to, const uint8_t *bytes, size_t n)
{ return t->ops->send_lane ? t->ops->send_lane(t, lane, to, bytes, n) : t->ops->send(t, to, bytes, n); }
static inline int fipsd_transport_lane_admit(fipsd_transport *t, fipsd_transport_lane lane,
    const fips_transport_addr *to)
{ return t->ops->lane_admit ? t->ops->lane_admit(t, lane, to) : 0; }
static inline size_t fipsd_transport_send_batch(fipsd_transport *t, const fipsd_transport_frame *f,
    size_t n, int *rc)
{
    if (t->ops->send_batch) return t->ops->send_batch(t, f, n, rc);
    size_t sent = 0;
    for (size_t i = 0; i < n; ++i)
        sent += (rc[i] = fipsd_transport_send_lane(t, FIPSD_TRANSPORT_LANE_DATA, f[i].to, f[i].bytes, f[i].len)) == 0;
    return sent;
}
fipsd_transport *fipsd_transport_find(const fipsd_transport_registry *, fips_transport_id);
#endif
