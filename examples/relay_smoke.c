#include "nostr.h"
#include "nostr_jansson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Simple real-relay smoke test:
// 1) Set JSON interface
// 2) Connect to relay (arg[1] or default)
// 3) Subscribe with an empty filter
// 4) Wait briefly
// 5) Cleanly close

int main(int argc, char **argv) {
    const char *url = (argc > 1) ? argv[1] : "wss://relay.damus.io";

    // Configure JSON backend (jansson-based)
    nostr_set_json_interface(jansson_impl);
    nostr_json_init();

    Error *err = NULL;
    GoContext *ctx = go_context_background();

    fprintf(stderr, "[relay_smoke] Connecting to %s...\n", url);
    Relay *relay = new_relay(ctx, url, &err);
    if (!relay || err) {
        fprintf(stderr, "[relay_smoke] new_relay failed: %s\n", err ? err->message : "unknown");
        if (err) free_error(err);
        return 1;
    }

    if (!relay_connect(relay, &err)) {
        fprintf(stderr, "[relay_smoke] relay_connect failed: %s\n", err ? err->message : "unknown");
        if (err) free_error(err);
        free_relay(relay);
        go_context_free(ctx);
        return 2;
    }

    if (!relay_is_connected(relay)) {
        fprintf(stderr, "[relay_smoke] relay_is_connected returned false\n");
        relay_close(relay, &err);
        if (err) { free_error(err); err = NULL; }
        free_relay(relay);
        go_context_free(ctx);
        return 3;
    }

    // Build a minimal filter
    Filter *filter = create_filter();
    // Example: request recent text notes (kind 1) - optional
    // int_array_add(&filter->kinds, 1);

    Filters filters = { .filters = filter };

    fprintf(stderr, "[relay_smoke] Subscribing...\n");
    if (!relay_subscribe(relay, ctx, &filters, &err)) {
        fprintf(stderr, "[relay_smoke] relay_subscribe failed: %s\n", err ? err->message : "unknown");
        if (err) { free_error(err); err = NULL; }
        // Continue to close gracefully
    }

    // Brief wait to allow any frames to arrive; we don't parse here
    sleep(2);

    fprintf(stderr, "[relay_smoke] Closing...\n");
    relay_close(relay, &err);
    if (err) { fprintf(stderr, "[relay_smoke] relay_close error: %s\n", err->message); free_error(err); }

    free_filter(filter);
    free_relay(relay);
    go_context_free(ctx);

    // Cleanup JSON backend
    nostr_json_cleanup();

    fprintf(stderr, "[relay_smoke] Done.\n");
    return 0;
}
