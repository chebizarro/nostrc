/**
 * @file blocking_executor.c
 * @brief Bounded OS-thread pool for offloading blocking I/O from fibers.
 *
 * Architecture:
 *   - Fixed-size pool of OS threads (pthreads)
 *   - Lock-free-ish work queue protected by nsync_mu + nsync_cv
 *   - Fiber integration via gof_hook_current/block/make_runnable
 *   - Graceful shutdown with drain semantics
 */
#include "blocking_executor.h"
#include "fiber_hooks.h"
#include "go_auto.h"
#include <nsync.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

/* ── Work item ────────────────────────────────────────────────────────── */

typedef struct GoBlockingWork {
    void *(*fn)(void *arg);
    void  *arg;
    void  *result;
    /* Fiber that submitted this work (NULL if non-fiber caller) */
    gof_fiber_handle  fiber;
    /* Completion flag for non-fiber callers */
    nsync_mu          done_mu;
    nsync_cv          done_cv;
    _Atomic int       done;
    struct GoBlockingWork *next;
} GoBlockingWork;

/* ── Executor state ───────────────────────────────────────────────────── */

#define BLOCKING_EXECUTOR_DEFAULT_THREADS 4
#define BLOCKING_EXECUTOR_MAX_THREADS     64

static struct {
    pthread_t  *threads;
    size_t      num_threads;
    _Atomic int initialized;
    _Atomic int shutting_down;
    _Atomic int active_count;
    _Atomic int pending_count;

    /* Work queue (singly-linked, FIFO) */
    nsync_mu    queue_mu;
    nsync_cv    queue_cv;
    GoBlockingWork *queue_head;
    GoBlockingWork *queue_tail;
} g_executor = {
    .threads       = NULL,
    .num_threads   = 0,
    .initialized   = 0,
    .shutting_down = 0,
    .active_count  = 0,
    .pending_count = 0,
    .queue_head    = NULL,
    .queue_tail    = NULL,
};

/* ── Queue operations ─────────────────────────────────────────────────── */

static void queue_push(GoBlockingWork *w) {
    nsync_mu_lock(&g_executor.queue_mu);
    w->next = NULL;
    if (g_executor.queue_tail) {
        g_executor.queue_tail->next = w;
    } else {
        g_executor.queue_head = w;
    }
    g_executor.queue_tail = w;
    atomic_fetch_add_explicit(&g_executor.pending_count, 1, memory_order_relaxed);
    nsync_cv_signal(&g_executor.queue_cv);
    nsync_mu_unlock(&g_executor.queue_mu);
}

static GoBlockingWork *queue_pop(void) {
    /* Caller must hold queue_mu */
    GoBlockingWork *w = g_executor.queue_head;
    if (w) {
        g_executor.queue_head = w->next;
        if (!g_executor.queue_head) {
            g_executor.queue_tail = NULL;
        }
        w->next = NULL;
        atomic_fetch_sub_explicit(&g_executor.pending_count, 1, memory_order_relaxed);
    }
    return w;
}

/* ── Worker thread ────────────────────────────────────────────────────── */

static void *executor_worker(void *arg) {
    (void)arg;
    for (;;) {
        nsync_mu_lock(&g_executor.queue_mu);

        /* Wait for work or shutdown */
        while (!g_executor.queue_head &&
               !atomic_load_explicit(&g_executor.shutting_down, memory_order_acquire)) {
            nsync_cv_wait(&g_executor.queue_cv, &g_executor.queue_mu);
        }

        /* Check shutdown after wake */
        if (atomic_load_explicit(&g_executor.shutting_down, memory_order_acquire) &&
            !g_executor.queue_head) {
            nsync_mu_unlock(&g_executor.queue_mu);
            break; /* Exit: shutting down and queue is empty */
        }

        GoBlockingWork *w = queue_pop();
        nsync_mu_unlock(&g_executor.queue_mu);

        if (!w) continue; /* Spurious wakeup */

        /* Execute the blocking work */
        atomic_fetch_add_explicit(&g_executor.active_count, 1, memory_order_relaxed);
        w->result = w->fn(w->arg);
        atomic_fetch_sub_explicit(&g_executor.active_count, 1, memory_order_relaxed);

        /* Wake the submitter */
        if (w->fiber) {
            /* Fiber caller: mark done and make runnable */
            atomic_store_explicit(&w->done, 1, memory_order_release);
            gof_hook_make_runnable(w->fiber);
        } else {
            /* Non-fiber caller: signal condition variable */
            nsync_mu_lock(&w->done_mu);
            atomic_store_explicit(&w->done, 1, memory_order_release);
            nsync_cv_signal(&w->done_cv);
            nsync_mu_unlock(&w->done_mu);
        }
    }
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────── */

int go_blocking_executor_init(size_t num_threads) {
    /* Guard against double init */
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_executor.initialized, &expected, 1)) {
        return 0; /* Already initialized */
    }

    if (num_threads == 0) num_threads = BLOCKING_EXECUTOR_DEFAULT_THREADS;
    if (num_threads > BLOCKING_EXECUTOR_MAX_THREADS) num_threads = BLOCKING_EXECUTOR_MAX_THREADS;

    g_executor.num_threads = num_threads;
    g_executor.threads = calloc(num_threads, sizeof(pthread_t));
    if (!g_executor.threads) {
        atomic_store(&g_executor.initialized, 0);
        return -1;
    }

    nsync_mu_init(&g_executor.queue_mu);
    nsync_cv_init(&g_executor.queue_cv);
    g_executor.queue_head = NULL;
    g_executor.queue_tail = NULL;
    atomic_store(&g_executor.shutting_down, 0);
    atomic_store(&g_executor.active_count, 0);
    atomic_store(&g_executor.pending_count, 0);

    for (size_t i = 0; i < num_threads; i++) {
        int rc = pthread_create(&g_executor.threads[i], NULL, executor_worker, NULL);
        if (rc != 0) {
            fprintf(stderr, "blocking_executor: failed to create thread %zu: %d\n", i, rc);
            /* Partial init: shut down what we have */
            atomic_store(&g_executor.shutting_down, 1);
            nsync_mu_lock(&g_executor.queue_mu);
            nsync_cv_broadcast(&g_executor.queue_cv);
            nsync_mu_unlock(&g_executor.queue_mu);
            for (size_t j = 0; j < i; j++) {
                pthread_join(g_executor.threads[j], NULL);
            }
            free(g_executor.threads);
            g_executor.threads = NULL;
            atomic_store(&g_executor.initialized, 0);
            return -1;
        }
    }

    return 0;
}

void *go_blocking_submit(void *(*fn)(void *arg), void *arg) {
    if (!fn) return NULL;

    /* If executor not initialized or we're not in a fiber, run synchronously */
    gof_fiber_handle fiber = gof_hook_current();
    if (!atomic_load_explicit(&g_executor.initialized, memory_order_acquire) || !fiber) {
        return fn(arg);
    }

    /* Allocate work item on heap (fiber stacks may be small).
     * NOTE: Do NOT use go_autofree here — the worker thread accesses w->result
     * after setting w->done, creating a race. We must manually free after
     * confirming the worker is done with the struct. */
    GoBlockingWork *w = calloc(1, sizeof(GoBlockingWork));
    if (!w) {
        /* Fallback: run synchronously */
        return fn(arg);
    }
    w->fn     = fn;
    w->arg    = arg;
    w->fiber  = fiber;
    w->result = NULL;
    atomic_store_explicit(&w->done, 0, memory_order_relaxed);

    /* Submit to queue */
    queue_push(w);

    /* Park the fiber — the executor worker will make us runnable when done.
     * We must loop because gof_hook_block_current() may have spurious wakes. */
    while (!atomic_load_explicit(&w->done, memory_order_acquire)) {
        gof_hook_block_current();
    }

    void *result = w->result;
    free(w);
    return result;
}

void go_blocking_executor_shutdown(void) {
    if (!atomic_load_explicit(&g_executor.initialized, memory_order_acquire)) return;

    /* Signal shutdown */
    atomic_store_explicit(&g_executor.shutting_down, 1, memory_order_release);
    nsync_mu_lock(&g_executor.queue_mu);
    nsync_cv_broadcast(&g_executor.queue_cv);
    nsync_mu_unlock(&g_executor.queue_mu);

    /* Join all worker threads */
    for (size_t i = 0; i < g_executor.num_threads; i++) {
        pthread_join(g_executor.threads[i], NULL);
    }

    /* Clean up any remaining work items (shouldn't happen if drained properly) */
    nsync_mu_lock(&g_executor.queue_mu);
    GoBlockingWork *w = g_executor.queue_head;
    while (w) {
        GoBlockingWork *next = w->next;
        /* Mark as done with NULL result to unblock any waiters */
        atomic_store_explicit(&w->done, 1, memory_order_release);
        if (w->fiber) {
            gof_hook_make_runnable(w->fiber);
        } else {
            nsync_mu_lock(&w->done_mu);
            nsync_cv_signal(&w->done_cv);
            nsync_mu_unlock(&w->done_mu);
        }
        w = next;
    }
    g_executor.queue_head = NULL;
    g_executor.queue_tail = NULL;
    nsync_mu_unlock(&g_executor.queue_mu);

    free(g_executor.threads);
    g_executor.threads = NULL;
    g_executor.num_threads = 0;
    atomic_store(&g_executor.initialized, 0);
    atomic_store(&g_executor.shutting_down, 0);
}

int go_blocking_executor_active_count(void) {
    return atomic_load_explicit(&g_executor.active_count, memory_order_relaxed);
}

int go_blocking_executor_pending_count(void) {
    return atomic_load_explicit(&g_executor.pending_count, memory_order_relaxed);
}
