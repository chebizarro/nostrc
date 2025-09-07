#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include "context.h"

static inline void sleep_ms(int ms){ struct timespec ts={ ms/1000, (long)(ms%1000)*1000000L }; nanosleep(&ts, NULL); }

#define WORKERS 16
#define ROUNDS 100

void *ctx_worker(void *arg) {
    GoContext *ctx = (GoContext*)arg;
    // Spin on wait; should exit promptly on cancel/deadline
    go_context_wait(ctx);
    return NULL;
}

int main(void){
    srand((unsigned)time(NULL));

    for (int r=0; r<ROUNDS; ++r) {
        // Stress cancellation path (deadline behavior covered by separate unit test)
        CancelContextResult res = go_context_with_cancel(NULL);
        GoContext *ctx = res.context;

        pthread_t th[WORKERS];
        for (int i=0;i<WORKERS;i++) pthread_create(&th[i], NULL, ctx_worker, ctx);

        // cancel soon
        sleep_ms(1);
        res.cancel(ctx);

        for (int i=0;i<WORKERS;i++) pthread_join(th[i], NULL);
        go_context_free(ctx);
    }

    printf("context stress test completed\n");
    return 0;
}
