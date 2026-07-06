/* Regression tests for Item B libnostr networking reliability audit fixes.
 *
 * Covers:
 *   nostrc-atf  nostr_relay_connect frees the freshly-created connection when
 *               a later startup error occurs (ASan/LSan-friendly leak test)
 *   nostrc-e80  nostr_connection_write_message accepts err == NULL on guarded
 *               error/success paths without dereferencing it
 *
 * Thread-start failure and live LWS service stalls are covered by code
 * inspection: they depend on resource exhaustion/service-loop failure that is
 * not deterministic enough for a unit test in this suite.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "nostr-connection.h"
#include "nostr-relay.h"
#include "error.h"
#include "go.h"
#include "../src/relay-private.h"

static void clear_error(Error **err) {
    if (err && *err) {
        free_error(*err);
        *err = NULL;
    }
}

static void test_relay_connect_error_path_discards_connection(void) {
    assert(setenv("NOSTR_TEST_MODE", "1", 1) == 0);

    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(go_context_background(), "wss://audit.invalid", &err);
    assert(relay != NULL);
    assert(err == NULL);

    /* Force nostr_relay_connect() to fail after nostr_connection_new() succeeds.
     * Pre-fix this left relay->connection pointing at the newly-created
     * NostrConnection and leaked it on this return path. */
    GoContext *ctx = relay->priv->connection_context;
    CancelFunc cancel = relay->priv->connection_context_cancel;
    if (cancel) cancel(ctx);
    if (ctx) go_context_unref(ctx);
    relay->priv->connection_context = NULL;
    relay->priv->connection_context_cancel = NULL;

    bool ok = nostr_relay_connect(relay, &err);
    assert(ok == false);
    assert(err != NULL);
    assert(relay->connection == NULL);
    clear_error(&err);

    nostr_relay_free(relay);
    unsetenv("NOSTR_TEST_MODE");
    printf("  [ok] relay connect error path discards connection\n");
}

static void test_write_message_allows_null_error_out(void) {
    assert(setenv("NOSTR_TEST_MODE", "1", 1) == 0);

    NostrConnection *conn = nostr_connection_new("wss://audit.invalid");
    assert(conn != NULL);

    /* Success/test-mode path with err == NULL must be a no-op. */
    nostr_connection_write_message(conn, NULL, "[]", NULL);

    GoChannel *recv_ch = conn->recv_channel;
    GoChannel *send_ch = conn->send_channel;
    nostr_connection_close(conn);
    if (recv_ch) go_channel_free(recv_ch);
    if (send_ch) go_channel_free(send_ch);

    /* Guarded invalid-argument path with err == NULL must also not crash. */
    nostr_connection_write_message(NULL, NULL, "[]", NULL);

    unsetenv("NOSTR_TEST_MODE");
    printf("  [ok] write_message tolerates NULL err out\n");
}

int main(void) {
    printf("Running libnostr networking audit regression tests...\n");
    test_relay_connect_error_path_discards_connection();
    test_write_message_allows_null_error_out();
    printf("All networking audit tests passed.\n");
    return 0;
}
