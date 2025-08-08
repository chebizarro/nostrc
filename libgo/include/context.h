#ifndef CONTEXT_H
#define CONTEXT_H

#include <time.h>
#include <nsync.h>
#include <stdbool.h>
#include "error.h"

typedef struct GoChannel GoChannel;

typedef struct GoContextInterface {
    void (*init)(void *ctx, ...);
    bool (*is_canceled)(void *ctx);
    void (*wait)(void *ctx);
    void (*free)(void *ctx);
    GoChannel *(*done)(void *ctx);
    const char *(*err)(void *ctx);
} GoContextInterface;

typedef struct GoContext {
    GoContextInterface *vtable;
    nsync_mu mutex;
    nsync_cv cond;
    GoChannel *done;
    bool canceled;
    const char *err_msg;
    struct timespec timeout;
} GoContext;

GoContext *go_context_background(void);

// GoContext wrapper functions
void go_context_init(GoContext *ctx, int timeout_seconds);
int go_context_is_canceled(const void *ctx);  // Wrapper for vtable->is_canceled
void go_context_wait(GoContext *ctx);         // Wrapper for vtable->wait
void go_context_free(GoContext *ctx);         // Wrapper for vtable->free
GoChannel *go_context_done(GoContext *ctx);   // Wrapper for vtable->done
Error *go_context_err(GoContext *ctx);   // Wrapper for vtable->err

// Deadline context
typedef struct {
    GoContext base;
    struct timespec deadline;
} GoDeadlineContext;

GoContext *go_with_deadline(GoContext *parent, struct timespec deadline);

// Value context
typedef struct {
    GoContext base;
    char **keys;
    char **values;
    int kv_count;
} GoValueContext;

char *go_value_context_get_value(GoValueContext *ctx, const char *key);

// Hierarchical context
typedef struct {
    GoContext base;
    GoContext *parent;
} GoHierarchicalContext;

bool go_hierarchical_context_is_canceled(GoHierarchicalContext *ctx);

typedef void (*CancelFunc)(GoContext *ctx);

typedef struct {
    GoContext *context;
    CancelFunc cancel;
} CancelContextResult;

CancelContextResult go_context_with_cancel(GoContext *parent);

#endif // CONTEXT_H
