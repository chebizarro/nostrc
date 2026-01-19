#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "channel.h"
#include "go.h"

#ifdef DEBUG_CHANNEL
static void dump_channel_state(GoChannel *chan, const char *label) {
    fprintf(stderr, "[%s] Channel %p state:\n", label, chan);
    fprintf(stderr, "  in=%zu out=%zu",
            atomic_load(&chan->in), atomic_load(&chan->out));
#if NOSTR_CHANNEL_DERIVE_SIZE
    size_t occ = atomic_load(&chan->in) - atomic_load(&chan->out);
    fprintf(stderr, " occupancy=%zu", occ);
#else
    fprintf(stderr, " size=%zu", chan->size);
#endif
    fprintf(stderr, " closed=%d\n", atomic_load(&chan->closed));
}
#else
#define dump_channel_state(chan, label) ((void)0)
#endif

/* Portable sleep (milliseconds) */
static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
}

typedef struct LateRecvArgs {
    GoChannel *chan;
    atomic_int *done;
    int expected;
} LateRecvArgs;

static void *late_receiver_thread(void *arg) {
    LateRecvArgs *args = (LateRecvArgs *)arg;
    void *data = NULL;
    int rc = go_channel_receive(args->chan, &data);
    if (rc != 0) {
        fprintf(stderr, "FAIL: Receive returned %d\n", rc);
        dump_channel_state(args->chan, "On failure");
        abort();
    }
    if ((intptr_t)data != args->expected) {
        fprintf(stderr, "FAIL: Expected %d, got %ld\n", args->expected, (intptr_t)data);
        abort();
    }
    atomic_store(args->done, 1);
    return NULL;
}

static void *warm_sender_thread(void *arg) {
    GoChannel *chan = (GoChannel *)arg;
    for (int i = 0; i < 5; i++) {
        go_channel_send(chan, (void*)(intptr_t)i);
    }
    return NULL;
}

static void *warm_receiver_thread(void *arg) {
    GoChannel *chan = (GoChannel *)arg;
    for (int i = 0; i < 5; i++) {
        void *data;
        go_channel_receive(chan, &data);
    }
    return NULL;
}

/* Simple sync barrier using atomics (portable replacement for pthread_barrier) */
typedef struct {
    atomic_int count;
    atomic_int released;
    int target;
} SimpleBarrier;

static void simple_barrier_init(SimpleBarrier *b, int target) {
    atomic_store(&b->count, 0);
    atomic_store(&b->released, 0);
    b->target = target;
}

static void simple_barrier_wait(SimpleBarrier *b) {
    int n = atomic_fetch_add(&b->count, 1) + 1;
    if (n == b->target) {
        /* Last thread to arrive - release all */
        atomic_store(&b->released, 1);
    } else {
        /* Wait for release */
        while (!atomic_load(&b->released)) {
            sleep_ms(1);
        }
    }
}

typedef struct ChannelPhase2Ctx {
    GoChannel *chan;
    SimpleBarrier *barrier;
    atomic_int *done;
} ChannelPhase2Ctx;

static void *phase2_receiver_thread(void *arg) {
    ChannelPhase2Ctx *args = (ChannelPhase2Ctx *)arg;
    /* Signal ready */
    simple_barrier_wait(args->barrier);
    void *data;
    int rc = go_channel_receive(args->chan, &data);
    if (rc != 0) {
        fprintf(stderr, "FAIL: Phase 2 receive failed: %d\n", rc);
        dump_channel_state(args->chan, "Phase 2 recv fail");
        abort();
    }
    atomic_store(args->done, 1);
    return NULL;
}

static void *phase2_sender_thread(void *arg) {
    ChannelPhase2Ctx *args = (ChannelPhase2Ctx *)arg;
    /* Wait for receiver to be ready */
    simple_barrier_wait(args->barrier);
    /* Give receiver a moment to enter wait */
    sleep_ms(10);
    int rc = go_channel_send(args->chan, (void*)(intptr_t)99);
    if (rc != 0) {
        fprintf(stderr, "FAIL: Phase 2 send failed: %d\n", rc);
        abort();
    }
    return NULL;
}

/* Wait for done flag with timeout */
static int wait_done(atomic_int *done, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        if (atomic_load(done) == 1) {
            return 0;
        }
        sleep_ms(10);
        waited += 10;
    }
    return -1; /* Timeout */
}

/* Test: Late receiver after idle must receive data */
int test_channel_late_receiver_after_idle() {
    printf("Testing late receiver after idle...\n");
    GoChannel *chan = go_channel_create(10);

    /* Phase 1: Send data when no receiver exists */
    int value = 42;
    int rc = go_channel_send(chan, (void*)(intptr_t)value);
    assert(rc == 0);

    dump_channel_state(chan, "After send");

    /* Phase 2: Channel is idle (no active threads) */
    /* Use atomic fence to ensure visibility */
    atomic_thread_fence(memory_order_seq_cst);

    /* Phase 3: Late receiver arrives */
    pthread_t receiver;
    atomic_int recv_done = 0;

    LateRecvArgs recv_args = (LateRecvArgs){ chan, &recv_done, value };
    pthread_create(&receiver, NULL, late_receiver_thread, &recv_args);

    /* Wait for done flag with 1 second timeout */
    if (wait_done(&recv_done, 1000) != 0) {
        fprintf(stderr, "FAIL: Late receiver blocked forever!\n");
        dump_channel_state(chan, "Timeout");
        abort();
    }

    pthread_join(receiver, NULL);
    assert(atomic_load(&recv_done) == 1);
    go_channel_free(chan);
    printf("  PASSED\n");
    return 0;
}

/* Test: Two-phase incremental usage */
int test_two_phase_incremental_usage() {
    printf("Testing two-phase incremental usage...\n");
    GoChannel *chan = go_channel_create(10);

    /* Phase 1: Warm-up burst */
    pthread_t sender1, receiver1;

    pthread_create(&sender1, NULL, warm_sender_thread, chan);
    pthread_create(&receiver1, NULL, warm_receiver_thread, chan);

    pthread_join(sender1, NULL);
    pthread_join(receiver1, NULL);

    dump_channel_state(chan, "After phase 1");

    /* Channel is now idle and empty */
    atomic_thread_fence(memory_order_seq_cst);

    /* Phase 2: Small incremental work */
    SimpleBarrier barrier;
    simple_barrier_init(&barrier, 2);

    pthread_t receiver2, sender2;
    atomic_int phase2_done = 0;

    ChannelPhase2Ctx phase2_ctx = (ChannelPhase2Ctx){ chan, &barrier, &phase2_done };

    /* Start receiver first */
    pthread_create(&receiver2, NULL, phase2_receiver_thread, &phase2_ctx);

    /* Start sender */
    pthread_create(&sender2, NULL, phase2_sender_thread, &phase2_ctx);

    /* Wait for phase2_done with 1 second timeout */
    if (wait_done(&phase2_done, 1000) != 0) {
        fprintf(stderr, "FAIL: Phase 2 receiver blocked!\n");
        dump_channel_state(chan, "Phase 2 timeout");
        abort();
    }

    pthread_join(receiver2, NULL);
    pthread_join(sender2, NULL);
    assert(atomic_load(&phase2_done) == 1);

    go_channel_free(chan);
    printf("  PASSED\n");
    return 0;
}

int main() {
    printf("Running incremental channel tests...\n");

    test_channel_late_receiver_after_idle();
    test_two_phase_incremental_usage();

    printf("All tests passed!\n");
    return 0;
}
