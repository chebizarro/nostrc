#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#include "go.h"
#include "nostr-event.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-filter.h"
#include "nostr-init.h"
#include "nostr/metrics.h"

#include "../libnostr/src/subscription-private.h"
#include "../libnostr/src/relay-private.h"

static NostrEvent *make_dummy_event(int i) {
    NostrEvent *ev = nostr_event_new();
    ev->kind = 1;
    char *s = (char*)malloc(32);
    snprintf(s, 32, "ev-%d", i);
    ev->content = s;
    return ev;
}

static int getenv_int(const char *k, int defv) {
    const char *s = getenv(k);
    if (s && *s) {
        int v = atoi(s);
        if (v >= 0) return v;
    }
    return defv;
}

typedef struct {
    NostrSubscription *sub;
    int consume_us;
    atomic_int *stop;
} ConsumerArg;

static void *consumer_thread(void *arg) {
    ConsumerArg *ca = (ConsumerArg*)arg;
    void *ev = NULL;
    while (atomic_load(ca->stop) == 0) {
        if (go_channel_try_receive(ca->sub->events, &ev) == 0) {
            if (ev) nostr_event_free((NostrEvent*)ev);
        }
        if (ca->consume_us > 0) usleep(ca->consume_us);
    }
    return NULL;
}

int main(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);
    setenv("NOSTR_METRICS_DUMP", "1", 1);
    const int interval_ms = getenv_int("NOSTR_METRICS_INTERVAL_MS", 200);
    char buf[16]; snprintf(buf, sizeof(buf), "%d", interval_ms);
    setenv("NOSTR_METRICS_INTERVAL_MS", buf, 1);
    setenv("NOSTR_METRICS_DUMP_ON_EXIT", "1", 1);
    nostr_global_init();

    Error *err = NULL;
    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
    assert(relay && err == NULL);

    NostrFilters *fs = nostr_filters_new();
    nostr_filters_add(fs, nostr_filter_new());
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, fs);
    assert(sub);
    atomic_store(&sub->priv->live, true);

    nostr_metric_histogram *h_dispatch = nostr_metric_histogram_get("bp_dispatch_ns");
    nostr_metric_histogram *h_burst = nostr_metric_histogram_get("bp_burst_ns");

    int duration_ms = getenv_int("BP_DURATION_MS", 5000);
    int burst = getenv_int("BP_BURST", 64);
    int prod_sleep_us = getenv_int("BP_SLEEP_US", 1000);
    int consume_us = getenv_int("BP_CONSUME_US", 5000);

    atomic_int stop = 0;
    ConsumerArg carg = { .sub = sub, .consume_us = consume_us, .stop = &stop };
    go(consumer_thread, &carg);

    struct timespec start; clock_gettime(CLOCK_REALTIME, &start);
    int i = 0;
    while (1) {
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        long elapsed = (long)((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000);
        if (elapsed >= duration_ms) break;

        nostr_metric_timer t_burst; nostr_metric_timer_start(&t_burst);
        for (int b = 0; b < burst; ++b) {
            nostr_metric_timer t; nostr_metric_timer_start(&t);
            // Blocking send to force wait when channel full
            go_channel_send(sub->events, make_dummy_event(i++));
            nostr_metric_timer_stop(&t, h_dispatch);
            nostr_metric_counter_add("bp_events_generated", 1);
        }
        nostr_metric_timer_stop(&t_burst, h_burst);

        if (i % 128 == 0) {
            nostr_subscription_dispatch_eose(sub);
            nostr_metric_counter_add("bp_eose_sent", 1);
        }
        if (prod_sleep_us > 0) usleep(prod_sleep_us);
    }

    atomic_store(&stop, 1);
    // give consumer a moment to exit
    usleep(20000);

    nostr_subscription_unsubscribe(sub);
    nostr_subscription_free(sub);
    nostr_filters_free(fs);
    nostr_relay_free(relay);
    go_context_free(ctx);

    nostr_metrics_dump();
    nostr_global_cleanup();

    printf("test_subscription_blocking_depth: OK\n");
    return 0;
}
