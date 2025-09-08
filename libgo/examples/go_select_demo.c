#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <unistd.h>
#include "go.h"
#include "channel.h"
#include "select.h"
#include "wait_group.h"

typedef struct { GoChannel *c; GoWaitGroup *wg; int count; int delay_ms; } ProdArgs;

void *producer(void *arg){
    ProdArgs *a = (ProdArgs*)arg;
    for (int i=0;i<a->count;i++){
        usleep(a->delay_ms * 1000);
        go_channel_send(a->c, (void*)(long)(i+1));
    }
    go_wait_group_done(a->wg);
    return NULL;
}

int main(void){
    GoChannel *c1 = go_channel_create(1);
    GoChannel *c2 = go_channel_create(1);

    GoWaitGroup wg; go_wait_group_init(&wg);
    go_wait_group_add(&wg, 2);
    ProdArgs a = { .c = c1, .wg = &wg, .count = 5, .delay_ms = 50 };
    ProdArgs b = { .c = c2, .wg = &wg, .count = 5, .delay_ms = 80 };
    go(producer, &a);
    go(producer, &b);

    int received = 0; void *out1 = NULL, *out2 = NULL;
    while (received < 10) {
        GoSelectCase cases[2] = {
            { .op = GO_SELECT_RECEIVE, .chan = c1, .recv_buf = &out1 },
            { .op = GO_SELECT_RECEIVE, .chan = c2, .recv_buf = &out2 },
        };
        int idx = go_select(cases, 2);
        if (idx == 0 && out1){ printf("recv c1: %ld\n", (long)out1); received++; out1=NULL; }
        else if (idx == 1 && out2){ printf("recv c2: %ld\n", (long)out2); received++; out2=NULL; }
        else { usleep(1000); }
    }

    go_wait_group_wait(&wg);
    go_wait_group_destroy(&wg);
    go_channel_free(c1); go_channel_free(c2);
    return 0;
}
