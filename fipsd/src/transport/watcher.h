#ifndef FIPSD_WATCHER_H
#define FIPSD_WATCHER_H
#include "transport/transport.h"
typedef struct fipsd_watcher fipsd_watcher;
typedef enum fipsd_bind_result { FIPSD_BIND_OK, FIPSD_BIND_ABSENT, FIPSD_BIND_RETRY } fipsd_bind_result;
typedef struct fipsd_binder_ops {
    /* Sole binder fiber. Setup syscalls must use the blocking executor; receive
     * tasks use gof_* and are tracked. Failed binds unwind partial resources.
     * stop is a cancellation channel, including cancellation during bind. */
    fipsd_bind_result (*bind)(void *, fipsd_runtime *, GoChannel *stop,
                             const fipsd_presence_facts *, uint64_t generation);
    /* Cancel/wake and join this generation's I/O before closing its descriptor.
     * Never close a descriptor still parked in gof_read/recv*. May fiber-wait. */
    void (*unbind)(void *, uint64_t generation);
    void *user;
} fipsd_binder_ops;
typedef struct fipsd_watcher_source {
    /* Optional injected facts; called on the blocking executor. Without it,
     * node netmon snapshots are authoritative, with native medium/MAC reads.
     * No node context: read native getifaddrs (standalone transport). */
    fipsd_presence_facts (*read)(void *);
    void *user;
    GoChannel *ticks; /* optional manual backstop; otherwise a 1 s ticker */
    uint64_t (*now_ms)(void *); /* optional deterministic clock */
} fipsd_watcher_source;
fipsd_watcher *fipsd_watcher_create(fipsd_transport *, const fipsd_binder_ops *, const fipsd_watcher_source *);
/* Opt in before start. Probe runs on owner after a network notification;
 * no periodic interface reads. Retry/stability deadlines remain bounded. */
void fipsd_watcher_event_driven(fipsd_watcher *, bool (*healthy)(void *));
/* Optional borrowed invalidation channel, installed before start. Pulses follow
 * state publication for BOTH bind and unbind; consumers snapshot current state. */
void fipsd_watcher_binding_updates(fipsd_watcher *, GoChannel *);
int fipsd_watcher_start(fipsd_watcher *, fipsd_runtime *, const fipsd_node_context *, GoChannel *events);
void fipsd_watcher_network_changed(fipsd_watcher *, const fipsd_netmon_event *);
/* A stale receive/writer exit cannot detach a replacement binding. */
void fipsd_watcher_binding_exited(fipsd_watcher *, uint64_t generation);
void fipsd_watcher_stop(fipsd_watcher *); /* prompt, idempotent */
void fipsd_watcher_destroy(fipsd_watcher *); /* only after runtime join */
/* Same reducer used by the actor, exposed for clock-driven tests. Sole owner
 * only; invalid reads retain the last binding, carrier is never a bind gate. */
/* Sole owner: UINT64_MAX means no pending deadline; overdue retries are
 * retained so scheduler delays cannot strand an event-only binding. */
uint64_t fipsd_watcher_next_deadline(fipsd_watcher *);
int fipsd_watcher_step(fipsd_watcher *, fipsd_presence_facts, uint64_t now_ms);
#endif
