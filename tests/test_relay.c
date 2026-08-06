#include "nostr-relay.h"
#include "nostr-subscription.h"
#include <assert.h>

void test_relay_initialization_and_cleanup() {
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    setenv("NOSTR_TEST_MODE", "1", 1);
    
    // Create relay
    NostrRelay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
    assert(relay != NULL);
    assert(err == NULL);

    // Free relay
    nostr_relay_free(relay);
    go_context_free(ctx);
}

void test_relay_connection_and_close() {
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    setenv("NOSTR_TEST_MODE", "1", 1);

    // Create relay
    NostrRelay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
    assert(relay != NULL);
    assert(err == NULL);

    // Connect the relay
    bool connected = nostr_relay_connect(relay, &err);
    assert(connected == true);
    assert(err == NULL);

    // Check if the relay is connected
    bool is_connected = nostr_relay_is_connected(relay);
    assert(is_connected == true);

    // Close the relay
    bool closed = nostr_relay_close(relay, &err);
    assert(closed == true);
    assert(err == NULL);

    // Free relay
    nostr_relay_free(relay);
    go_context_free(ctx);
}

void test_relay_subscription() {
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    setenv("NOSTR_TEST_MODE", "1", 1);

    // Create relay
    NostrRelay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
    assert(relay != NULL);
    assert(err == NULL);

    // Connect the relay
    bool connected = nostr_relay_connect(relay, &err);
    assert(connected == true);
    assert(err == NULL);

    // Create a filter for subscription
    NostrFilter *filter = nostr_filter_new();

    NostrFilters *filters = nostr_filters_new();
    nostr_filters_add(filters, filter);

    // Prepare and fire explicitly so the test retains ownership of the
    // subscription and can stop its lifecycle worker before relay teardown.
    NostrSubscription *subscription =
        nostr_relay_prepare_subscription(relay, ctx, filters);
    assert(subscription != NULL);
    bool subscribed = nostr_subscription_fire(subscription, &err);
    assert(subscribed == true);
    assert(err == NULL);

    nostr_subscription_unsubscribe(subscription);
    nostr_subscription_free(subscription);

    // Close the relay
    nostr_relay_close(relay, &err);
    nostr_relay_free(relay);
    nostr_filters_free(filters);
    go_context_free(ctx);
}

void test_relay_write() {
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    setenv("NOSTR_TEST_MODE", "1", 1);

    // Create relay
    NostrRelay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
    assert(relay != NULL);
    assert(err == NULL);

    // Connect the relay
    bool connected = nostr_relay_connect(relay, &err);
    assert(connected == true);
    assert(err == NULL);

    // Write a message and wait for the offline writer to finish. Closing
    // the relay with an in-flight request made this test scheduler-dependent.
    GoChannel *write_channel = nostr_relay_write(relay, "test message");
    assert(write_channel != NULL);
    Error *write_err = NULL;
    assert(go_channel_receive(write_channel, (void **)&write_err) == 0);
    assert(write_err == NULL);
    go_channel_unref(write_channel);

    // Close the relay
    nostr_relay_close(relay, &err);
    nostr_relay_free(relay);
    go_context_free(ctx);
}

int main() {

    test_relay_initialization_and_cleanup();
    test_relay_connection_and_close();
    test_relay_subscription();
    test_relay_write();

    return 0;
}