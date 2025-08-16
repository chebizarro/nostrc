#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <nsync.h>

#include "channel.h"
#include "context.h"

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef struct {
    GoChannel *ch;
    size_t messages;
} producer_arg_t;

typedef struct {
    GoChannel *ch;
    size_t messages;
    size_t consumed;
} consumer_arg_t;

static void *producer_thread(void *arg) {
    producer_arg_t *pa = (producer_arg_t *)arg;
    for (size_t i = 0; i < pa->messages; ++i) {
        // busy loop send
        while (go_channel_try_send(pa->ch, (void*)(uintptr_t)(i + 1)) != 0) {
            // fallback to blocking send to avoid wasting CPU when full
            go_channel_send(pa->ch, (void*)(uintptr_t)(i + 1));
            break;
        }
    }
    return NULL;
}

static void *consumer_thread(void *arg) {
    consumer_arg_t *ca = (consumer_arg_t *)arg;
    for (;;) {
        void *v = NULL;
        if (go_channel_try_receive(ca->ch, &v) == 0) {
            ca->consumed++;
            if (ca->consumed >= ca->messages) break;
        } else {
            // blocking receive to ensure progress
            go_channel_receive(ca->ch, &v);
            ca->consumed++;
            if (ca->consumed >= ca->messages) break;
        }
    }
    return NULL;
}

static void run_bench(size_t capacity, int prod, int cons, size_t total_msgs) {
    GoChannel *ch = go_channel_create(capacity);
    if (!ch) { fprintf(stderr, "failed to create channel\n"); exit(1); }

    size_t msgs_per_cons = total_msgs / (size_t)cons;
    size_t msgs_per_prod = total_msgs / (size_t)prod;

    pthread_t *pt = (pthread_t *)calloc((size_t)prod, sizeof(pthread_t));
    pthread_t *ct = (pthread_t *)calloc((size_t)cons, sizeof(pthread_t));
    producer_arg_t *pargs = (producer_arg_t *)calloc((size_t)prod, sizeof(producer_arg_t));
    consumer_arg_t *cargs = (consumer_arg_t *)calloc((size_t)cons, sizeof(consumer_arg_t));

    for (int i = 0; i < prod; ++i) { pargs[i].ch = ch; pargs[i].messages = msgs_per_prod; }
    for (int i = 0; i < cons; ++i) { cargs[i].ch = ch; cargs[i].messages = msgs_per_cons; cargs[i].consumed = 0; }

    uint64_t t0 = now_ns();
    for (int i = 0; i < cons; ++i) pthread_create(&ct[i], NULL, consumer_thread, &cargs[i]);
    for (int i = 0; i < prod; ++i) pthread_create(&pt[i], NULL, producer_thread, &pargs[i]);

    for (int i = 0; i < prod; ++i) pthread_join(pt[i], NULL);
    for (int i = 0; i < cons; ++i) pthread_join(ct[i], NULL);
    uint64_t t1 = now_ns();

    double secs = (double)(t1 - t0) / 1e9;
    size_t consumed_total = 0;
    for (int i = 0; i < cons; ++i) consumed_total += cargs[i].consumed;

    double mps = (double)consumed_total / secs;
    printf("capacity=%zu prod=%d cons=%d total=%zu time=%.3fs rate=%.0f msgs/s\n",
           capacity, prod, cons, consumed_total, secs, mps);
    fflush(stdout);

    go_channel_close(ch);
    go_channel_free(ch);

    free(pt); free(ct); free(pargs); free(cargs);
}

static size_t parse_size(const char *s, size_t def) {
    if (!s || !*s) return def;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (end && (*end == 'k' || *end == 'K')) v *= 1000ull;
    if (end && (*end == 'm' || *end == 'M')) v *= 1000ull * 1000ull;
    if (v == 0) return def;
    return (size_t)v;
}

int main(int argc, char **argv) {
    // Env or args:
    // ARGS: capacity prod cons total_msgs
    size_t capacity = (argc > 1) ? (size_t)strtoull(argv[1], NULL, 10) : 1024;
    int prod = (argc > 2) ? atoi(argv[2]) : 1;
    int cons = (argc > 3) ? atoi(argv[3]) : 1;
    size_t total = (argc > 4) ? parse_size(argv[4], 1000000) : 1000000; // default 1M msgs

    const char *iters = getenv("NOSTR_SPIN_ITERS");
    const char *us = getenv("NOSTR_SPIN_US");
    printf("NOSTR_SPIN_ITERS=%s NOSTR_SPIN_US=%s\n", iters ? iters : "(default)", us ? us : "(default)");
    fflush(stdout);

    run_bench(capacity, prod, cons, total);
    return 0;
}
