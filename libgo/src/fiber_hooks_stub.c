/**
 * @file fiber_hooks_stub.c
 * @brief Weak-symbol stubs for fiber hooks when fiber runtime is not linked.
 *
 * These return NULL / no-op so that GoChannel and go_select fall back
 * to OS-thread blocking (nsync_cv_wait) when fibers aren't available.
 *
 * When the fiber runtime IS linked, the real implementations in
 * fiber_hooks_impl.c override these via stronger symbols.
 */
#include "fiber_hooks.h"

__attribute__((weak))
gof_fiber_handle gof_hook_current(void) {
    return NULL; /* Not in a fiber — use OS thread blocking */
}

__attribute__((weak))
void gof_hook_block_current(void) {
    /* No-op: caller should not reach here if gof_hook_current() returned NULL */
}

__attribute__((weak))
void gof_hook_block_current_until(uint64_t deadline_ns) {
    (void)deadline_ns;
    /* No-op: caller should not reach here if gof_hook_current() returned NULL */
}

__attribute__((weak))
void gof_hook_make_runnable(gof_fiber_handle f) {
    (void)f;
    /* No-op: caller should not reach here if gof_hook_current() returned NULL */
}
