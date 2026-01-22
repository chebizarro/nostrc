#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>

#include "go.h"
#include "nostr-event.h"
#include "error.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-filter.h"

#include "../libnostr/src/subscription-private.h"

static NostrFilters *make_min_filters(void) {
    NostrFilters *fs = (NostrFilters *)malloc(sizeof(NostrFilters));
    memset(fs, 0, sizeof(NostrFilters));
    fs->filters = (NostrFilter *)malloc(sizeof(NostrFilter));
    memset(fs->filters, 0, sizeof(NostrFilter));
    return fs;
}

int main(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
    assert(relay && err == NULL);

    NostrFilters *filters = make_min_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, filters);
    assert(sub);

    // Simulate that the subscription is live and receiving
    atomic_store(&sub->priv->live, true);

    // Dispatch some events and then immediately unsubscribe mid-stream
    for (int i = 0; i < 5; i++) {
        NostrEvent *ev = nostr_event_new();
        ev->kind = 1;
        ev->content = strdup("payload");
        nostr_subscription_dispatch_event(sub, ev);
    }

    // Now unsubscribe; lifecycle should cancel, close channels, and may emit CLOSED locally
    nostr_subscription_unsubscribe(sub);

    // Wait a bit for the lifecycle thread to process the cancellation and close channels
    usleep(100000);  // 100ms should be enough for the lifecycle thread to exit

    // Events channel should become closed/drained shortly; observe -1 receive consistently
    // Note: go_channel_receive_with_context with deadline doesn't properly wake up on timeout
    // due to nsync_cv_wait blocking, so we drain events using non-blocking try_receive
    bool events_closed = false;
    for (int i = 0; i < 100; i++) {
        void *tmp = NULL;
        int rc_probe = go_channel_try_receive(sub->events, &tmp);
        if (rc_probe == -1) {
            // Channel is closed and empty
            events_closed = true;
            break;
        }
        if (rc_probe == 0 && tmp) {
            // Successfully received an event, free it and continue draining
            nostr_event_free((NostrEvent *)tmp);
        }
        // rc_probe == 1 means channel empty but not closed, wait and retry
        usleep(10000);  // 10ms between retries
    }
    assert(events_closed);

    // Further receives on events should fail as channel is closed and drained
    void *msg = NULL;
    int rc_ev = go_channel_try_receive(sub->events, &msg);
    assert(rc_ev == -1);  // Should return -1 since channel is closed and empty

    nostr_subscription_free(sub);
    nostr_filters_free(filters);
    nostr_relay_free(relay);
    go_context_free(ctx);
    printf("test_relay_unsubscribe: OK\n");
    return 0;
}
