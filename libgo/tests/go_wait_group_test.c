#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include "wait_group.h"

#define NWORKERS 5

static inline void sleep_ms(int ms){ struct timespec ts={ ms/1000, (long)(ms%1000)*1000000L }; nanosleep(&ts, NULL); }

static void *worker(void *arg) {
    GoWaitGroup *wg = (GoWaitGroup*)arg;
    sleep_ms(50); // simulate work
    go_wait_group_done(wg);
    return NULL;
}

int main(void) {
    GoWaitGroup wg;
    go_wait_group_init(&wg);

    pthread_t th[NWORKERS];
    go_wait_group_add(&wg, NWORKERS);
    for (int i = 0; i < NWORKERS; ++i) {
        pthread_create(&th[i], NULL, worker, &wg);
    }

    // Wait should block until all workers call done
    go_wait_group_wait(&wg);

    for (int i = 0; i < NWORKERS; ++i) {
        pthread_join(th[i], NULL);
    }

    // Counter should be zero now; another wait should return immediately
    go_wait_group_wait(&wg);

    go_wait_group_destroy(&wg);
    printf("wait group test passed\n");
    return 0;
}
