#include "context.h"
#include <stdlib.h>
#include <string.h>

void go_context_init(GoContext *ctx, int timeout_seconds) {
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    ctx->canceled = 0;
    clock_gettime(CLOCK_REALTIME, &ctx->timeout);
    ctx->timeout.tv_sec += timeout_seconds;
}

int go_context_is_canceled(GoContext *ctx) {
    pthread_mutex_lock(&ctx->mutex);
    int result = ctx->canceled;
    pthread_mutex_unlock(&ctx->mutex);
    return result;
}

void go_context_wait(GoContext *ctx) {
    pthread_mutex_lock(&ctx->mutex);
    while (!ctx->canceled) {
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    pthread_mutex_unlock(&ctx->mutex);
}

void go_context_free(GoContext *ctx) {
    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->cond);
}

void go_deadline_context_init(GoDeadlineContext *ctx, struct timespec deadline) {
    go_context_init((GoContext *)ctx, 0);
    ctx->deadline = deadline;
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
    go_context_init((GoContext *)ctx, timeout_seconds);
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
    go_context_init((GoContext *)ctx, timeout_seconds);
    ctx->parent = parent;
}

int go_hierarchical_context_is_canceled(GoHierarchicalContext *ctx) {
    return go_context_is_canceled((GoContext *)ctx) ||
           (ctx->parent && go_context_is_canceled(ctx->parent));
}
