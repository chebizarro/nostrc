static inline void sleep_ms(int ms){ struct timespec ts={ ms/1000, (long)(ms%1000)*1000000L }; nanosleep(&ts, NULL); }
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include "go.h"

static void *recv_block_then_close_result;

static void *recv_blocker(void *arg) {
    GoChannel *c = (GoChannel*)arg;
    void *val = (void*)0xdeadbeef;
    int rc = go_channel_receive(c, &val);
    recv_block_then_close_result = (void*)(long)rc;
    return NULL;
}

static void *send_blocker(void *arg) {
    GoChannel *c = (GoChannel*)arg;
    // channel capacity 1, pre-filled by main; this will block until close
    int rc = go_channel_send(c, (void*)123);
    return (void*)(long)rc;
}

int main(void) {
    // Test 1: receiver unblocks on close and get failure
    GoChannel *c1 = go_channel_create(1);
    pthread_t rth;
    pthread_create(&rth, NULL, recv_blocker, c1);
    sleep_ms(50);
    go_channel_close(c1);
    pthread_join(rth, NULL);
    // Expect receive to fail (rc != 0) when channel is closed and empty
    if ((long)recv_block_then_close_result == 0) {
        fprintf(stderr, "receive unexpectedly succeeded on closed channel\n");
        return 1;
    }
    go_channel_free(c1);

    // Test 2: sender unblocks on close and get failure
    GoChannel *c2 = go_channel_create(1);
    // Pre-fill so next send blocks
    go_channel_send(c2, (void*)1);
    pthread_t sth;
    pthread_create(&sth, NULL, send_blocker, c2);
    sleep_ms(50);
    go_channel_close(c2);
    void *send_rc;
    pthread_join(sth, &send_rc);
    if ((long)send_rc == 0) {
        fprintf(stderr, "send unexpectedly succeeded on closed channel\n");
        return 2;
    }
    go_channel_free(c2);

    printf("channel close tests passed\n");
    return 0;
}
