#ifndef GO_SELECT_H_H
#define GO_SELECT_H_H

#include "channel.h"

typedef enum {
    GO_SELECT_SEND,
    GO_SELECT_RECEIVE
} GoSelectOp;

typedef struct {
    GoSelectOp op;
    GoChannel *chan;
    void *value;  // Used for sending
    void **recv_buf;  // Used for receiving
} GoSelectCase;

int go_select(GoSelectCase *cases, size_t num_cases);

#endif // GO_SELECT_H_H
