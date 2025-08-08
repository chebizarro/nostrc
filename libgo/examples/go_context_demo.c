#include <stdio.h>
#include <unistd.h>
#include "go.h"
#include "context.h"
#include "wait_group.h"

typedef struct { GoContext *ctx; GoWaitGroup *wg; } Args;

void *ctx_worker(void *arg) {
    Args *a = (Args*)arg;
    // Block until canceled
    go_context_wait(a->ctx);
    // Optionally inspect error
    Error *err = go_context_err(a->ctx);
    printf("worker: context done (%s)\n", err ? err->message : "no error");
    go_wait_group_done(a->wg);
    return NULL;
}

int main(void){
    // Create a cancellable context
    CancelContextResult r = go_context_with_cancel(NULL);

    // Start a few goroutines that wait on the context
    GoWaitGroup wg; go_wait_group_init(&wg);
    go_wait_group_add(&wg, 3);
    Args a = { .ctx = r.context, .wg = &wg };
    for (int i=0;i<3;i++) {
        go(ctx_worker, &a);
    }

    // Do some work...
    usleep(200 * 1000);

    // Cancel the context; all workers should unblock promptly
    r.cancel(r.context);

    // Wait for workers to exit
    go_wait_group_wait(&wg);
    go_wait_group_destroy(&wg);

    go_context_free(r.context);
    return 0;
}
