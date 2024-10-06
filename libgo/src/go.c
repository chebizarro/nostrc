#include "go.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <nsync.h>


// Wrapper function to create a new thread
int go(void *(*start_routine)(void *), void *arg) {
    pthread_t thread;
    int result = pthread_create(&thread, NULL, start_routine, arg);
    if (result != 0) {
        fprintf(stderr, "Failed to create thread: %d\n", result);
        return result;
    }
    // Detach the thread to allow it to clean up resources automatically upon completion
    result = pthread_detach(thread);
    if (result != 0) {
        fprintf(stderr, "Failed to detach thread: %d\n", result);
    }
    return result;
}
