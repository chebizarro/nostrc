#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>

#include "go.h"
#include "nostr-event.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-filter.h"

// Drive internals
#include "../libnostr/src/subscription-private.h"

static NostrFilters *make_min_filters(void) {
    NostrFilters *fs = nostr_filters_new();
    nostr_filters_add(fs, nostr_filter_new());
    return fs;
}

static NostrEvent *make_dummy_event(int i) {
    NostrEvent *ev = nostr_event_new();
    ev->kind = 1;
    char *s = (char*)malloc(32);
    snprintf(s, 32, "ev-%d", i);
    ev->content = s;
    return ev;
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

int main(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);
    // Set capacity to 1 for this test to verify backpressure/drop behavior
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

    // Rapidly enqueue more events than capacity; subscription should drop extra without deadlock.
    // We also dispatch EOSE and ensure it is still delivered.
    for (int i = 0; i < 50; i++) {
        nostr_subscription_dispatch_event(sub, make_dummy_event(i));
    }
    nostr_subscription_dispatch_eose(sub);

    // We should be able to read at least one event (capacity=1)
    void *got = NULL;
    GoChannel *ev_ch = nostr_subscription_get_events_channel(sub);
    int rc1 = poll_receive(ev_ch, &got, 200);
    assert(rc1 == 0 && got != NULL);
    nostr_event_free((NostrEvent*)got);

    // Drain until empty; most events should have been dropped (capacity=1)
    got = NULL;
    int rc2 = poll_receive(ev_ch, &got, 100);
    assert(rc2 == 1);  // Timeout - no more events (were dropped)

    // EOSE must still arrive even if events queue was full/dropping
    void *sig = NULL;
    int rc3 = poll_receive(sub->end_of_stored_events, &sig, 200);
    assert(rc3 == 0);

    // Verify many events were dropped
    unsigned long long dropped = atomic_load(&sub->priv->events_dropped);
    assert(dropped >= 48);  // At least 48 of 50 should be dropped (capacity=1)

    nostr_subscription_unsubscribe(sub);
    usleep(100000);  // Give lifecycle thread time to exit
    nostr_subscription_free(sub);
    nostr_filters_free(fs);
    nostr_relay_free(relay);
    go_context_free(ctx);

    // Reset capacity for other tests
    unsetenv("NOSTR_SUB_EVENTS_CAP");

    printf("test_subscription_backpressure: OK\n");
    return 0;
}
