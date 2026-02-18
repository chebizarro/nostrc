#include "go.h"
#include "go_auto.h"
#include <nsync.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

/* Global counter for active goroutines (for debugging) */
static atomic_int g_active_goroutines = 0;

int go_get_active_count(void) {
    return atomic_load(&g_active_goroutines);
}

/* Wrapper to track goroutine lifecycle */
typedef struct {
    void *(*start_routine)(void *);
    void *arg;
} GoWrapper;

static void *go_wrapper_func(void *arg) {
    GoWrapper *w = (GoWrapper *)arg;
    void *(*fn)(void *) = w->start_routine;
    void *fn_arg = w->arg;
    free(w);
    
    atomic_fetch_add(&g_active_goroutines, 1);
    void *result = fn(fn_arg);
    atomic_fetch_sub(&g_active_goroutines, 1);
    
    return result;
}

/* Forward declaration — provided by fiber runtime.
 * We use a function pointer that defaults to NULL and is set by the fiber
 * runtime when it initializes. This avoids weak symbol issues on macOS. */
struct gof_fiber;
typedef struct gof_fiber gof_fiber_t;
typedef void (*gof_fn)(void *arg);

/* Function pointer to fiber spawn — NULL if fiber runtime not linked/initialized */
static gof_fiber_t* (*gof_spawn_ptr)(gof_fn fn, void *arg, size_t stack_bytes) = NULL;

/* Called by fiber runtime to register its spawn function */
void go_register_fiber_spawn(gof_fiber_t* (*spawn_fn)(gof_fn, void*, size_t)) {
    gof_spawn_ptr = spawn_fn;
}

int go_fiber(void (*fn)(void *arg), void *arg, size_t stack_bytes) {
    if (!gof_spawn_ptr) {
        /* Fiber runtime not linked — fall back to error */
        fprintf(stderr, "go_fiber: fiber runtime not linked\n");
        return -1;
    }
    gof_fiber_t *f = gof_spawn_ptr(fn, arg, stack_bytes);
    return f ? 0 : -1;
}

/* ── go_fiber_compat: fiber-based replacement for go() ─────────────────
 * Accepts the pthread-style void*(*)(void*) signature, wraps it into a
 * void(*)(void*) trampoline so existing goroutine functions can be migrated
 * to fibers without changing their signatures. Falls back to go() (OS thread)
 * if the fiber runtime is not linked. */

typedef struct {
    void *(*fn)(void *);
    void  *arg;
} GoFiberCompatWrapper;

static void go_fiber_compat_trampoline(void *ctx) {
    GoFiberCompatWrapper *w = (GoFiberCompatWrapper *)ctx;
    void *(*fn)(void *) = w->fn;
    void *arg = w->arg;
    free(w);
    atomic_fetch_add(&g_active_goroutines, 1);
    fn(arg);  /* return value discarded — same as detached pthread */
    atomic_fetch_sub(&g_active_goroutines, 1);
}

int go_fiber_compat(void *(*start_routine)(void *), void *arg) {
    /* nostrc-b0h-revert: Always use OS threads instead of fibers.
     * The fiber runtime has issues where spawned fibers don't execute,
     * causing relay message loops to never start. OS threads are reliable. */
    return go(start_routine, arg);
}

// Wrapper function to create a new thread
int go(void *(*start_routine)(void *), void *arg) {
    go_autofree GoWrapper *w = (GoWrapper *)malloc(sizeof(GoWrapper));
    if (!w) {
        fprintf(stderr, "Failed to allocate goroutine wrapper\n");
        return -1;
    }
    w->start_routine = start_routine;
    w->arg = arg;
    
    pthread_t thread;
    int result = pthread_create(&thread, NULL, go_wrapper_func, w);
    if (result != 0) {
        fprintf(stderr, "Failed to create thread: %d\n", result);
        return result; /* w still valid, auto-freed at scope exit */
    }
    /* Only steal the pointer after pthread_create succeeds */
    go_steal_pointer(&w);
    // Detach the thread to allow it to clean up resources automatically upon completion
    result = pthread_detach(thread);
    if (result != 0) {
        fprintf(stderr, "Failed to detach thread: %d\n", result);
    }
    return result;
}
