#include "go.h"
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

// Wrapper function to create a new thread
int go(void *(*start_routine)(void *), void *arg) {
    GoWrapper *w = (GoWrapper *)malloc(sizeof(GoWrapper));
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
        free(w);
        return result;
    }
    // Detach the thread to allow it to clean up resources automatically upon completion
    result = pthread_detach(thread);
    if (result != 0) {
        fprintf(stderr, "Failed to detach thread: %d\n", result);
    }
    return result;
}
