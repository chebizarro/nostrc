#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include "go.h"
#include "ticker.h"

static inline void sleep_ms(int ms){ struct timespec ts={ ms/1000, (long)(ms%1000)*1000000L }; nanosleep(&ts, NULL); }

typedef struct {
    Ticker *ticker;
    int target;
    int count;
} TickCounter;

void *consumer_thread(void *arg) {
    TickCounter *tc = (TickCounter *)arg;
    void *data = NULL;
    while (tc->count < tc->target) {
        if (go_channel_try_receive(tc->ticker->c, &data) == 0) {
            tc->count++;
        } else {
            // avoid busy spin
            sleep_ms(5);
        }
    }
    return NULL;
}

int main(void) {
    // Create a ticker that ticks every 50ms
    Ticker *t = create_ticker(50);
    if (!t) {
        fprintf(stderr, "failed to create ticker\n");
        return 1;
    }

    TickCounter tc = { .ticker = t, .target = 5, .count = 0 };
    pthread_t th;
    pthread_create(&th, NULL, consumer_thread, &tc);

    // Wait up to 2 seconds for 5 ticks
    int elapsed_ms = 0;
    while (tc.count < tc.target && elapsed_ms < 2000) {
        sleep_ms(50);
        elapsed_ms += 50;
    }

    stop_ticker(t);
    pthread_join(th, NULL);

    if (tc.count < tc.target) {
        fprintf(stderr, "ticker produced only %d ticks within deadline\n", tc.count);
        return 2;
    }

    printf("received %d ticks\n", tc.count);
    return 0;
}
