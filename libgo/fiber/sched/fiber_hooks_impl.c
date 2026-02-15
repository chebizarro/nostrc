/**
 * @file fiber_hooks_impl.c
 * @brief Real fiber hook implementations (overrides weak stubs).
 *
 * These are linked when the fiber runtime is included. They provide
 * the actual cooperative parking/waking primitives that GoChannel
 * and go_select use to avoid blocking OS threads from fiber contexts.
 */
#include "../../include/fiber_hooks.h"
#include "sched.h"

gof_fiber_handle gof_hook_current(void) {
    gof_fiber *f = gof_sched_current();
    return (gof_fiber_handle)f;
}

void gof_hook_block_current(void) {
    gof_sched_block_current();
}

void gof_hook_make_runnable(gof_fiber_handle f) {
    if (!f) return;
    gof_sched_make_runnable((gof_fiber *)f);
}
