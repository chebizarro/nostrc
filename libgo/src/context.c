#include "context.h"
#include <errno.h>
#include <nsync.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern GoChannel *go_channel_create(size_t capacity);
extern int go_channel_send(GoChannel *chan, void *data);
extern void go_channel_free(GoChannel *chan);

void go_context_cancel(GoContext *ctx) {
    nsync_mu_lock(&ctx->mutex);
    if (!ctx->canceled) {
        ctx->canceled = 1;
        ctx->err_msg = "context canceled";
        printf("Context canceled, signaling all waiting threads.\n");
        go_channel_send(ctx->done, NULL); // Close the done channel
        nsync_cv_broadcast(&ctx->cond);   // Signal all waiting threads
    }
    nsync_mu_unlock(&ctx->mutex);
}

// Base functions (common for all contexts)
void base_context_free(void *ctx) {
    GoContext *base_ctx = (GoContext *)ctx;
    go_channel_free(base_ctx->done);
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
    printf("Waiting for context to be canceled...\n");
    nsync_mu_lock(&base_ctx->mutex);

    while (!base_ctx->canceled) {
        nsync_cv_wait(&base_ctx->cond, &base_ctx->mutex);
    }

    printf("Context was canceled!\n");
    nsync_mu_unlock(&base_ctx->mutex);
}

// Wrapper functions to avoid direct vtable access
void go_context_free(GoContext *ctx) {
    if (ctx && ctx->vtable && ctx->vtable->free) {
        ctx->vtable->free(ctx);
    }
}

// Wrapper functions
bool go_context_is_canceled(GoContext *ctx) {
    return ctx && ctx->vtable && ctx->vtable->is_canceled ? ctx->vtable->is_canceled(ctx) : true;
}

GoChannel *go_context_done(GoContext *ctx) {
    return ctx && ctx->vtable && ctx->vtable->done ? ctx->vtable->done(ctx) : NULL;
}

Error *go_context_err(GoContext *ctx) {
    if (ctx && ctx->vtable && ctx->vtable->err) {
        char* err = ctx->vtable->err(ctx);
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
    GoContext *ctx = (GoContext *)malloc(sizeof(GoContext));
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
        go_context_cancel((GoContext *)dctx);
        dctx->base.err_msg = "context deadline exceeded";
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

        if (result == 0) {
            // Condition was signaled, check if the context is canceled
            printf("Received signal, checking context state...\n");
            if (ctx->vtable->is_canceled(ctx)) {
                printf("Context was canceled!\n");
                break;
            }
        } else if (result == ETIMEDOUT) {
            // We timed out, check if the deadline has passed
            printf("Deadline exceeded in wait cycle, checking again...\n");

            if (ctx->vtable->is_canceled(ctx)) {
                printf("Context deadline reached or context was canceled!\n");
                break;
            }
        }
    }

    printf("Context was canceled or deadline reached!\n");
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
    GoHierarchicalContext *ctx = malloc(sizeof(GoHierarchicalContext));
    hierarchical_context_init(ctx, parent);

    ctx->base.vtable = malloc(sizeof(GoContextInterface));
    ctx->base.vtable->is_canceled = hierarchical_context_is_canceled;
    ctx->base.vtable->wait = base_context_wait;
    ctx->base.vtable->free = base_context_free;
    ctx->base.vtable->done = base_context_done;
    ctx->base.vtable->err = base_context_err;

    CancelContextResult result = {(GoContext *)ctx, go_context_cancel};
    return result;
}