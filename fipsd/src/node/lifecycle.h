#ifndef FIPSD_NODE_LIFECYCLE_H
#define FIPSD_NODE_LIFECYCLE_H
#include "node/status.h"
#include "runtime.h"

#define FIPSD_NODE_MAX_CHILDREN 40u
typedef enum fipsd_node_event_kind {
    FIPSD_NODE_START, FIPSD_NODE_UP, FIPSD_NODE_UP_DEGRADED, FIPSD_NODE_START_FAILED,
    FIPSD_NODE_EXITED, FIPSD_NODE_ABSENT, FIPSD_NODE_PRESENT,
    FIPSD_NODE_DRAIN, FIPSD_NODE_DRAIN_COMPLETE, FIPSD_NODE_STOP,
    FIPSD_NODE_CHILD_STOPPED
} fipsd_node_event_kind;
typedef struct fipsd_node_event {
    fipsd_node_event_kind kind;
    size_t child;
    uint64_t deadline_ms;
} fipsd_node_event;
typedef enum fipsd_node_action_kind {
    FIPSD_NODE_SPAWN, FIPSD_NODE_STOP_CHILD, FIPSD_NODE_PUBLISH,
    FIPSD_NODE_GATE_PEERING, FIPSD_NODE_ARM_DRAIN
} fipsd_node_action_kind;
typedef struct fipsd_node_action {
    fipsd_node_action_kind kind;
    size_t child;
} fipsd_node_action;
typedef struct fipsd_node_actions {
    size_t count;
    fipsd_node_action items[FIPSD_NODE_MAX_CHILDREN + 2];
} fipsd_node_actions;
typedef struct fipsd_supervisor {
    fipsd_node_status status;
    uint64_t configured, transports, required, pending, owned, deadline_ms;
    size_t count;
} fipsd_supervisor;
/* Pure reducer. Required substrate failure is fatal; optional worker failure
 * degrades when a transport is up. No automatic restarts. Absence is reversible
 * and still up; exited/failed is latched until a new START after STOPPED.
 * Runtime last-transport exit publishes Failed, does not initiate teardown.
 * All children tear down in reverse startup order, including workers (unlike
 * the historical upstream omission). Unknown/duplicate/late events are inert. */
int fipsd_supervisor_init(fipsd_supervisor *, size_t children,
                          uint64_t transports, uint64_t required);
void fipsd_supervisor_step(fipsd_supervisor *, fipsd_node_event, fipsd_node_actions *);

typedef struct fipsd_node_child {
    const char *name;
    bool transport, required;
    /* On lifecycle fiber. Start must unwind partial failure. Stop may await
     * fiber/channel completion, never raw blocking I/O. Runtime grace bounds
     * the whole actor; identity/resources remain allocated until runtime join. */
    int (*start)(void *, fipsd_runtime *);
    void (*stop)(void *);
    void *user;
    bool (*absent)(void *);
    bool (*degraded)(void *); /* partial optional child: live handle, short health */
} fipsd_node_child;
typedef struct fipsd_lifecycle fipsd_lifecycle;
fipsd_lifecycle *fipsd_lifecycle_create(const fipsd_node_child *, size_t count);
fipsd_subsystem *fipsd_lifecycle_subsystem(fipsd_lifecycle *);
GoSwap *fipsd_lifecycle_status(fipsd_lifecycle *);
/* Nonblocking bounded producer seam; on success takes malloc'd event. A full
 * queue refuses (caller retains); async producers may select inbox send against
 * root cancellation. One sole actor owns the reducer and publishes snapshots. */
GoChannel *fipsd_lifecycle_inbox(fipsd_lifecycle *);
void fipsd_lifecycle_destroy(fipsd_lifecycle *); /* after runtime join */
#endif
