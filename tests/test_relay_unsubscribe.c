#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>

#include "go.h"
#include "error.h"
#include "relay.h"
#include "subscription.h"
#include "filter.h"

#include "../libnostr/src/subscription-private.h"

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
    return fs;
}

int main(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    Relay *relay = new_relay(ctx, "wss://example.invalid", &err);
    assert(relay && err == NULL);

    Filters *fs = make_min_filters();
    Subscription *sub = create_subscription(relay, fs);
    assert(sub);

    // Simulate that the subscription is live and receiving
    atomic_store(&sub->priv->live, true);

    // Dispatch some events and then immediately unsubscribe mid-stream
    for (int i = 0; i < 5; i++) {
        NostrEvent *ev = create_event();
        ev->kind = 1;
        ev->content = strdup("payload");
        subscription_dispatch_event(sub, ev);
    }

    // Now unsubscribe; lifecycle should cancel, close channels, and may emit CLOSED locally
    subscription_unsub(sub);

    // Verify we either get a CLOSED reason promptly OR the channel is already closed
    GoContext *rx_closed = ctx_with_timeout_ms(300);
    char *reason = NULL;
    int rc_closed = go_channel_receive_with_context(sub->closed_reason, (void **)&reason, rx_closed);
    assert(rc_closed == 0 || rc_closed == -1);

    // Events channel should become closed/drained shortly; observe -1 receive consistently
    bool events_closed = false;
    for (int i = 0; i < 30; i++) {
        GoContext *rx_probe = ctx_with_timeout_ms(50);
        void *tmp = NULL;
        int rc_probe = go_channel_receive_with_context(sub->events, &tmp, rx_probe);
        go_context_free(rx_probe);
        if (rc_probe == -1) { events_closed = true; break; }
        usleep(10000);
    }
    assert(events_closed);

    // Further receives on events should fail as channel is closed and drained
    GoContext *rx_ev = ctx_with_timeout_ms(100);
    void *msg = NULL;
    int rc_ev = go_channel_receive_with_context(sub->events, &msg, rx_ev);
    assert(rc_ev == -1 || msg == NULL);

    free_subscription(sub);
    free_filters(fs);
    free_relay(relay);
    go_context_free(ctx);
    printf("test_relay_unsubscribe: OK\n");
    return 0;
}
