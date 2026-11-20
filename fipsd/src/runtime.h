#ifndef FIPSD_RUNTIME_H
#define FIPSD_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>

#include "channel.h"
#include "context.h"
#include "wait_group.h"

/*
 * Fiber runtime bring-up, signals and orderly shutdown.
 *
 * Lifecycle (one runtime per process; the fiber scheduler is process-global):
 *
 *   create -> register subsystems -> start -> run -> destroy
 *
 * start:  sizes the scheduler (FIPSD_WORKERS env, else cfg.workers, else
 *         GOF_NWORKERS env, else online CPUs; pollers cfg.pollers else
 *         clamp(workers/4, 1, 4)), gof_start_background(), the blocking
 *         executor, the signal fiber, then each subsystem's start() in
 *         registration order on the calling thread. A failing start() stops
 *         the already-started subsystems in reverse and tears the runtime
 *         down; start returns -1.
 * run:    blocks the calling (main) thread until the root context is
 *         cancelled: SIGINT/SIGTERM via the signal fiber, or
 *         fipsd_runtime_shutdown(). Then: stop() on every started subsystem in
 *         reverse order; wait for the subsystem wait group up to cfg.grace_ms
 *         (FIPSD_RUN_GRACE_ELAPSED if it does not drain); stop the signal
 *         fiber; gof_request_stop(); gof_join_background(); blocking executor
 *         shutdown.
 * SIGHUP: delivers one token on the reload channel (capacity 1; a SIGHUP
 *         while a token is pending is coalesced). The configuration loader
 *         (nostrc-9keoz.2) consumes it.
 *
 * Signals are turned into events by a self-pipe: the async-signal-safe
 * handler writes the signal number to a non-blocking pipe and a fiber parked
 * in gof_read() dispatches it. No fiber ever blocks on a raw syscall; the
 * main thread is not a fiber and waits with nsync.
 *
 * Fiber rules for subsystems: spawn through fipsd_runtime_spawn() so the
 * runtime can account for the fiber; observe fipsd_runtime_context() (its
 * done channel is a select case) and return promptly after cancellation;
 * never block a fiber on anything but gof_* / libgo primitives; route
 * blocking calls through go_blocking_submit(); hold locks briefly and never
 * across a gof_* call; CPU-bound ticks yield.
 */

typedef struct fipsd_runtime fipsd_runtime;
typedef struct fipsd_subsystem fipsd_subsystem;

struct fipsd_subsystem {
    const char *name;
    /* Called on the starting thread, in registration order. Return 0, or -1
     * to abort start-up. Spawn fibers with fipsd_runtime_spawn(). */
    int (*start)(fipsd_subsystem *self, fipsd_runtime *rt);
    /* Called on the shutting-down thread, in reverse order, after the root
     * context is cancelled. Request the subsystem's fibers to finish and
     * return promptly; the runtime waits for them within the grace period. */
    void (*stop)(fipsd_subsystem *self, fipsd_runtime *rt);
    void *user;
};

#define FIPSD_RUNTIME_MAX_SUBSYSTEMS 16
#define FIPSD_RUNTIME_DEFAULT_GRACE_MS 5000u

typedef struct fipsd_runtime_config {
    int workers;           /* 0: FIPSD_WORKERS / GOF_NWORKERS / online CPUs */
    int pollers;           /* 0: derived from workers */
    int blocking_threads;  /* 0: libgo default */
    unsigned grace_ms;     /* 0: FIPSD_RUNTIME_DEFAULT_GRACE_MS */
    bool handle_signals;   /* install SIGINT/SIGTERM/SIGHUP handling */
} fipsd_runtime_config;

void fipsd_runtime_config_default(fipsd_runtime_config *cfg);

typedef enum fipsd_run_status {
    FIPSD_RUN_OK = 0,
    FIPSD_RUN_GRACE_ELAPSED = 1 /* some subsystem fiber outlived the grace */
} fipsd_run_status;

fipsd_runtime *fipsd_runtime_create(const fipsd_runtime_config *cfg);
void fipsd_runtime_destroy(fipsd_runtime *rt);

/* Before start. The subsystem struct must outlive the runtime. */
int fipsd_runtime_register(fipsd_runtime *rt, fipsd_subsystem *subsystem);

int fipsd_runtime_start(fipsd_runtime *rt);
/* Returns a fipsd_run_status (>= 0), or -1 when the runtime is not started. */
int fipsd_runtime_run(fipsd_runtime *rt);
/* Programmatic equivalent of SIGTERM; idempotent, callable from any thread. */
void fipsd_runtime_shutdown(fipsd_runtime *rt);

GoContext *fipsd_runtime_context(fipsd_runtime *rt);
GoWaitGroup *fipsd_runtime_wait_group(fipsd_runtime *rt);
GoChannel *fipsd_runtime_reload_channel(fipsd_runtime *rt);
/* Effective worker count once started (the scheduler's, which may differ
 * from the request when an earlier gof_init() in the process fixed it). */
int fipsd_runtime_workers(const fipsd_runtime *rt);

/* Spawns fn(arg) as a tracked fiber: the wait group is incremented before
 * the spawn and decremented when fn returns. Returns 0 or -1. */
int fipsd_runtime_spawn(fipsd_runtime *rt, void (*fn)(void *arg), void *arg);

#endif /* FIPSD_RUNTIME_H */
