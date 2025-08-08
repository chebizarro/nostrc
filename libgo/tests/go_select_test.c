#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include "go.h"
#include "select.h"

static void *delayed_send(void *arg){
    GoChannel *c = (GoChannel*)arg;
    usleep(50*1000);
    int v = 42;
    go_channel_send(c, (void*)(long)v);
    return NULL;
}

int main(void){
    // Case 1: two receive cases, one has data now -> should pick that index
    GoChannel *a = go_channel_create(2);
    GoChannel *b = go_channel_create(2);
    void *recv_a = NULL, *recv_b = NULL;
    go_channel_send(a, (void*)1);
    GoSelectCase cases1[2] = {
        { .op = GO_SELECT_RECEIVE, .chan = a, .recv_buf = &recv_a },
        { .op = GO_SELECT_RECEIVE, .chan = b, .recv_buf = &recv_b },
    };
    int idx = go_select(cases1, 2);
    if (idx != 0 || recv_a != (void*)1) {
        fprintf(stderr, "select receive immediate failed idx=%d recv_a=%p\n", idx, recv_a);
        return 1;
    }

    // Case 2: send vs receive, receive will become ready after delay
    GoChannel *c = go_channel_create(1);
    pthread_t th;
    pthread_create(&th, NULL, delayed_send, c);
    void *out = NULL;
    // Use a separate channel 'd' that is already full so send will not be chosen
    GoChannel *d = go_channel_create(1);
    go_channel_send(d, (void*)123);
    GoSelectCase cases2[2] = {
        { .op = GO_SELECT_RECEIVE, .chan = c, .recv_buf = &out },
        { .op = GO_SELECT_SEND, .chan = d, .value = (void*)2 },
    };
    int idx2 = go_select(cases2, 2);
    pthread_join(th, NULL);
    if (idx2 != 0 || (long)out != 42) {
        fprintf(stderr, "select mixed failed idx2=%d out=%ld\n", idx2, (long)out);
        return 2;
    }

    go_channel_free(a);
    go_channel_free(b);
    go_channel_free(c);
    go_channel_free(d);
    return 0;
}
