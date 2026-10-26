#ifndef CONTEXT_H
#define CONTEXT_H

#include "libgo-export.h"

#include <time.h>
#include <nsync.h>
#include <stdbool.h>
#include <stdatomic.h>
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
    _Atomic int canceled;           // 0/1 flag, atomic to avoid TSAN races
    _Atomic(const char *) err_msg;  // atomic pointer for racy reads
    _Atomic int refcount;           // reference count for safe deferred free
    struct timespec timeout;
} GoContext;

/* A new root context (refcount 1). Release it with go_context_unref() once
 * no context derived from it (go_context_with_cancel) is still in use: a
 * child reads its parent. */
LIBGO_API GoContext *go_context_background(void);

// GoContext wrapper functions
LIBGO_API void go_context_init(GoContext *ctx, int timeout_seconds);
LIBGO_API int go_context_is_canceled(const void *ctx);  // Wrapper for vtable->is_canceled
LIBGO_API void go_context_wait(GoContext *ctx);         // Wrapper for vtable->wait
LIBGO_API void go_context_free(GoContext *ctx);         // Wrapper for vtable->free (prefer unref)
LIBGO_API GoChannel *go_context_done(GoContext *ctx);   // Wrapper for vtable->done
LIBGO_API Error *go_context_err(GoContext *ctx);   // Wrapper for vtable->err

// Reference counting for safe context lifecycle management
LIBGO_API GoContext *go_context_ref(GoContext *ctx);    // Increment refcount, returns ctx
LIBGO_API void go_context_unref(GoContext *ctx);        // Decrement refcount, free when 0

// Deadline context
typedef struct {
    GoContext base;
    struct timespec deadline;
} GoDeadlineContext;

LIBGO_API GoContext *go_with_deadline(GoContext *parent, struct timespec deadline);

// Value context
typedef struct {
    GoContext base;
    char **keys;
    char **values;
    int kv_count;
} GoValueContext;

LIBGO_API char *go_value_context_get_value(GoValueContext *ctx, const char *key);

// Hierarchical context
typedef struct {
    GoContext base;
    GoContext *parent;
} GoHierarchicalContext;

LIBGO_API bool go_hierarchical_context_is_canceled(GoHierarchicalContext *ctx);

typedef void (*CancelFunc)(GoContext *ctx);

typedef struct {
    GoContext *context;
    CancelFunc cancel;
} CancelContextResult;

LIBGO_API CancelContextResult go_context_with_cancel(GoContext *parent);

#endif // CONTEXT_H
