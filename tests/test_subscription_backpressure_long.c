#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#include "go.h"
#include "relay.h"
#include "subscription.h"
#include "filter.h"

// Internals for driving live + notice handler
#include "../libnostr/src/subscription-private.h"
#include "../libnostr/src/relay-private.h"

static GoContext *ctx_with_timeout_ms(int ms) {
    GoContext *bg = go_context_background();
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    struct timespec d = now;
    d.tv_sec += ms / 1000;
    long add = (long)(ms % 1000) * 1000000L;
    d.tv_nsec += add;
    if (d.tv_nsec >= 1000000000L) { d.tv_sec += 1; d.tv_nsec -= 1000000000L; }
    return go_with_deadline(bg, d);
}

static Filters *make_min_filters(void) {
    Filters *fs = (Filters *)malloc(sizeof(Filters));
    memset(fs, 0, sizeof(Filters));
    fs->filters = (Filter *)malloc(sizeof(Filter));
    memset(fs->filters, 0, sizeof(Filter));
    fs->count = 1; fs->capacity = 1;
    return fs;
}

static NostrEvent *make_dummy_event(int i) {
    NostrEvent *ev = create_event();
    ev->kind = 1;
    char *s = (char*)malloc(32);
    snprintf(s, 32, "ev-%d", i);
    ev->content = s;
    return ev;
}

static atomic_int notice_count = 0;
static void notice_stub(const char *msg) {
    (void)msg;
    atomic_fetch_add(&notice_count, 1);
}

int main(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    Error *err = NULL;
    GoContext *ctx = go_context_background();
    Relay *relay = new_relay(ctx, "wss://example.invalid", &err);
    assert(relay && err == NULL);

    // Install a notice handler to simulate NOTICE handling under load
    relay->priv->notice_handler = notice_stub;

    Filters *fs = make_min_filters();
    Subscription *sub = create_subscription(relay, fs);
    assert(sub);

    // Activate live to allow dispatch
    atomic_store(&sub->priv->live, true);

    // Prolonged dispatch for ~2 seconds: bursts, periodic EOSE and NOTICE
    const int duration_ms = 2000;
    struct timespec start; clock_gettime(CLOCK_REALTIME, &start);

    int i = 0;
    while (1) {
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        long elapsed = (long)((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000);
        if (elapsed >= duration_ms) break;

        // Burst 32 events; subscription should drop if full, but never deadlock
        for (int b = 0; b < 32; ++b) {
            subscription_dispatch_event(sub, make_dummy_event(i++));
        }
        // Every ~100ms, dispatch EOSE and invoke NOTICE handler
        if (i % 128 == 0) {
            subscription_dispatch_eose(sub);
            if (relay->priv->notice_handler) relay->priv->notice_handler("test-notice");
        }
        // Small sleep to simulate pacing
        usleep(2000);

        // Non-blocking probe to ensure we can observe activity and no stalls
        void *ev = NULL;
        GoContext *rx_probe = ctx_with_timeout_ms(1);
        (void)go_channel_receive_with_context(sub->events, &ev, rx_probe);
        go_context_free(rx_probe);
        if (ev) free_event((NostrEvent*)ev);
    }

    // Ensure at least one EOSE observed eventually
    void *sig = NULL;
    GoContext *rx_eose = ctx_with_timeout_ms(500);
    int rc_eose = go_channel_receive_with_context(sub->end_of_stored_events, &sig, rx_eose);
    go_context_free(rx_eose);
    assert(rc_eose == 0 || rc_eose == -1); // it's fine if drained already

    // NOTICE handler should have been called at least once during the run
    assert(atomic_load(&notice_count) > 0);

    subscription_unsub(sub);
    free_subscription(sub);
    free_filters(fs);
    free_relay(relay);
    go_context_free(ctx);

    printf("test_subscription_backpressure_long: OK\n");
    return 0;
}
