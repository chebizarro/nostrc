/**
 * Concurrency test: Channel operations
 * Tests basic channel send/recv, blocking, and shutdown behavior
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>

#include "go.h"

/* Test state */
static atomic_int g_goroutines_active = 0;
static atomic_int g_test_failures = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s at %s:%d\n", msg, __FILE__, __LINE__); \
        atomic_fetch_add(&g_test_failures, 1); \
    } \
} while(0)

/* Test 1: Basic send/recv */
static void test_channel_basic(void) {
    printf("TEST: channel_basic\n");
    
    GoChannel *ch = go_channel_create(1);
    TEST_ASSERT(ch != NULL, "channel create failed");
    
    int *send_val = malloc(sizeof(int));
    *send_val = 42;
    void *recv_val = NULL;
    
    int rc = go_channel_send(ch, send_val);
    TEST_ASSERT(rc == 0, "send failed");
    
    rc = go_channel_receive(ch, &recv_val);
    TEST_ASSERT(rc == 0, "receive failed");
    TEST_ASSERT(recv_val != NULL && *(int*)recv_val == 42, "wrong value received");
    TEST_ASSERT(recv_val == send_val, "received different pointer");
    
    /* Channel doesn't take ownership - we still own the pointer */
    free(send_val);
    
    go_channel_free(ch);
    printf("  PASS\n");
}

/* Test 2: Blocking behavior */
typedef struct {
    GoChannel *ch;
    int value;
    atomic_bool started;
    atomic_bool completed;
} sender_ctx_t;

static void *sender_thread(void *arg) {
    sender_ctx_t *ctx = arg;
    atomic_store(&ctx->started, true);
    atomic_fetch_add(&g_goroutines_active, 1);
    
    usleep(50000); /* 50ms delay */
    int *val = malloc(sizeof(int));
    *val = ctx->value;
    go_channel_send(ctx->ch, val);
    
    atomic_store(&ctx->completed, true);
    atomic_fetch_sub(&g_goroutines_active, 1);
    return NULL;
}

static void test_channel_blocking(void) {
    printf("TEST: channel_blocking\n");
    
    GoChannel *ch = go_channel_create(1); /* Buffered (capacity 1) */
    TEST_ASSERT(ch != NULL, "channel create failed");
    
    sender_ctx_t ctx = {
        .ch = ch,
        .value = 99,
        .started = false,
        .completed = false
    };
    
    pthread_t tid;
    pthread_create(&tid, NULL, sender_thread, &ctx);
    
    /* Wait for sender to start */
    while (!atomic_load(&ctx.started)) usleep(1000);
    
    /* Receive should block until sender sends */
    void *recv_val = NULL;
    int rc = go_channel_receive(ch, &recv_val);
    TEST_ASSERT(rc == 0, "receive failed");
    TEST_ASSERT(recv_val != NULL && *(int*)recv_val == 99, "wrong value");
    TEST_ASSERT(atomic_load(&ctx.completed), "sender didn't complete");
    /* Sender allocated, we free */
    free(recv_val);
    
    pthread_join(tid, NULL);
    go_channel_free(ch);
    
    printf("  PASS\n");
}

/* Test 3: Channel close */
static void test_channel_close(void) {
    printf("TEST: channel_close\n");
    
    GoChannel *ch = go_channel_create(1);
    TEST_ASSERT(ch != NULL, "channel create failed");
    
    int *val = malloc(sizeof(int));
    *val = 123;
    go_channel_send(ch, val);
    
    go_channel_close(ch);
    
    /* Receive from closed channel should still get buffered value */
    void *recv_val = NULL;
    int rc = go_channel_receive(ch, &recv_val);
    TEST_ASSERT(rc == 0, "receive from closed failed");
    TEST_ASSERT(recv_val != NULL && *(int*)recv_val == 123, "wrong value from closed");
    free(recv_val);  /* We own the pointer */
    
    /* Second receive should fail */
    recv_val = NULL;
    rc = go_channel_receive(ch, &recv_val);
    TEST_ASSERT(rc != 0, "receive from empty closed should fail");
    
    go_channel_free(ch);
    printf("  PASS\n");
}

/* Test 4: Multi-producer/consumer */
#define NUM_PRODUCERS 3
#define NUM_CONSUMERS 2
#define ITEMS_PER_PRODUCER 100

typedef struct {
    GoChannel *ch;
    int producer_id;
    atomic_int *total_sent;
} producer_ctx_t;

typedef struct {
    GoChannel *ch;
    atomic_int *total_received;
} consumer_ctx_t;

static void *producer_func(void *arg) {
    producer_ctx_t *ctx = arg;
    atomic_fetch_add(&g_goroutines_active, 1);
    
    for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
        int *val = malloc(sizeof(int));
        *val = ctx->producer_id * 1000 + i;
        go_channel_send(ctx->ch, val);
        atomic_fetch_add(ctx->total_sent, 1);
    }
    
    atomic_fetch_sub(&g_goroutines_active, 1);
    return NULL;
}

static void *consumer_func(void *arg) {
    consumer_ctx_t *ctx = arg;
    atomic_fetch_add(&g_goroutines_active, 1);
    
    void *val = NULL;
    while (go_channel_receive(ctx->ch, &val) == 0) {
        atomic_fetch_add(ctx->total_received, 1);
        free(val);  /* Producers allocated, consumers free */
        val = NULL;
    }
    
    atomic_fetch_sub(&g_goroutines_active, 1);
    return NULL;
}

static void test_channel_multi_producer_consumer(void) {
    printf("TEST: channel_multi_producer_consumer\n");
    
    GoChannel *ch = go_channel_create(10);
    TEST_ASSERT(ch != NULL, "channel create failed");
    
    atomic_int total_sent = 0;
    atomic_int total_received = 0;
    
    pthread_t producers[NUM_PRODUCERS];
    pthread_t consumers[NUM_CONSUMERS];
    producer_ctx_t prod_ctx[NUM_PRODUCERS];
    consumer_ctx_t cons_ctx[NUM_CONSUMERS];
    
    /* Start producers */
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        prod_ctx[i].ch = ch;
        prod_ctx[i].producer_id = i;
        prod_ctx[i].total_sent = &total_sent;
        pthread_create(&producers[i], NULL, producer_func, &prod_ctx[i]);
    }
    
    /* Start consumers */
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        cons_ctx[i].ch = ch;
        cons_ctx[i].total_received = &total_received;
        pthread_create(&consumers[i], NULL, consumer_func, &cons_ctx[i]);
    }
    
    /* Wait for producers */
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pthread_join(producers[i], NULL);
    }
    
    /* Close channel to signal consumers */
    go_channel_close(ch);
    
    /* Wait for consumers */
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        pthread_join(consumers[i], NULL);
    }
    
    int expected = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
    TEST_ASSERT(atomic_load(&total_sent) == expected, "wrong total sent");
    TEST_ASSERT(atomic_load(&total_received) == expected, "wrong total received");
    TEST_ASSERT(atomic_load(&g_goroutines_active) == 0, "goroutines leaked");
    
    go_channel_free(ch);
    printf("  PASS\n");
}

int main(void) {
    printf("=== Concurrency Tests: Channels ===\n");
    
    test_channel_basic();
    test_channel_blocking();
    test_channel_close();
    test_channel_multi_producer_consumer();
    
    int failures = atomic_load(&g_test_failures);
    printf("\n=== Results: %d failures ===\n", failures);
    
    return failures > 0 ? 1 : 0;
}
