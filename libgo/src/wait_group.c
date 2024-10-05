#include "wait_group.h"
#include <stdio.h>
#include <stdlib.h>


// Initialize the GoWaitGroup
void go_wait_group_init(GoWaitGroup *wg) {
    pthread_mutex_init(&wg->mutex, NULL);
    pthread_cond_init(&wg->cond, NULL);
    wg->counter = 0;
}

// Increment the GoWaitGroup counter
void go_wait_group_add(GoWaitGroup *wg, int delta) {
    pthread_mutex_lock(&wg->mutex);
    wg->counter += delta;
    pthread_mutex_unlock(&wg->mutex);
}

// Decrement the GoWaitGroup counter when a task is done
void go_wait_group_done(GoWaitGroup *wg) {
    pthread_mutex_lock(&wg->mutex);
    wg->counter--;
    if (wg->counter == 0) {
        pthread_cond_broadcast(&wg->cond);  // Signal all waiting threads
    }
    pthread_mutex_unlock(&wg->mutex);
}

// Wait for all tasks to finish (counter becomes 0)
void go_wait_group_wait(GoWaitGroup *wg) {
    pthread_mutex_lock(&wg->mutex);
    while (wg->counter > 0) {
        pthread_cond_wait(&wg->cond, &wg->mutex);
    }
    pthread_mutex_unlock(&wg->mutex);
}

// Destroy the GoWaitGroup (cleanup)
void go_wait_group_destroy(GoWaitGroup *wg) {
    pthread_mutex_destroy(&wg->mutex);
    pthread_cond_destroy(&wg->cond);
}
