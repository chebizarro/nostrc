/**
 * @file fiber_hooks.h
 * @brief Fiber-aware hooks for GoChannel / GoSelect cooperative blocking.
 *
 * These hooks allow GoChannel and go_select to cooperatively park/wake
 * fiber scheduler fibers instead of blocking OS threads, when called
 * from within a fiber context.
 *
 * The hooks use weak symbols so they resolve to no-ops (thread blocking)
 * when the fiber runtime is not linked.
 *
 * Usage from channel.c / select.c:
 *   if (gof_hook_current() != NULL) {
 *       // We're in a fiber — park instead of nsync_cv_wait
 *       gof_hook_block_current();
 *   } else {
 *       // Normal OS thread — use existing nsync_cv_wait
 *       nsync_cv_wait(...);
 *   }
 */
#ifndef LIBGO_FIBER_HOOKS_H
#define LIBGO_FIBER_HOOKS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque fiber handle (same as gof_fiber_t from fiber.h) */
typedef void *gof_fiber_handle;

/**
 * @brief Get the current fiber handle, or NULL if not running in a fiber.
 *
 * Use this to detect whether the caller is inside the fiber scheduler.
 * Returns NULL from normal pthreads, non-NULL from fiber workers.
 */
gof_fiber_handle gof_hook_current(void);

/**
 * @brief Park (block) the current fiber cooperatively.
 *
 * The fiber is suspended and the scheduler worker thread is freed to
 * run other fibers. The fiber must be made runnable again via
 * gof_hook_make_runnable().
 *
 * WARNING: Must NOT be called while holding any mutex that other fibers
 * on the same worker might need. Release locks before calling.
 *
 * No-op if not called from a fiber context.
 */
void gof_hook_block_current(void);

/**
 * @brief Park the current fiber until a deadline (or early wake via make_runnable).
 *
 * Like gof_hook_block_current() but with an absolute timeout.
 * The fiber will be woken by gof_hook_make_runnable() if called before
 * the deadline, OR automatically by the scheduler's sleeper mechanism
 * when deadline_ns (nanoseconds since epoch) expires.
 *
 * @param deadline_ns Absolute deadline in nanoseconds (clock_gettime CLOCK_REALTIME).
 *                    Use 0 to block indefinitely (same as gof_hook_block_current).
 *
 * No-op if not called from a fiber context.
 */
void gof_hook_block_current_until(uint64_t deadline_ns);

/**
 * @brief Make a previously-parked fiber runnable again.
 *
 * Safe to call from any thread (fiber worker, poller, or normal pthread).
 * The fiber will be enqueued to a worker's run queue.
 */
void gof_hook_make_runnable(gof_fiber_handle f);

#ifdef __cplusplus
}
#endif
#endif /* LIBGO_FIBER_HOOKS_H */
