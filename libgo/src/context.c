#include "context.h"
#include <nsync.h>
#include <stdlib.h>
#include <string.h>

extern GoChannel *go_channel_create(size_t capacity);
extern int go_channel_send(GoChannel *chan, void *data);
extern void go_channel_free(GoChannel *chan);

void go_context_init(GoContext *ctx, int timeout_seconds) {
    nsync_mu_init(&ctx->mutex);
    nsync_cv_init(&ctx->cond);
    ctx->done = go_channel_create(1); // Initialize done channel
    ctx->canceled = 0;
    ctx->err_msg = NULL;
    clock_gettime(CLOCK_REALTIME, &ctx->timeout);
    ctx->timeout.tv_sec += timeout_seconds;
}

int go_context_is_canceled(GoContext *ctx) {
    nsync_mu_lock(&ctx->mutex);
    int result = ctx->canceled;
    nsync_mu_unlock(&ctx->mutex);
    return result;
}

GoChannel *go_context_done(GoContext *ctx) {
    return ctx->done; // Return the channel signaling cancellation
}

const char *go_context_err(GoContext *ctx) {
    nsync_mu_lock(&ctx->mutex);
    const char *err = ctx->err_msg;
    nsync_mu_unlock(&ctx->mutex);
    return err; // Return the error message
}

void go_context_cancel(GoContext *ctx, const char *err_msg) {
    nsync_mu_lock(&ctx->mutex);
    ctx->canceled = 1;
    ctx->err_msg = err_msg;
    go_channel_send(ctx->done, NULL); // Close the done channel
    nsync_cv_broadcast(&ctx->cond);   // Signal all waiting threads
    nsync_mu_unlock(&ctx->mutex);
}

void go_context_wait(GoContext *ctx) {
    nsync_mu_lock(&ctx->mutex);
    while (!ctx->canceled) {
        nsync_cv_wait(&ctx->cond, &ctx->mutex);
    }
    nsync_mu_unlock(&ctx->mutex);
}

void go_context_free(GoContext *ctx) {
    go_channel_free(ctx->done);
    // No need to destroy the mutex or cond, nsync handles it
}

void go_deadline_context_init(GoDeadlineContext *ctx, struct timespec deadline) {
    go_context_init((GoContext *)ctx, 0); // Initialize base context
    ctx->deadline = deadline;
}

int go_deadline_context_is_canceled(GoDeadlineContext *ctx) {
    if (go_context_is_canceled((GoContext *)ctx)) {
        return 1;
    }
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec > ctx->deadline.tv_sec ||
        (now.tv_sec == ctx->deadline.tv_sec && now.tv_nsec > ctx->deadline.tv_nsec)) {
        go_context_cancel((GoContext *)ctx, "context deadline exceeded");
        return 1;
    }
    return 0;
}

void go_value_context_init(GoValueContext *ctx, int timeout_seconds, char **keys, char **values, int kv_count) {
    go_context_init((GoContext *)ctx, timeout_seconds); // Initialize base context
    ctx->keys = keys;
    ctx->values = values;
    ctx->kv_count = kv_count;
}

char *go_value_context_get_value(GoValueContext *ctx, const char *key) {
    for (int i = 0; i < ctx->kv_count; ++i) {
        if (strcmp(ctx->keys[i], key) == 0) {
            return ctx->values[i];
        }
    }
    return NULL;
}

void go_hierarchical_context_init(GoHierarchicalContext *ctx, GoContext *parent, int timeout_seconds) {
    go_context_init((GoContext *)ctx, timeout_seconds); // Initialize base context
    ctx->parent = parent;
}

int go_hierarchical_context_is_canceled(GoHierarchicalContext *ctx) {
    return go_context_is_canceled((GoContext *)ctx) ||
           (ctx->parent && go_context_is_canceled(ctx->parent));
}

GoContext *go_with_cancel(GoContext *parent) {
    GoHierarchicalContext *ctx = malloc(sizeof(GoHierarchicalContext));
    go_hierarchical_context_init(ctx, parent, 0);
    return (GoContext *)ctx;
}

GoContext *go_with_deadline(GoContext *parent, struct timespec deadline) {
    GoDeadlineContext *ctx = malloc(sizeof(GoDeadlineContext));
    go_deadline_context_init(ctx, deadline);
    return (GoContext *)ctx;
}

GoContext *go_with_value(GoContext *parent, char **keys, char **values, int kv_count) {
    GoValueContext *ctx = malloc(sizeof(GoValueContext));
    go_value_context_init(ctx, 0, keys, values, kv_count);
    return (GoContext *)ctx;
}
