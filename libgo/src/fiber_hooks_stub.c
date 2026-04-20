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

/* On Windows/MinGW, __attribute__((weak)) doesn't work reliably with
 * static archives.  The fiber runtime is not built on Windows, so these
 * stubs must be strong symbols.  On ELF platforms the real fiber runtime
 * overrides them via stronger symbols when go_fiber is linked. */
#if defined(_WIN32) || defined(__MINGW32__)
#define GOF_STUB_ATTR /* strong symbol */
#else
#define GOF_STUB_ATTR __attribute__((weak))
#endif

GOF_STUB_ATTR
gof_fiber_handle gof_hook_current(void) {
    return NULL; /* Not in a fiber — use OS thread blocking */
}

GOF_STUB_ATTR
void gof_hook_block_current(void) {
    /* No-op: caller should not reach here if gof_hook_current() returned NULL */
}

GOF_STUB_ATTR
void gof_hook_block_current_until(uint64_t deadline_ns) {
    (void)deadline_ns;
    /* No-op: caller should not reach here if gof_hook_current() returned NULL */
}

GOF_STUB_ATTR
void gof_hook_make_runnable(gof_fiber_handle f) {
    (void)f;
    /* No-op: caller should not reach here if gof_hook_current() returned NULL */
}
