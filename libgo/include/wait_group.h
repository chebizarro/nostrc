#ifndef GO_WAIT_GROUP_H
#define GO_WAIT_GROUP_H

#include "libgo-export.h"

#include <nsync.h>

typedef struct {
    nsync_mu mutex; // Mutex to protect the counter
    nsync_cv cond;  // Condition variable for signaling
    _Atomic int counter;    // Counter for tracking tasks (atomic to appease TSAN)
} GoWaitGroup;

// Public API
LIBGO_API void go_wait_group_init(GoWaitGroup *wg);
LIBGO_API void go_wait_group_add(GoWaitGroup *wg, int delta);
LIBGO_API void go_wait_group_done(GoWaitGroup *wg);
LIBGO_API void go_wait_group_wait(GoWaitGroup *wg);
LIBGO_API void go_wait_group_destroy(GoWaitGroup *wg);

#endif // GO_WAIT_GROUP_H
