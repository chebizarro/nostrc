#define _POSIX_C_SOURCE 200809L
#include "go.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

void *wait_with_timeout(void *arg) {
    GoContext *ctx = (GoContext *)arg;

    printf("Thread: Waiting for the context to timeout...\n");
    go_context_wait(ctx); // Wait for the context to timeout or be canceled
    Error *err = go_context_err(ctx);
    printf("Thread: Context timed out, error message: %s\n", err ? err->message : NULL);

    return NULL;
}

int main(void) {
    // Set a 3-second deadline
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3; // Set the timeout 3 seconds in the future

    printf("Main: Creating context with a 3-second deadline.\n");
    GoContext *ctx = go_with_deadline(NULL, deadline);

    // Create a thread to wait for the context to timeout
    pthread_t thread;
    pthread_create(&thread, NULL, wait_with_timeout, (void *)ctx);

    // Wait for the thread to finish
    pthread_join(thread, NULL);

    // Free the context (vtable and struct are freed internally)
    go_context_free(ctx);

    printf("Test complete!\n");
    return 0;
}
