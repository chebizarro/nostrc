#include "nostr-relay.h"
#include <assert.h>

void test_relay_initialization_and_cleanup() {
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    setenv("NOSTR_TEST_MODE", "1", 1);
    
    // Create relay
    Relay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
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
    Relay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
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
    Relay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
    assert(relay != NULL);
    assert(err == NULL);

    // Connect the relay
    bool connected = nostr_relay_connect(relay, &err);
    assert(connected == true);
    assert(err == NULL);

    // Create a filter for subscription
    Filter *filter = create_filter();

    Filters filters = { .filters = filter};

    // Subscribe
    bool subscribed = nostr_relay_subscribe(relay, ctx, &filters, &err);
    assert(subscribed == true);
    assert(err == NULL);

    // Close the relay
    nostr_relay_close(relay, &err);
    nostr_relay_free(relay);
    free_filter(filter);
    go_context_free(ctx);
}

void test_relay_write() {
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    setenv("NOSTR_TEST_MODE", "1", 1);

    // Create relay
    Relay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
    assert(relay != NULL);
    assert(err == NULL);

    // Connect the relay
    bool connected = nostr_relay_connect(relay, &err);
    assert(connected == true);
    assert(err == NULL);

    // Write a message
    GoChannel *write_channel = nostr_relay_write(relay, "test message");
    assert(write_channel != NULL);

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