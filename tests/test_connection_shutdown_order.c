#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "go.h"
#include "nostr-relay.h"

// Internals we need to validate invariants non-invasively
#include "../libnostr/src/relay-private.h"

int main(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    Error *err = NULL;
    GoContext *ctx = go_context_background();

    NostrRelay *relay = nostr_relay_new(ctx, "wss://example.invalid", &err);
    assert(relay && err == NULL);

    // Connect starts workers (writer + message_loop). In test mode, message_loop exits promptly.
    bool ok = nostr_relay_connect(relay, &err);
    assert(ok && err == NULL);

    // Immediately close. This should:
    //  - cancel context
    //  - close write/close queues
    //  - wait for workers to finish
    //  - snapshot connection and set relay->connection=NULL
    //  - close connection and free its internals and channels safely
    ok = nostr_relay_close(relay, &err);
    assert(ok && err == NULL);

    // Verify snapshot cleared the connection pointer
    assert(relay->connection == NULL);

    // Verify write_queue is closed (send should fail)
    int send_rc = go_channel_send(relay->priv->write_queue, (void*)0x1);
    assert(send_rc != 0);

    GoChannel *write_ch = nostr_relay_write(relay, "closed-queue-probe");
    assert(write_ch != NULL);

    Error *write_err = NULL;
    assert(go_channel_receive(write_ch, (void **)&write_err) == 0);
    assert(write_err != NULL);
    free_error(write_err);

    for (int i = 0; i < 100 &&
         atomic_load_explicit(&write_ch->refs, memory_order_acquire) != 1; i++) {
        usleep(1000);
    }
    assert(atomic_load_explicit(&write_ch->refs, memory_order_acquire) == 1);
    go_channel_close(write_ch);
    go_channel_unref(write_ch);

    // Free the relay; should be safe after close and not hang
    nostr_relay_free(relay);
    go_context_free(ctx);

    printf("test_connection_shutdown_order: OK\n");
    return 0;
}
