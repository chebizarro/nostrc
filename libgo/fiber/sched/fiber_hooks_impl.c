/**
 * @file fiber_hooks_impl.c
 * @brief Registered fiber hook implementations.
 *
 * gof_init() explicitly references this object and registers its callbacks.
 * They provide the cooperative parking/waking primitives that GoChannel
 * and go_select use to avoid blocking OS threads from fiber contexts.
 */
#include "../../include/fiber_hooks.h"
#include "sched.h"

static gof_fiber_handle hook_current(void) {
    gof_fiber *f = gof_sched_current();
    return (gof_fiber_handle)f;
}

static void hook_block_current(void) {
    gof_sched_block_current();
}

static void hook_block_current_until(uint64_t deadline_ns) {
    if (deadline_ns == 0) {
        gof_sched_block_current();
        return;
    }
    gof_sched_park_until(deadline_ns);
}

static void hook_make_runnable(gof_fiber_handle f) {
    if (!f) return;
    gof_sched_make_runnable((gof_fiber *)f);
}

void gof_register_channel_hooks(void) {
    static const struct go_fiber_hooks hooks = {
        .current = hook_current,
        .block_current = hook_block_current,
        .block_current_until = hook_block_current_until,
        .make_runnable = hook_make_runnable,
    };
    go_register_fiber_hooks(&hooks);
}
