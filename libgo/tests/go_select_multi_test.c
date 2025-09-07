#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "go.h"
#include "select.h"

static inline void sleep_ms(int ms){ struct timespec ts={ ms/1000, (long)(ms%1000)*1000000L }; nanosleep(&ts, NULL); }

int main(void){
    srand((unsigned)time(NULL));
    GoChannel *a = go_channel_create(2);
    GoChannel *b = go_channel_create(2);
    GoChannel *c = go_channel_create(1);

    // Make two receive-ready, and a send-impossible (full) channel
    go_channel_send(a, (void*)11);
    go_channel_send(b, (void*)22);
    go_channel_send(c, (void*)33); // full so sends should fail

    void *ra = NULL, *rb = NULL;
    GoSelectCase cases[3] = {
        { .op = GO_SELECT_RECEIVE, .chan = a, .recv_buf = &ra },
        { .op = GO_SELECT_RECEIVE, .chan = b, .recv_buf = &rb },
        { .op = GO_SELECT_SEND, .chan = c, .value = (void*)44 },
    };

    int idx = go_select(cases, 3);
    if (idx == 0) {
        if ((long)ra != 11) { fprintf(stderr, "expected ra 11, got %ld\n", (long)ra); return 1; }
    } else if (idx == 1) {
        if ((long)rb != 22) { fprintf(stderr, "expected rb 22, got %ld\n", (long)rb); return 2; }
    } else {
        fprintf(stderr, "select chose invalid case %d\n", idx);
        return 3;
    }

    // Basic fairness sanity: over many trials, both a and b should be picked at least once
    int seen_a = 0, seen_b = 0;
    for (int i = 0; i < 200; ++i) {
        // refill both
        go_channel_try_send(a, (void*)1);
        go_channel_try_send(b, (void*)2);
        ra = rb = NULL;
        int k = go_select(cases, 3);
        if (k == 0) seen_a = 1; else if (k == 1) seen_b = 1;
        if (seen_a && seen_b) break;
        sleep_ms(1);
    }
    if (!(seen_a && seen_b)) { fprintf(stderr, "fairness check failed\n"); return 4; }

    go_channel_free(a); go_channel_free(b); go_channel_free(c);
    return 0;
}
