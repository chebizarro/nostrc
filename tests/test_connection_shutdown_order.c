#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "go.h"
#include "relay.h"

// Internals we need to validate invariants non-invasively
#include "../libnostr/src/relay-private.h"

int main(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    Error *err = NULL;
    GoContext *ctx = go_context_background();

    Relay *relay = new_relay(ctx, "wss://example.invalid", &err);
    assert(relay && err == NULL);

    // Connect starts workers (writer + message_loop). In test mode, message_loop exits promptly.
    bool ok = relay_connect(relay, &err);
    assert(ok && err == NULL);

    // Immediately close. This should:
    //  - cancel context
    //  - close write/close queues
    //  - wait for workers to finish
    //  - snapshot connection and set relay->connection=NULL
    //  - close connection and free its internals and channels safely
    ok = relay_close(relay, &err);
    assert(ok && err == NULL);

    // Verify snapshot cleared the connection pointer
    assert(relay->connection == NULL);

    // Verify write_queue is closed (send should fail)
    int send_rc = go_channel_send(relay->priv->write_queue, (void*)0x1);
    assert(send_rc != 0);

    // Free the relay; should be safe after close and not hang
    free_relay(relay);
    go_context_free(ctx);

    printf("test_connection_shutdown_order: OK\n");
    return 0;
}
