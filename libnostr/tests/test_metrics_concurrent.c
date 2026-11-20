#define _POSIX_C_SOURCE 200809L
#include "metrics-private.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Always check pthread calls and counts, including in NDEBUG builds. */
#undef assert
#define assert(expr) do { if (!(expr)) abort(); } while (0)

enum { THREADS = 24, SITES = 64, RECORDS = 32 };
static _Atomic(nostr_metric_histogram *) caches[SITES];
static char names[SITES][32];
static nostr_metric_histogram *observed[THREADS][SITES];
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static unsigned arrived, generation;

/* macOS has no pthread_barrier_t. No timing-dependent startup or sleeps. */
static void rendezvous(void)
{
    assert(pthread_mutex_lock(&gate_mu) == 0);
    unsigned gen = generation;
    if (++arrived == THREADS) {
        arrived = 0;
        ++generation;
        assert(pthread_cond_broadcast(&gate_cv) == 0);
    } else {
        while (gen == generation)
            assert(pthread_cond_wait(&gate_cv, &gate_mu) == 0);
    }
    assert(pthread_mutex_unlock(&gate_mu) == 0);
}

static nostr_metric_histogram *macro_site(void)
{
    NOSTR_METRIC_HISTOGRAM(h, "metrics_concurrent_macro");
    return h;
}

static void *worker(void *arg)
{
    size_t id = *(size_t *)arg;
    rendezvous();
    /* A counter can be the first registry user; initialization must precede
     * reading NOSTR_COUNTER_FLUSH_NS, including with concurrent lookups. */
    nostr_metric_counter_add("metrics_concurrent_total", 1);
    nostr_metric_histogram *macro = macro_site();
    assert((macro != NULL) == NOSTR_ENABLE_METRICS);
    nostr_metric_histogram_record(macro, 1000);
    for (int site = 0; site < SITES; ++site) {
        rendezvous();
        for (int j = 0; j < RECORDS; ++j) {
            nostr_metric_histogram *h =
                nostr_metric_histogram_cached(&caches[site], names[site]);
            assert((h != NULL) == NOSTR_ENABLE_METRICS);
            if (j == 0) observed[id][site] = h;
            assert(h == observed[id][site]);
            nostr_metric_histogram_record(h, 1000);
            /* More names than TLS slots exercises eviction and thread-exit
             * drains. Histogram/counter names may legally be identical. */
            nostr_metric_counter_add(names[site], 1);
            nostr_metric_counter_add("metrics_concurrent_total", 1);
        }
    }
    return NULL;
}

#if NOSTR_ENABLE_METRICS
static void expect_value(const char *output, const char *name, unsigned value)
{
    char line[128];
    snprintf(line, sizeof(line), "\nnostr_%s %u\n", name, value);
    assert(strstr(output, line) != NULL);
}
#endif

int main(void)
{
    /* Force a non-default configuration so an unsynchronized first counter
     * call cannot accidentally escape TSan through the default value. */
    assert(setenv("NOSTR_COUNTER_FLUSH_NS", "1000000000", 1) == 0);
    for (int i = 0; i < SITES; ++i) {
        atomic_init(&caches[i], NULL);
        snprintf(names[i], sizeof(names[i]), "metrics_concurrent_%d", i);
    }
    pthread_t threads[THREADS];
    size_t ids[THREADS];
    for (size_t i = 0; i < THREADS; ++i) {
        ids[i] = i;
        assert(pthread_create(&threads[i], NULL, worker, &ids[i]) == 0);
    }
    for (int i = 0; i < THREADS; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    for (int site = 0; site < SITES; ++site)
        for (int i = 0; i < THREADS; ++i)
            assert(observed[i][site] == observed[0][site]);

    size_t size = nostr_metrics_prometheus(NULL, 0);
#if NOSTR_ENABLE_METRICS
    assert(size > 0);
    char *output = malloc(size + 1);
    assert(output != NULL);
    assert(nostr_metrics_prometheus(output, size + 1) == size);
    expect_value(output, "metrics_concurrent_total", THREADS * (1 + SITES * RECORDS));
    expect_value(output, "metrics_concurrent_macro_count", THREADS);
    for (int i = 0; i < SITES; ++i) {
        char count_name[64];
        snprintf(count_name, sizeof(count_name), "%s_count", names[i]);
        expect_value(output, count_name, THREADS * RECORDS);
        expect_value(output, names[i], THREADS * RECORDS);
    }
    free(output);
#else
    assert(size == 0);
#endif
    assert(pthread_cond_destroy(&gate_cv) == 0);
    assert(pthread_mutex_destroy(&gate_mu) == 0);
    puts("concurrent metric resolution/counts: OK");
    return 0;
}
