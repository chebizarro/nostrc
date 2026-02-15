#ifndef GO_H
#define GO_H

#include "channel.h"
#include "context.h"
#include "counter.h"
#include "hash_map.h"
#include "int_array.h"
#include "refptr.h"
#include "string_array.h"
#include "wait_group.h"
#include "gtime.h"
#include "ticker.h"
#include "error.h"
#include "select.h"
#include "blocking_executor.h"

// Launch a detached goroutine-like thread executing start_routine(arg).
// Returns 0 on success, non-zero on failure.
// Prefer coordinating completion with GoWaitGroup instead of sleeps.
int go(void *(*start_routine)(void *), void *arg);

/**
 * @brief Launch a fiber (cooperative lightweight thread) instead of an OS thread.
 *
 * This is the fiber-based alternative to go(). The function runs on the
 * fiber scheduler's worker pool instead of creating a new pthread.
 *
 * Benefits over go():
 *   - ~64KB stack vs ~8MB per OS thread
 *   - Cooperative scheduling (no preemption overhead)
 *   - All fiber channel ops (gof_chan) are non-blocking to the worker
 *   - Works with GoChannel when fiber hooks are linked
 *
 * @param fn Entry function. Note: void(*)(void*) signature (not void*(*)(void*)).
 * @param arg User argument passed to fn.
 * @param stack_bytes Stack size (0 = default, typically 256KB).
 * @return 0 on success, -1 on failure.
 *
 * @note The fiber scheduler must be running (via gof_start_background() or
 *       gof_run()) before calling this.
 */
int go_fiber(void (*fn)(void *arg), void *arg, size_t stack_bytes);

/**
 * @brief Convenience wrapper: launch a fiber with the default stack size.
 *
 * Equivalent to go_fiber(fn, arg, 0).
 */
static inline int go_fiber_default(void (*fn)(void *arg), void *arg) {
    return go_fiber(fn, arg, 0);
}

/**
 * @brief Fiber-based drop-in replacement for go().
 *
 * Accepts the same void*(*)(void*) signature as go() so existing goroutine
 * functions can be migrated without changing their signatures. The return
 * value of start_routine is discarded (same as a detached pthread).
 *
 * If the fiber runtime is not linked, transparently falls back to go()
 * (creates an OS thread). This makes it safe to use unconditionally.
 *
 * @param start_routine Function to execute (pthread-compatible signature).
 * @param arg           Argument passed to start_routine.
 * @return 0 on success, non-zero on failure.
 */
int go_fiber_compat(void *(*start_routine)(void *), void *arg);

// Get the current count of active goroutines (for debugging)
int go_get_active_count(void);

/**
 * @brief Register a fiber spawn function with libgo.
 *
 * Called by the fiber runtime (go_fiber library) during initialization to
 * register its spawn function. This allows go_fiber_compat() to use fibers
 * when the fiber runtime is linked.
 *
 * @param spawn_fn The fiber spawn function to register.
 * @note This is an internal API used by the fiber runtime. Applications
 *       should not call this directly.
 */
struct gof_fiber;
typedef struct gof_fiber gof_fiber_t;
typedef void (*gof_fn)(void *arg);
void go_register_fiber_spawn(gof_fiber_t* (*spawn_fn)(gof_fn, void*, size_t));

#endif // GO_H
