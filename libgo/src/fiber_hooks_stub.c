/**
 * @file fiber_hooks_stub.c
 * @brief Link-order-independent dispatch for optional fiber support.
 *
 * Core libgo owns these symbols, even when go_fiber is linked. gof_init()
 * publishes the real callbacks explicitly; without it we use OS threads.
 */
#include "fiber_hooks.h"
#include <stdatomic.h>

static _Atomic(const struct go_fiber_hooks *) fiber_hooks = NULL;

void go_register_fiber_hooks(const struct go_fiber_hooks *hooks) {
    atomic_store_explicit(&fiber_hooks, hooks, memory_order_release);
}

gof_fiber_handle gof_hook_current(void) {
    const struct go_fiber_hooks *hooks =
        atomic_load_explicit(&fiber_hooks, memory_order_acquire);
    return hooks ? hooks->current() : NULL;
}

void gof_hook_block_current(void) {
    const struct go_fiber_hooks *hooks =
        atomic_load_explicit(&fiber_hooks, memory_order_acquire);
    if (hooks) hooks->block_current();
}

void gof_hook_block_current_until(uint64_t deadline_ns) {
    const struct go_fiber_hooks *hooks =
        atomic_load_explicit(&fiber_hooks, memory_order_acquire);
    if (hooks) hooks->block_current_until(deadline_ns);
}

void gof_hook_make_runnable(gof_fiber_handle f) {
    const struct go_fiber_hooks *hooks =
        atomic_load_explicit(&fiber_hooks, memory_order_acquire);
    if (hooks) hooks->make_runnable(f);
}
