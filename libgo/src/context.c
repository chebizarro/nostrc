#include "context.h"
#include "channel.h"
#include <errno.h>
#include <nsync.h>
#include <stdlib.h>
#include <string.h>

void go_context_cancel(GoContext *ctx) {
    nsync_mu_lock(&ctx->mutex);
    if (!ctx->canceled) {
        ctx->canceled = 1;
        ctx->err_msg = "context canceled";
        // Wake all waiters; avoid any blocking operations here
        nsync_cv_broadcast(&ctx->cond);
    }
    nsync_mu_unlock(&ctx->mutex);
}

// Base functions (common for all contexts)
void base_context_free(void *ctx) {
    GoContext *base_ctx = (GoContext *)ctx;
    go_channel_close(base_ctx->done);
    free(base_ctx->vtable);
    free(base_ctx);
}

GoChannel *base_context_done(void *ctx) {
    GoContext *base_ctx = (GoContext *)ctx;
    return base_ctx->done;
}

const char *base_context_err(void *ctx) {
    GoContext *base_ctx = (GoContext *)ctx;
    return base_ctx->err_msg;
}

bool base_context_is_canceled(void *ctx) {
    GoContext *base_ctx = (GoContext *)ctx;
    return base_ctx->canceled;
}

void base_context_wait(void *ctx) {
    GoContext *base_ctx = (GoContext *)ctx;
    nsync_mu_lock(&base_ctx->mutex);

    while (!base_ctx->canceled) {
        nsync_cv_wait(&base_ctx->cond, &base_ctx->mutex);
    }
    nsync_mu_unlock(&base_ctx->mutex);
}

// Wrapper functions to avoid direct vtable access
void go_context_free(GoContext *ctx) {
    if (ctx && ctx->vtable && ctx->vtable->free) {
        ctx->vtable->free(ctx);
    }
}

// Wrapper functions
int go_context_is_canceled(const void *ctxp) {
    GoContext *ctx = (GoContext*)ctxp;
    return ctx && ctx->vtable && ctx->vtable->is_canceled ? ctx->vtable->is_canceled(ctx) : true;
}

GoChannel *go_context_done(GoContext *ctx) {
    return ctx && ctx->vtable && ctx->vtable->done ? ctx->vtable->done(ctx) : NULL;
}

Error *go_context_err(GoContext *ctx) {
    if (ctx && ctx->vtable && ctx->vtable->err) {
        const char* err = ctx->vtable->err(ctx);
        if (err) return new_error(-1, err);
    }
    return NULL;
}


void go_context_wait(GoContext *ctx) {
    if (ctx && ctx->vtable && ctx->vtable->wait) {
        ctx->vtable->wait(ctx);
    }
}

// Background context
GoContext *go_context_background() {
    GoContext *ctx = (GoContext *)calloc(1, sizeof(GoContext));
    nsync_mu_init(&ctx->mutex);
    nsync_cv_init(&ctx->cond);
    ctx->done = go_channel_create(1);
    ctx->canceled = false;
    ctx->err_msg = NULL;
    return ctx;
}

// Initialization function for base contexts
void go_context_init(GoContext *ctx, int timeout_seconds) {
    nsync_mu_init(&ctx->mutex);
    nsync_cv_init(&ctx->cond);
    ctx->done = go_channel_create(1);
    ctx->canceled = false;
    ctx->err_msg = NULL;
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

    return dctx->base.canceled;
}

void deadline_context_wait(void *dctx) {
    GoContext *ctx = (GoContext*)dctx;
    nsync_mu_lock(&ctx->mutex);

    while (!ctx->canceled) {
        // Set an absolute deadline 1 second into the future for each wait cycle
        nsync_time abs_deadline = nsync_time_add(nsync_time_now(), nsync_time_ms(1000));

        // Wait with deadline, checking the result
        int result = nsync_cv_wait_with_deadline(&ctx->cond, &ctx->mutex, abs_deadline, NULL);

        if (result == 0 || result == ETIMEDOUT) {
            // Check cancel/deadline and mark canceled if needed
            if (ctx->vtable->is_canceled(ctx)) {
                if (!ctx->canceled) {
                    ctx->canceled = true;
                    if (!ctx->err_msg) ctx->err_msg = "context deadline exceeded";
                    nsync_cv_broadcast(&ctx->cond);
                }
                break;
            }
        }
    }
    nsync_mu_unlock(&ctx->mutex);
}

GoContext *go_with_deadline(GoContext *parent, struct timespec deadline) {
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