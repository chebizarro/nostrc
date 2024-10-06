#ifndef GO_WAIT_GROUP_H
#define GO_WAIT_GROUP_H

#include <nsync.h>

typedef struct {
    nsync_mu mutex;    // Mutex to protect the counter
    nsync_cv cond;     // Condition variable for signaling
    int counter;       // Counter for tracking tasks
} GoWaitGroup;


#endif // GO_WAIT_GROUP_H