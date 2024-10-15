#ifndef CONTEXT_H
#define CONTEXT_H

#include <time.h>
#include <nsync.h>
#include <time.h>
#include <stdbool.h>

typedef struct GoChannel GoChannel;

typedef struct GoContextInterface {
    void (*init)(void *ctx, ...);
    int (*is_canceled)(void *ctx);
    void (*wait)(void *ctx);
    void (*free)(void *ctx);
    GoChannel *(*done)(void *ctx); // Done function returning a GoChannel
    const char *(*err)(void *ctx); // Err function returning error message
} GoContextInterface;

typedef struct GoContext {
    GoContextInterface *vtable;
    nsync_mu mutex;
    nsync_cv cond;
    GoChannel *done; // Channel signaling context cancellation
    int canceled;
    const char *err_msg; // Store error message
    struct timespec timeout;
} GoContext;

GoContext *go_context_background();

// GoContext functions
void go_context_init(GoContext *ctx, int timeout_seconds);
bool go_context_is_canceled(GoContext *ctx);
void go_context_wait(GoContext *ctx);
void go_context_free(GoContext *ctx);
GoChannel *go_context_done(GoContext *ctx); // Done function
const char *go_context_err(GoContext *ctx); // Err function

// Deadline context
typedef struct {
    GoContext base;
    struct timespec deadline;
} GoDeadlineContext;

void go_deadline_context_init(GoDeadlineContext *ctx, struct timespec deadline);
int go_deadline_context_is_canceled(GoDeadlineContext *ctx);
GoContext *go_with_deadline(GoContext *parent, struct timespec deadline);

// Value context
typedef struct {
    GoContext base;
    char **keys;
    char **values;
    int kv_count;
} GoValueContext;

void go_value_context_init(GoValueContext *ctx, int timeout_seconds, char **keys, char **values, int kv_count);
char *go_value_context_get_value(GoValueContext *ctx, const char *key);

// Hierarchical context
typedef struct {
    GoContext base;
    GoContext *parent;
} GoHierarchicalContext;

void go_hierarchical_context_init(GoHierarchicalContext *ctx, GoContext *parent, int timeout_seconds);
int go_hierarchical_context_is_canceled(GoHierarchicalContext *ctx);

typedef void (*CancelFunc)(GoContext *ctx);

typedef struct {
    GoContext *context;
    CancelFunc cancel;
} CancelContextResult;

CancelContextResult go_context_with_cancel(GoContext *parent);

#endif // CONTEXT_H
