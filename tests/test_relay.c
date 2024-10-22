#include "relay.h"
#include <assert.h>

void test_relay_initialization_and_cleanup() {
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    
    // Create relay
    Relay *relay = new_relay(ctx, "localhost:8080", &err);
    assert(relay != NULL);
    assert(err == NULL);

    // Free relay
    free_relay(relay);
    go_context_free(ctx);
}

void test_relay_connection_and_close() {
    Error *err = NULL;
    GoContext *ctx = go_context_background();

    // Create relay
    Relay *relay = new_relay(ctx, "relay.sharegap.net", &err);
    assert(relay != NULL);
    assert(err == NULL);

    // Connect the relay
    bool connected = relay_connect(relay, &err);
    //assert(connected == true);
    assert(err == NULL);

    // Check if the relay is connected
    bool is_connected = relay_is_connected(relay);
    assert(is_connected == true);

    // Close the relay
    bool closed = relay_close(relay, &err);
    assert(closed == true);
    assert(err == NULL);

    // Free relay
    free_relay(relay);
    go_context_free(ctx);
}

void test_relay_subscription() {
    Error *err = NULL;
    GoContext *ctx = go_context_background();

    // Create relay
    Relay *relay = new_relay(ctx, "localhost:8080", &err);
    assert(relay != NULL);
    assert(err == NULL);

    // Connect the relay
    bool connected = relay_connect(relay, &err);
    assert(connected == true);
    assert(err == NULL);

    // Create a filter for subscription
    Filter *filter = create_filter();
    string_array_add(&filter->ids, "some_event_id");

    Filters filters = { .filters = filter};

    // Subscribe
    bool subscribed = relay_subscribe(relay, ctx, &filters, &err);
    assert(subscribed == true);
    assert(err == NULL);

    // Close the relay
    relay_close(relay, &err);
    free_relay(relay);
    go_context_free(ctx);
}

void test_relay_write() {
    Error *err = NULL;
    GoContext *ctx = go_context_background();

    // Create relay
    Relay *relay = new_relay(ctx, "localhost:8080", &err);
    assert(relay != NULL);
    assert(err == NULL);

    // Connect the relay
    bool connected = relay_connect(relay, &err);
    assert(connected == true);
    assert(err == NULL);

    // Write a message
    GoChannel *write_channel = relay_write(relay, "test message");
    assert(write_channel != NULL);

    // Close the relay
    relay_close(relay, &err);
    free_relay(relay);
    go_context_free(ctx);
}

int main() {

    //test_relay_initialization_and_cleanup();
    test_relay_connection_and_close();
    //test_relay_subscription();
    //test_relay_write();

    return 0;
}