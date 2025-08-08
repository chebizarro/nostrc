#ifndef GO_WAIT_GROUP_H
#define GO_WAIT_GROUP_H

#include <nsync.h>

typedef struct {
    nsync_mu mutex; // Mutex to protect the counter
    nsync_cv cond;  // Condition variable for signaling
    int counter;    // Counter for tracking tasks
} GoWaitGroup;

// Public API
void go_wait_group_init(GoWaitGroup *wg);
void go_wait_group_add(GoWaitGroup *wg, int delta);
void go_wait_group_done(GoWaitGroup *wg);
void go_wait_group_wait(GoWaitGroup *wg);
void go_wait_group_destroy(GoWaitGroup *wg);

#endif // GO_WAIT_GROUP_H
