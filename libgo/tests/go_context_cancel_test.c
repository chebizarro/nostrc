#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "go.h"

void *wait_for_cancel(void *arg) {
    GoContext *ctx = (GoContext *)arg;
    
    printf("Thread waiting for context to be canceled...\n");
    go_context_wait(ctx);  // Wait for the context to be canceled
    printf("Thread: Context canceled, error message: %s\n", go_context_err(ctx));
    
    return NULL;
}

int main() {
    printf("Creating a cancellable context...\n");

    // Create a cancellable context
    CancelContextResult cancel_result = go_context_with_cancel(NULL);
    GoContext *ctx = cancel_result.context;

    // Create a thread to wait for cancellation
    pthread_t thread;
    pthread_create(&thread, NULL, wait_for_cancel, (void *)ctx);

    // Sleep for a while before canceling the context
    printf("Main: Sleeping for 2 seconds before canceling context...\n");
    sleep(2);

    // Cancel the context
    printf("Main: Canceling the context now...\n");
    cancel_result.cancel(ctx);

    // Wait for the thread to complete
    pthread_join(thread, NULL);

    // Free the context
    go_context_free(ctx);
    free(ctx->vtable);
    free(ctx);

    printf("Test complete!\n");
    return 0;
}
