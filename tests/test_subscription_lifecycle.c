#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>
#include <unistd.h>

#include "go.h"
#include "error.h"
#include "nostr-relay.h"
#include "nostr-filter.h"
#include "nostr-subscription.h"
#include "nostr-event.h"

// Include private API to drive dispatchers directly and observe internals
#include "../libnostr/src/subscription-private.h"

static GoContext *ctx_with_timeout_ms(int ms) {
    GoContext *bg = go_context_background();
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct timespec deadline = now;
    // add ms to deadline
    deadline.tv_sec += ms / 1000;
    long nsec_add = (long)(ms % 1000) * 1000000L;
    deadline.tv_nsec += nsec_add;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    return go_with_deadline(bg, deadline);
}

static NostrEvent *make_dummy_event(void) {
    NostrEvent *ev = nostr_event_new();
    ev->kind = 1; // text note
    ev->content = strdup("hello");
    return ev;
}

static NostrFilters *make_min_filters(void) {
    NostrFilters *fs = nostr_filters_new();
    nostr_filters_add(fs, nostr_filter_new());
    return fs;
}

static void test_eose_then_receive_signal(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
    assert(relay && err == NULL);

    NostrFilters *fs = make_min_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, fs);
    assert(sub);

    // Initially not EOSE'd
    assert(!atomic_load(&sub->priv->eosed));
    nostr_subscription_dispatch_eose(sub);
    assert(atomic_load(&sub->priv->eosed));

    // Receive on end_of_stored_events promptly
    GoContext *rxctx = ctx_with_timeout_ms(200);
    void *sig = NULL;
    int rc = go_channel_receive_with_context(sub->end_of_stored_events, &sig, rxctx);
    assert(rc == 0);

    nostr_subscription_unsubscribe(sub);
    nostr_subscription_unsubscribe(sub);
    usleep(50000);
    nostr_subscription_free(sub);
    nostr_filters_free(fs);
    nostr_relay_free(relay);
    go_context_free(ctx);
}

static void test_closed_with_reason(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
    assert(relay && err == NULL);

    NostrFilters *fs = make_min_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, fs);
    assert(sub);

    const char *reason = "test closed";
    nostr_subscription_dispatch_closed(sub, reason);
    assert(atomic_load(&sub->priv->closed));

    GoContext *rxctx = ctx_with_timeout_ms(200);
    char *got = NULL;
    int rc = go_channel_receive_with_context(sub->closed_reason, (void **)&got, rxctx);
    assert(rc == 0);
    assert(got == reason || (got && strcmp(got, reason) == 0));

    nostr_subscription_unsubscribe(sub);
    nostr_subscription_free(sub);
    nostr_filters_free(fs);
    nostr_relay_free(relay);
    go_context_free(ctx);
}

static void test_unsubscribe_closes_events_channel(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
    assert(relay && err == NULL);

    NostrFilters *fs = make_min_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, fs);
    assert(sub);

    // Mark as live to simulate active subscription
    atomic_store(&sub->priv->live, true);

    // Unsubscribe triggers cancel; lifecycle thread will close events channel
    nostr_subscription_unsubscribe(sub);

    // Receive should promptly fail with -1 due to closed+empty
    GoContext *rxctx = ctx_with_timeout_ms(300);
    void *msg = NULL;
    int rc = go_channel_receive_with_context(sub->events, &msg, rxctx);
    assert(rc == -1);

    nostr_subscription_free(sub);
    nostr_filters_free(fs);
    nostr_relay_free(relay);
    go_context_free(ctx);
}

static void test_event_queue_full_drops(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
    assert(relay && err == NULL);

    NostrFilters *fs = make_min_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, fs);
    assert(sub);

    // Activate live to allow dispatch
    atomic_store(&sub->priv->live, true);

    // Capacity is 1; send two events non-blocking
    NostrEvent *e1 = make_dummy_event();
    NostrEvent *e2 = make_dummy_event();
    nostr_subscription_dispatch_event(sub, e1);
    nostr_subscription_dispatch_event(sub, e2); // should be dropped if queue full

    // We expect exactly one receive success, second receive should block; use try-receive via short timeout
    GoContext *rxctx1 = ctx_with_timeout_ms(200);
    void *got1 = NULL;
    int rc1 = go_channel_receive_with_context(sub->events, &got1, rxctx1);
    assert(rc1 == 0 && got1 != NULL);

    GoContext *rxctx2 = ctx_with_timeout_ms(100);
    void *got2 = NULL;
    int rc2 = go_channel_receive_with_context(sub->events, &got2, rxctx2);
    assert(rc2 == -1); // nothing else (second was dropped)

    // Cleanup
    nostr_subscription_unsubscribe(sub);
    nostr_subscription_free(sub);
    nostr_filters_free(fs);
    nostr_relay_free(relay);
    go_context_free(ctx);
}

int main(void) {
    test_eose_then_receive_signal();
    test_closed_with_reason();
    test_unsubscribe_closes_events_channel();
    test_event_queue_full_drops();
    printf("test_subscription_lifecycle: OK\n");
    return 0;
}
