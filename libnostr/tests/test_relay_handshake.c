/* A successful HTTP connection is not a successful WebSocket upgrade. */
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-filter.h"
#include "error.h"
#include "error_codes.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "select.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    abort(); \
} } while (0)

static atomic_uint reconnects;
static atomic_uint established_reconnects;

static void state_changed(NostrRelay *relay, NostrRelayConnectionState old_state,
                          NostrRelayConnectionState state, void *data)
{
    (void)old_state;
    (void)data;
    if (state == NOSTR_RELAY_STATE_CONNECTING)
        atomic_fetch_add(&reconnects, 1);
    if (state == NOSTR_RELAY_STATE_CONNECTED && atomic_load(&reconnects) > 1) {
        /* The initial connect API queues a dial. Reconnect, however, must not
         * announce success or re-fire subscriptions before its handshake. */
        CHECK(nostr_relay_is_established(relay));
        atomic_fetch_add(&established_reconnects, 1);
    }
}

static void await_eose(NostrSubscription *sub)
{
    GoSelectCase wait = { .op = GO_SELECT_RECEIVE,
                         .chan = nostr_subscription_get_eose_channel(sub) };
    CHECK(go_select_timeout(&wait, 1, 10000).selected_case == 0);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    char url[256];
    Error *error = NULL;
    snprintf(url, sizeof(url), "%s/unavailable", argv[1]);
    NostrRelay *relay = nostr_relay_new(NULL, url, &error);
    CHECK(relay && !error);
    nostr_relay_set_auto_reconnect(relay, false);
    CHECK(nostr_relay_connect(relay, &error));
    CHECK(!nostr_relay_wait_established(relay, 10000, &error));
    CHECK(error && strstr(error->message, "could not connect"));
    CHECK(!nostr_relay_is_established(relay));
    free_error(error);
    error = NULL;
    nostr_relay_free(relay);

    snprintf(url, sizeof(url), "%s/reconnect", argv[1]);
    relay = nostr_relay_new(NULL, url, &error);
    CHECK(relay && !error);
    nostr_relay_set_state_callback(relay, state_changed, NULL);
    CHECK(nostr_relay_connect(relay, &error));
    CHECK(nostr_relay_wait_established(relay, 10000, &error));
    NostrFilter *filter = nostr_filter_new();
    CHECK(filter);
    nostr_filter_add_kind(filter, 1);
    filter->limit = 1;
    NostrFilters filters = { .filters = filter, .count = 1 };
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, &filters);
    CHECK(sub && nostr_subscription_fire(sub, &error));
    await_eose(sub);
    await_eose(sub); /* Only the third connection can answer the re-fired REQ. */
    CHECK(atomic_load(&reconnects) == 3);
    CHECK(atomic_load(&established_reconnects) == 1);
    nostr_subscription_unsubscribe(sub);
    nostr_subscription_close(sub, NULL);
    nostr_subscription_free(sub);
    nostr_relay_free(relay);
    nostr_filter_free(filter);

    snprintf(url, sizeof(url), "%s/publish-drop", argv[1]);
    relay = nostr_relay_new(NULL, url, &error);
    CHECK(relay && !error && nostr_relay_connect(relay, &error));
    CHECK(nostr_relay_wait_established(relay, 10000, &error));
    NostrEvent *event = nostr_event_new();
    CHECK(event);
    event->kind = 1;
    event->created_at = 1;
    nostr_event_set_content(event, "transport loss");
    const char *secret = "0000000000000000000000000000000000000000000000000000000000000001";
    char *pubkey = nostr_key_get_public(secret);
    CHECK(pubkey);
    nostr_event_set_pubkey(event, pubkey);
    free(pubkey);
    CHECK(nostr_event_sign(event, secret) == 0);
    CHECK(!nostr_relay_publish_and_wait(relay, event, 10000, &error));
    CHECK(error && error->code == ERR_WEBSOCKET_CLOSED);
    free_error(error);
    nostr_event_free(event);
    nostr_relay_free(relay);
    puts("HTTP rejection and handshake-gated reconnect passed");
    return 0;
}
