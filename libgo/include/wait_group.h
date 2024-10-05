#ifndef GO_WAIT_GROUP_H
#define GO_WAIT_GROUP_H

#include <pthread.h>

typedef struct {
    pthread_mutex_t mutex;    // Mutex to protect the counter
    pthread_cond_t cond;      // Condition variable for signaling
    int counter;              // Counter for tracking tasks
} GoWaitGroup;


#endif // GO_WAIT_GROUP_H