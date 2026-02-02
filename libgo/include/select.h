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
 * GoSelectWaiter - A waiter that can be registered with multiple channels.
 *
 * When any registered channel becomes ready (has data or space), it signals
 * the waiter's condition variable, waking up the select.
 */
typedef struct GoSelectWaiter {
    nsync_mu mutex;              // Protects signaled flag
    nsync_cv cond;               // Signaled when any channel is ready
    _Atomic int signaled;        // Set to 1 when a channel signals us
    struct GoSelectWaiter *next; // Intrusive linked list for channel's waiter list
} GoSelectWaiter;

/* Initialize a select waiter (call before registering with channels) */
void go_select_waiter_init(GoSelectWaiter *w);

/* Register a waiter with a channel (channel will signal when ready) */
void go_channel_register_select_waiter(GoChannel *chan, GoSelectWaiter *w);

/* Unregister a waiter from a channel */
void go_channel_unregister_select_waiter(GoChannel *chan, GoSelectWaiter *w);

/* Standard select: blocks until one case is ready */
int go_select(GoSelectCase *cases, size_t num_cases);

/* Select with timeout: returns after timeout_ms if no case is ready
 * Returns: GoSelectResult with selected_case=-1 if timeout occurred */
GoSelectResult go_select_timeout(GoSelectCase *cases, size_t num_cases, uint64_t timeout_ms);

#endif // GO_SELECT_H_H
