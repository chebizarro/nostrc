#ifndef CONTEXT_H
#define CONTEXT_H

#include <nsync.h>
#include <time.h>

typedef struct GoContextInterface {
    void (*init)(void *ctx, ...);
    int (*is_canceled)(void *ctx);
    void (*wait)(void *ctx);
    void (*free)(void *ctx);
} GoContextInterface;

typedef struct GoContext {
    GoContextInterface *vtable;
    nsync_mu mutex;
    nsync_cv cond;
    int canceled;
    struct timespec timeout;
} GoContext;

void go_context_init(GoContext *ctx, int timeout_seconds);
int go_context_is_canceled(GoContext *ctx);
void go_context_wait(GoContext *ctx);
void go_context_free(GoContext *ctx);

typedef struct {
    GoContext base;
    struct timespec deadline;
} GoDeadlineContext;

void go_deadline_context_init(GoDeadlineContext *ctx, struct timespec deadline);
int go_deadline_context_is_canceled(GoDeadlineContext *ctx);

typedef struct {
    GoContext base;
    char **keys;
    char **values;
    int kv_count;
} GoValueContext;

void go_value_context_init(GoValueContext *ctx, int timeout_seconds, char **keys, char **values, int kv_count);
char *go_value_context_get_value(GoValueContext *ctx, const char *key);

typedef struct {
    GoContext base;
    GoContext *parent;
} GoHierarchicalContext;

void go_hierarchical_context_init(GoHierarchicalContext *ctx, GoContext *parent, int timeout_seconds);
int go_hierarchical_context_is_canceled(GoHierarchicalContext *ctx);

#endif // CONTEXT_H
