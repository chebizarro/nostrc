#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include "context.h"
#include "channel.h"
#include "nostr/metrics.h"
#include <errno.h>
#include <nsync.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void go_context_cancel(GoContext *ctx) {
    nostr_metric_counter_add("go_ctx_cancel_invocations", 1);
    nsync_mu_lock(&ctx->mutex);
    if (atomic_load_explicit(&ctx->canceled, memory_order_acquire) == 0) {
        atomic_store_explicit(&ctx->canceled, 1, memory_order_release);
        atomic_store_explicit(&ctx->err_msg, "context canceled", memory_order_release);
        // Wake all waiters; avoid any blocking operations here
        nsync_cv_broadcast(&ctx->cond);
        nostr_metric_counter_add("go_ctx_cancel_broadcasts", 1);
        // Also make ctx->done observable by non-blocking selects
        // Best-effort: try to enqueue a single token, then close to wake any waiters
        if (ctx->done) {
            int rc = go_channel_try_send(ctx->done, (void *)ctx); // token value unused
            if (rc == 0) {
                nostr_metric_counter_add("go_ctx_done_enqueued", 1);
            }
            go_channel_close(ctx->done);
        }
    }
    nsync_mu_unlock(&ctx->mutex);
}

// Reference counting for safe context lifecycle
GoContext *go_context_ref(GoContext *ctx) {
    if (!ctx) return NULL;
    atomic_fetch_add_explicit(&ctx->refcount, 1, memory_order_relaxed);
    return ctx;
}

void go_context_unref(GoContext *ctx) {
    if (!ctx) return;
    int old = atomic_fetch_sub_explicit(&ctx->refcount, 1, memory_order_acq_rel);
    if (old == 1) {
        // Refcount hit zero, actually free the context
        go_context_free(ctx);
    }
}

// Base functions (common for all contexts)
void base_context_free(void *ctx) {
    GoContext *base_ctx = (GoContext *)ctx;
    if (base_ctx->done) {
        go_channel_close(base_ctx->done);
        go_channel_free(base_ctx->done);
        base_ctx->done = NULL;
    }
    free(base_ctx->vtable);
    free(base_ctx);
}

GoChannel *base_context_done(void *ctx) {
    GoContext *base_ctx = (GoContext *)ctx;
    return base_ctx->done;
}

const char *base_context_err(void *ctx) {
    GoContext *base_ctx = (GoContext *)ctx;
    return atomic_load_explicit(&base_ctx->err_msg, memory_order_acquire);
}

bool base_context_is_canceled(void *ctx) {
    GoContext *base_ctx = (GoContext *)ctx;
    return atomic_load_explicit(&base_ctx->canceled, memory_order_acquire) != 0;
}

void base_context_wait(void *ctx) {
    GoContext *base_ctx = (GoContext *)ctx;
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    nsync_mu_lock(&base_ctx->mutex);

    while (!base_ctx->canceled) {
        if (atomic_load_explicit(&base_ctx->canceled, memory_order_acquire)) break;
        if (!blocked) { nostr_metric_counter_add("go_ctx_block_waits", 1); blocked = 1; }
        nsync_cv_wait(&base_ctx->cond, &base_ctx->mutex);
        nostr_metric_counter_add("go_ctx_wait_wakeups", 1);
    }
    nsync_mu_unlock(&base_ctx->mutex);
    nostr_metric_timer_stop(&t, nostr_metric_histogram_get("go_ctx_wait_ns"));
}

// Wrapper functions to avoid direct vtable access
void go_context_free(GoContext *ctx) {
    if (ctx && ctx->vtable && ctx->vtable->free) {
        ctx->vtable->free(ctx);
    }
}

// Wrapper functions
int go_context_is_canceled(const void *ctxp) {
    GoContext *ctx = (GoContext *)ctxp;
    return ctx && ctx->vtable && ctx->vtable->is_canceled ? ctx->vtable->is_canceled(ctx) : true;
}

GoChannel *go_context_done(GoContext *ctx) {
    return ctx && ctx->vtable && ctx->vtable->done ? ctx->vtable->done(ctx) : NULL;
}

Error *go_context_err(GoContext *ctx) {
    if (ctx && ctx->vtable && ctx->vtable->err) {
        const char *err = ctx->vtable->err(ctx);
        if (err)
            return new_error(-1, err);
    }
    return NULL;
}

void go_context_wait(GoContext *ctx) {
    if (ctx && ctx->vtable && ctx->vtable->wait) {
        ctx->vtable->wait(ctx);
    }
}

// Background context
GoContext *go_context_background(void) {
    GoContext *ctx = (GoContext *)calloc(1, sizeof(GoContext));
    nsync_mu_init(&ctx->mutex);
    nsync_cv_init(&ctx->cond);
    ctx->done = go_channel_create(1);
    atomic_store_explicit(&ctx->canceled, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx->err_msg, NULL, memory_order_relaxed);
    atomic_store_explicit(&ctx->refcount, 1, memory_order_relaxed);
    return ctx;
}

// Initialization function for base contexts
void go_context_init(GoContext *ctx, int timeout_seconds) {
    (void)timeout_seconds; // unused in current implementation
    nsync_mu_init(&ctx->mutex);
    nsync_cv_init(&ctx->cond);
    ctx->done = go_channel_create(1);
    atomic_store_explicit(&ctx->canceled, 0, memory_order_relaxed);
    atomic_store_explicit(&ctx->err_msg, NULL, memory_order_relaxed);
    atomic_store_explicit(&ctx->refcount, 1, memory_order_relaxed);
}

// Deadline context functions
void deadline_context_init(void *ctx, struct timespec deadline) {
    GoDeadlineContext *dctx = (GoDeadlineContext *)ctx;
    go_context_init((GoContext *)dctx, 0);
    dctx->deadline = deadline;
}

bool deadline_context_is_canceled(void *ctx) {
    GoDeadlineContext *dctx = (GoDeadlineContext *)ctx;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    if (now.tv_sec > dctx->deadline.tv_sec ||
        (now.tv_sec == dctx->deadline.tv_sec && now.tv_nsec > dctx->deadline.tv_nsec)) {
        // Pure predicate: indicate cancellation due to deadline
        return true;
    }

    return atomic_load_explicit(&dctx->base.canceled, memory_order_acquire) != 0;
}

void deadline_context_wait(void *dctx) {
    GoContext *ctx = (GoContext *)dctx;
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    nsync_mu_lock(&ctx->mutex);

    while (atomic_load_explicit(&ctx->canceled, memory_order_acquire) == 0) {
        // Set an absolute deadline 1 second into the future for each wait cycle
        nsync_time abs_deadline = nsync_time_add(nsync_time_now(), nsync_time_ms(1000));

        // Wait with deadline, checking the result
        if (!blocked) { nostr_metric_counter_add("go_ctx_block_waits", 1); blocked = 1; }
        int result = nsync_cv_wait_with_deadline(&ctx->cond, &ctx->mutex, abs_deadline, NULL);
        nostr_metric_counter_add("go_ctx_wait_wakeups", 1);

        if (result == 0 || result == ETIMEDOUT) {
            // Check cancel/deadline and mark canceled if needed
            if (atomic_load_explicit(&ctx->canceled, memory_order_acquire) == 0) {
                GoDeadlineContext *dctx = (GoDeadlineContext *)ctx;
                // Check if deadline reached
                if (/* deadline exceeded */ 1) {
                    // conservative: use timespec in dctx during signal path
                    struct timespec now;
                    clock_gettime(CLOCK_REALTIME, &now);
                    if (now.tv_sec > dctx->deadline.tv_sec ||
                        (now.tv_sec == dctx->deadline.tv_sec && now.tv_nsec > dctx->deadline.tv_nsec)) {
                        atomic_store_explicit(&ctx->canceled, 1, memory_order_release);
                        const char *expected = NULL;
                        (void)expected;
                        if (atomic_load_explicit(&ctx->err_msg, memory_order_acquire) == NULL)
                            atomic_store_explicit(&ctx->err_msg, "context deadline exceeded", memory_order_release);
                        nsync_cv_broadcast(&ctx->cond);
                        nostr_metric_counter_add("go_ctx_deadline_broadcasts", 1);
                    }
                    break;
                }
            }
        }
    }
    nsync_mu_unlock(&ctx->mutex);
    nostr_metric_timer_stop(&t, nostr_metric_histogram_get("go_ctx_wait_ns"));
}

GoContext *go_with_deadline(GoContext *parent, struct timespec deadline) {
    (void)parent; // not used in this implementation
    GoDeadlineContext *ctx = malloc(sizeof(GoDeadlineContext));
    go_context_init(&ctx->base, 0);

    ctx->deadline = deadline;

    ctx->base.vtable = malloc(sizeof(GoContextInterface));
    ctx->base.vtable->is_canceled = deadline_context_is_canceled;
    ctx->base.vtable->wait = deadline_context_wait;
    ctx->base.vtable->free = base_context_free;
    ctx->base.vtable->done = base_context_done;
    ctx->base.vtable->err = base_context_err;

    return (GoContext *)ctx;
}

void hierarchical_context_init(GoHierarchicalContext *ctx, GoContext *parent) {
    go_context_init(&ctx->base, 0);
    ctx->parent = parent;
}

bool hierarchical_context_is_canceled(void *ctx) {
    GoHierarchicalContext *hctx = (GoHierarchicalContext *)ctx;
    return base_context_is_canceled(ctx) || (hctx->parent && base_context_is_canceled(hctx->parent));
}

CancelContextResult go_context_with_cancel(GoContext *parent) {
    GoHierarchicalContext *ctx = calloc(1, sizeof(GoHierarchicalContext));
    hierarchical_context_init(ctx, parent);

    ctx->base.vtable = calloc(1, sizeof(GoContextInterface));
    ctx->base.vtable->is_canceled = hierarchical_context_is_canceled;
    ctx->base.vtable->wait = base_context_wait;
    ctx->base.vtable->free = base_context_free;
    ctx->base.vtable->done = base_context_done;
    ctx->base.vtable->err = base_context_err;

    CancelContextResult result = {(GoContext *)ctx, go_context_cancel};
    return result;
}
