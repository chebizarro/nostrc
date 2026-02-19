#ifndef GO_SELECT_H_H
#define GO_SELECT_H_H

#include "channel.h"
#include <nsync.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

typedef enum {
    GO_SELECT_SEND,
    GO_SELECT_RECEIVE
} GoSelectOp;

typedef struct {
    GoSelectOp op;
    GoChannel *chan;
    void *value;     // Used for sending
    void **recv_buf; // Used for receiving
} GoSelectCase;

typedef struct {
    int selected_case;  // Index of selected case, -1 if timeout
    bool ok;            // True if operation succeeded, false if channel closed
} GoSelectResult;

/**
 * GoSelectWaiter - Heap-allocated, reference-counted waiter for select.
 *
 * LIFECYCLE (nostrc-select-refcount):
 *   - Created by go_select_waiter_create() with refcount=1.
 *   - Each channel registration (GoSelectWaiterNode) takes a ref.
 *   - go_select() holds one ref for the duration of the call.
 *   - The waiter is freed only when the last ref is released, guaranteeing
 *     the embedded nsync_mu/nsync_cv are NEVER accessed after free.
 *
 * This eliminates the previous stack-allocation hazard where nsync objects
 * could be signaled after the calling function's stack frame was destroyed
 * — a race invisible to ASAN because nsync is not instrumented.
 */
typedef struct GoSelectWaiter {
    nsync_mu mutex;              // Protects signaled flag
    nsync_cv cond;               // Signaled when any channel is ready
    _Atomic int signaled;        // Set to 1 when a channel signals us
    void *fiber_handle;          // Fiber handle for cooperative wakeup (NULL if OS thread)
    _Atomic int refcount;        // Reference count — free when it hits zero
} GoSelectWaiter;

/* Per-channel registration node for a select waiter.
 * A single GoSelectWaiter may be registered on multiple channels concurrently,
 * so list linkage must be per registration (not stored in the waiter itself).
 * Each node holds a ref on its waiter to prevent premature destruction. */
typedef struct GoSelectWaiterNode {
    GoSelectWaiter *waiter;          /* Holds a ref on the waiter */
    struct GoSelectWaiterNode *next;
} GoSelectWaiterNode;

/* Create a new heap-allocated select waiter (refcount=1).
 * Preferred over go_select_waiter_init for safe lifecycle management. */
GoSelectWaiter *go_select_waiter_create(void);

/* Increment waiter reference count. Returns w for chaining. */
GoSelectWaiter *go_select_waiter_ref(GoSelectWaiter *w);

/* Decrement waiter reference count. Frees when it hits zero. */
void go_select_waiter_unref(GoSelectWaiter *w);

/* Initialize a select waiter in-place (sets refcount=1).
 * Legacy API — prefer go_select_waiter_create() for heap allocation. */
void go_select_waiter_init(GoSelectWaiter *w);

/* Register a waiter with a channel (channel will signal when ready).
 * The registration node takes a ref on the waiter. */
void go_channel_register_select_waiter(GoChannel *chan, GoSelectWaiter *w);

/* Unregister a waiter from a channel.
 * Drops the node's ref on the waiter. */
void go_channel_unregister_select_waiter(GoChannel *chan, GoSelectWaiter *w);

/**
 * Signal all select waiters registered on a channel.
 * Called internally from channel send/recv/close paths.
 * Must be called while holding chan->mutex.
 */
void go_channel_signal_select_waiters(GoChannel *chan);

/**
 * Clean up all select waiter registrations on a channel.
 * Frees all GoSelectWaiterNode entries and unrefs their waiters.
 * Must be called while holding chan->mutex.
 * Used during channel destruction (go_channel_unref) to prevent
 * leaked nodes with dangling waiter pointers.
 */
void go_channel_cleanup_select_waiters(GoChannel *chan);

/* Standard select: blocks until one case is ready.
 * Takes refs on all valid channels for the duration of the call. */
int go_select(GoSelectCase *cases, size_t num_cases);

/* Select with timeout: returns after timeout_ms if no case is ready.
 * Takes refs on all valid channels for the duration of the call.
 * Returns: GoSelectResult with selected_case=-1 if timeout occurred */
GoSelectResult go_select_timeout(GoSelectCase *cases, size_t num_cases, uint64_t timeout_ms);

#endif // GO_SELECT_H_H
