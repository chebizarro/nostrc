#include <stdio.h>
#include <stdlib.h>
#include "go.h"
#include "channel.h"
#include "wait_group.h"

typedef struct { GoChannel *chan; GoWaitGroup *wg; } TaskArgs;

void *producer(void *arg) {
    TaskArgs *ta = (TaskArgs*)arg;
    GoChannel *chan = ta->chan;
    for (int i = 0; i < 10; i++) {
        int *data = malloc(sizeof(int));
        *data = i;
        go_channel_send(chan, data);
        printf("Produced: %d\n", *data);
    }
    // Signal no more values will be sent
    go_channel_close(chan);
    go_wait_group_done(ta->wg);
    return NULL;
}

void *consumer(void *arg) {
    TaskArgs *ta = (TaskArgs*)arg;
    GoChannel *chan = ta->chan;
    int *data;
    while (go_channel_receive(chan, (void **)&data) == 0) {
        printf("Consumed: %d\n", *data);
        free(data);
    }
    go_wait_group_done(ta->wg);
    return NULL;
}

int main(void) {
    GoChannel *chan = go_channel_create(5);
    GoWaitGroup wg; go_wait_group_init(&wg);
    go_wait_group_add(&wg, 2);
    TaskArgs args = { .chan = chan, .wg = &wg };
    go(producer, &args);
    go(consumer, &args);
    go_wait_group_wait(&wg);
    go_wait_group_destroy(&wg);
    go_channel_free(chan);
    return 0;
}
