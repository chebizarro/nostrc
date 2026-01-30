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

// Helper: poll for channel to have data, using try_receive with timeout
// Returns: 0 = got data, 1 = timeout (empty but open), -1 = closed
static int poll_receive(GoChannel *ch, void **out, int max_ms) {
    int elapsed = 0;
    while (elapsed < max_ms) {
        int rc = go_channel_try_receive(ch, out);
        if (rc == 0) return 0;  // Got data
        // rc == -1 means either empty or closed; check which
        if (go_channel_is_closed(ch)) return -1;  // Actually closed
        // Otherwise just empty, keep polling
        usleep(10000);  // 10ms
        elapsed += 10;
    }
    return 1;  // Timeout (channel open but no data)
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

    // Receive on end_of_stored_events - data was sent so should succeed
    void *sig = NULL;
    int rc = poll_receive(sub->end_of_stored_events, &sig, 200);
    assert(rc == 0);

    nostr_subscription_unsubscribe(sub);
    nostr_subscription_unsubscribe(sub);  // Idempotent - should be fine
    usleep(100000);  // Give lifecycle thread time to exit
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

    // Receive the closed reason - data was sent so should succeed
    char *got = NULL;
    int rc = poll_receive(sub->closed_reason, (void **)&got, 200);
    assert(rc == 0);
    assert(got != NULL && strcmp(got, reason) == 0);
    free(got);  // dispatch_closed makes a copy

    nostr_subscription_unsubscribe(sub);
    usleep(100000);
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

    // Wait for lifecycle thread to close the channel
    usleep(100000);

    // Events channel should be closed (empty + closed = -1)
    void *msg = NULL;
    int rc = go_channel_try_receive(sub->events, &msg);
    assert(rc == -1);

    nostr_subscription_free(sub);
    nostr_filters_free(fs);
    nostr_relay_free(relay);
    go_context_free(ctx);
}

static void test_event_queue_full_drops(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);
    // Set capacity to 1 for this test to verify drop behavior
    setenv("NOSTR_SUB_EVENTS_CAP", "1", 1);
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

    // First receive should succeed (got e1)
    void *got1 = NULL;
    int rc1 = poll_receive(sub->events, &got1, 200);
    assert(rc1 == 0 && got1 != NULL);
    nostr_event_free((NostrEvent *)got1);

    // Second receive should timeout (e2 was dropped)
    void *got2 = NULL;
    int rc2 = poll_receive(sub->events, &got2, 100);
    assert(rc2 == 1);  // Timeout - no more events

    // Verify drop counter was incremented (use public API)
    assert(nostr_subscription_events_dropped(sub) >= 1);

    // Cleanup
    nostr_subscription_unsubscribe(sub);
    usleep(100000);
    nostr_subscription_free(sub);
    nostr_filters_free(fs);
    nostr_relay_free(relay);
    go_context_free(ctx);

    // Reset capacity for other tests
    unsetenv("NOSTR_SUB_EVENTS_CAP");
}

int main(void) {
    test_eose_then_receive_signal();
    test_closed_with_reason();
    test_unsubscribe_closes_events_channel();
    test_event_queue_full_drops();
    printf("test_subscription_lifecycle: OK\n");
    return 0;
}
