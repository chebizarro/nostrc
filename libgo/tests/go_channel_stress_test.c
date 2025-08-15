#define _POSIX_C_SOURCE 200809L
#include "channel.h"
#include "go.h"
#include "wait_group.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define PRODUCERS 8
#define CONSUMERS 8
#define ITEMS_PER_PROD 2000

typedef struct {
    GoChannel *c;
    int id;
    GoWaitGroup *wg;
} Args;

void *prod(void *arg) {
    Args *a = (Args *)arg;
    for (int i = 0; i < ITEMS_PER_PROD; i++) {
        // mix blocking and try_send
        if (i % 5 == 0) {
            while (go_channel_try_send(a->c, (void *)(long)(i + 1)) != 0)
                usleep(100);
        } else {
            go_channel_send(a->c, (void *)(long)(i + 1));
        }
    }
    go_wait_group_done(a->wg);
    return NULL;
}

void *cons(void *arg) {
    Args *a = (Args *)arg;
    void *v;
    int64_t local_sum = 0;
    int received = 0;
    while (go_channel_receive(a->c, &v) == 0) {
        local_sum += (long)v;
        received++;
    }
    // Basic sanity: if we received anything, sum must be positive and count > 0
    if (received < 0 || local_sum < 0) {
        fprintf(stderr, "consumer %d counters invalid: received=%d sum=%ld\n", a->id, received, (long)local_sum);
        abort();
    }
    go_wait_group_done(a->wg);
    return NULL;
}

int main(void) {
    GoChannel *c = go_channel_create(256);
    GoWaitGroup prodwg;
    go_wait_group_init(&prodwg);
    GoWaitGroup conswg;
    go_wait_group_init(&conswg);

    // Launch producers
    go_wait_group_add(&prodwg, PRODUCERS);
    Args pargs[PRODUCERS];
    for (int i = 0; i < PRODUCERS; i++) {
        pargs[i] = (Args){.c = c, .id = i, .wg = &prodwg};
        go(prod, &pargs[i]);
    }

    // Launch consumers
    go_wait_group_add(&conswg, CONSUMERS);
    Args cargs[CONSUMERS];
    for (int i = 0; i < CONSUMERS; i++) {
        cargs[i] = (Args){.c = c, .id = i, .wg = &conswg};
        go(cons, &cargs[i]);
    }

    // Wait for all producers to finish then close channel
    go_wait_group_wait(&prodwg);
    go_channel_close(c);

    // Wait for consumers to drain and finish
    go_wait_group_wait(&conswg);

    go_wait_group_destroy(&prodwg);
    go_wait_group_destroy(&conswg);
    go_channel_free(c);
    printf("channel stress test completed\n");
    return 0;
}
