#ifndef GO_SELECT_H_H
#define GO_SELECT_H_H

#include "channel.h"
#include <stdint.h>
#include <stdbool.h>

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

/* Standard select: blocks until one case is ready */
int go_select(GoSelectCase *cases, size_t num_cases);

/* Select with timeout: returns after timeout_ms if no case is ready 
 * Returns: GoSelectResult with selected_case=-1 if timeout occurred */
GoSelectResult go_select_timeout(GoSelectCase *cases, size_t num_cases, uint64_t timeout_ms);

#endif // GO_SELECT_H_H
