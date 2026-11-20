#ifndef FIPSD_GATEWAY_SHUTDOWN_H
#define FIPSD_GATEWAY_SHUTDOWN_H
/* Stopping the gateway's tasks, the port of upstream src/gateway/shutdown.rs
 * (nostrc-iq1n9.3).
 *
 * The resolver, the pool tick and the control socket run as fibers, all
 * feeding pool events into one bounded channel that the main loop drains.
 * When the main loop ends, fipsd_gw_stop_tasks stops them in the order that
 * lets each end, and gives up on any that does not end in time, so the route,
 * proxy NDP and NAT cleanup that follows is always reached. */
#include <channel.h>
#include <nsync.h>
#include <stdbool.h>

/* Capacity of the pool-event channel from the resolver and the tick. */
#define FIPSD_GW_EVENT_CAPACITY 64u
/* How long the resolver and the tick together get to end after shutdown is
 * signalled: well inside OpenWrt's kill 3 s after SIGTERM and systemd's
 * TimeoutStopSec=15, leaving time for the final state write and cleanup. */
#define FIPSD_GW_STOP_TIMEOUT_MS 1000u

/* One task's lifetime as the stop sees it. */
typedef struct fipsd_gw_task {
    nsync_mu mu; nsync_cv cv;
    const char *name;
    bool running, ended, failed; /* under mu */
} fipsd_gw_task;
void fipsd_gw_task_init(fipsd_gw_task *t, const char *name);
/* Before the task's fiber is spawned. */
void fipsd_gw_task_begin(fipsd_gw_task *t);
/* As the task's fiber returns; failed for an end the task did not choose. */
void fipsd_gw_task_end(fipsd_gw_task *t, bool failed);
bool fipsd_gw_task_ended(fipsd_gw_task *t);

typedef enum fipsd_gw_task_outcome {
    FIPSD_GW_TASK_FINISHED,  /* returned */
    FIPSD_GW_TASK_FAILED,    /* ended on its own failure */
    FIPSD_GW_TASK_ABANDONED, /* had not ended by the deadline; work it handed to a blocking thread may still run */
    FIPSD_GW_TASK_ABSENT     /* was not running (the main loop had seen it end) */
} fipsd_gw_task_outcome;
typedef struct fipsd_gw_stopped { fipsd_gw_task_outcome resolver, tick; } fipsd_gw_stopped;

typedef struct fipsd_gw_stop_hooks {
    GoChannel *events;                                   /* closed first */
    void (*signal)(void *user);                          /* signal shutdown */
    void (*abort_control)(void *user);                   /* stop the control socket at once */
    void (*abort)(void *user, fipsd_gw_task *task);      /* a task past the deadline */
    void *user;
} fipsd_gw_stop_hooks;
/* Close the event channel, signal shutdown, abort the control socket, then
 * wait for the resolver (NULL when not running) and the tick until one
 * deadline timeout_ms after the signal, aborting either one still running. */
fipsd_gw_stopped fipsd_gw_stop_tasks(const fipsd_gw_stop_hooks *hooks, fipsd_gw_task *resolver, fipsd_gw_task *tick,
                                     unsigned timeout_ms);
#endif
