#include "context.h"
#include <nsync.h>
#include <stdlib.h>
#include <string.h>

void go_context_init(GoContext *ctx, int timeout_seconds) {
    nsync_mu_init(&ctx->mutex); // Initialize nsync mutex
    nsync_cv_init(&ctx->cond);  // Initialize nsync condition variable
    ctx->canceled = 0;
    clock_gettime(CLOCK_REALTIME, &ctx->timeout);
    ctx->timeout.tv_sec += timeout_seconds;
}

int go_context_is_canceled(GoContext *ctx) {
    nsync_mu_lock(&ctx->mutex); // Lock the mutex
    int result = ctx->canceled;
    nsync_mu_unlock(&ctx->mutex); // Unlock the mutex
    return result;
}

void go_context_wait(GoContext *ctx) {
    nsync_mu_lock(&ctx->mutex); // Lock the mutex
    while (!ctx->canceled) {
        nsync_cv_wait(&ctx->cond, &ctx->mutex); // Wait until canceled
    }
    nsync_mu_unlock(&ctx->mutex); // Unlock the mutex
}

void go_context_free(GoContext *ctx) {
    // No need for explicit mutex or cond destruction, nsync manages its own resources
}

void go_deadline_context_init(GoDeadlineContext *ctx, struct timespec deadline) {
    go_context_init((GoContext *)ctx, 0); // Initialize as a regular context
    ctx->deadline = deadline;             // Set deadline
}

int go_deadline_context_is_canceled(GoDeadlineContext *ctx) {
    if (go_context_is_canceled((GoContext *)ctx)) {
        return 1;
    }
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (now.tv_sec > ctx->deadline.tv_sec ||
            (now.tv_sec == ctx->deadline.tv_sec && now.tv_nsec > ctx->deadline.tv_nsec));
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
