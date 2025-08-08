#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>

#include "go.h"
#include "relay.h"
#include "subscription.h"
#include "filter.h"

// Drive internals
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

static NostrEvent *make_dummy_event(int i) {
    NostrEvent *ev = create_event();
    ev->kind = 1;
    char *s = (char*)malloc(32);
    snprintf(s, 32, "ev-%d", i);
    ev->content = s;
    return ev;
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

    // Activate live to allow dispatch
    atomic_store(&sub->priv->live, true);

    // Rapidly enqueue more events than capacity; subscription should drop extra without deadlock.
    // We also dispatch EOSE and ensure it is still delivered.
    for (int i = 0; i < 50; i++) {
        subscription_dispatch_event(sub, make_dummy_event(i));
    }
    subscription_dispatch_eose(sub);

    // We should be able to read at least one event then EOSE without blocking forever.
    void *got = NULL;
    GoContext *rx1 = ctx_with_timeout_ms(200);
    int rc1 = go_channel_receive_with_context(sub->events, &got, rx1);
    assert(rc1 == 0 && got != NULL);
    if (got) free_event((NostrEvent*)got);

    // Drain until empty quickly; remaining should have been dropped.
    GoContext *rx2 = ctx_with_timeout_ms(50);
    got = NULL;
    int rc2 = go_channel_receive_with_context(sub->events, &got, rx2);
    assert(rc2 == -1);

    // EOSE must still arrive even if events queue was full/dropping
    GoContext *rx3 = ctx_with_timeout_ms(200);
    void *sig = NULL;
    int rc3 = go_channel_receive_with_context(sub->end_of_stored_events, &sig, rx3);
    assert(rc3 == 0);

    subscription_unsub(sub);
    free_subscription(sub);
    free_filters(fs);
    free_relay(relay);
    go_context_free(ctx);
    printf("test_subscription_backpressure: OK\n");
    return 0;
}
