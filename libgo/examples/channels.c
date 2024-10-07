#include <stdio.h>
#include <stdlib.h>
#include "go.h"
#include "channel.h"

void *producer(void *arg) {
    GoChannel *chan = (GoChannel *)arg;
    for (int i = 0; i < 10; i++) {
        int *data = malloc(sizeof(int));
        *data = i;
        go_channel_send(chan, data);
        printf("Produced: %d\n", *data);
    }
    return NULL;
}

void *consumer(void *arg) {
    GoChannel *chan = (GoChannel *)arg;
    for (int i = 0; i < 10; i++) {
        int *data;
        go_channel_receive(chan, (void **)&data);
        printf("Consumed: %d\n", *data);
        free(data);
    }
    return NULL;
}

int main() {
    GoChannel *chan = go_channel_create(5);

    go(producer, chan);
    go(consumer, chan);

    // Sleep to allow threads to finish execution
    //sleep(2);

    go_channel_free(chan);
    return 0;
}
