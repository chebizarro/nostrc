#include "wait_group.h"
#include <stdio.h>
#include <stdlib.h>
#include <nsync.h>

// Initialize the GoWaitGroup
void go_wait_group_init(GoWaitGroup *wg) {
    nsync_mu_init(&wg->mutex);  // Initialize nsync mutex
    nsync_cv_init(&wg->cond);   // Initialize nsync condition variable
    wg->counter = 0;
}

// Increment the GoWaitGroup counter
void go_wait_group_add(GoWaitGroup *wg, int delta) {
    nsync_mu_lock(&wg->mutex);   // Lock the mutex
    wg->counter += delta;
    nsync_mu_unlock(&wg->mutex); // Unlock the mutex
}

// Decrement the GoWaitGroup counter when a task is done
void go_wait_group_done(GoWaitGroup *wg) {
    nsync_mu_lock(&wg->mutex);   // Lock the mutex
    wg->counter--;
    if (wg->counter == 0) {
        nsync_cv_broadcast(&wg->cond);  // Signal all waiting threads
    }
    nsync_mu_unlock(&wg->mutex); // Unlock the mutex
}

// Wait for all tasks to finish (counter becomes 0)
void go_wait_group_wait(GoWaitGroup *wg) {
    nsync_mu_lock(&wg->mutex);   // Lock the mutex
    while (wg->counter > 0) {
        nsync_cv_wait(&wg->cond, &wg->mutex);  // Wait until the counter is 0
    }
    nsync_mu_unlock(&wg->mutex); // Unlock the mutex
}

// Destroy the GoWaitGroup (cleanup) - No explicit cleanup required for nsync
void go_wait_group_destroy(GoWaitGroup *wg) {
    // nsync doesn't require explicit destruction of mutex or condition variables
}
