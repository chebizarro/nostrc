#include "nostr.h"
#include "nostr_jansson.h"
#include "nostr-event.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {

    nostr_set_json_interface(jansson_impl);

    char *privateKey = generate_private_key();
    char *pubKey = get_public_key(privateKey);

    NostrEvent *event = nostr_event_new();
    assert(event != NULL);

    event->pubkey = strdup(pubKey);
    event->created_at = Now();
    event->kind = 1;
    event->content = strdup("Hello, Nostr!");

    const char *id = nostr_event_get_id(event);
    assert(id != NULL);

    int res = nostr_event_sign(event, privateKey);
    assert(res == 0);

    bool verified = nostr_event_check_signature(event);
    assert(verified);

    Filter *filter = create_filter();
    string_array_add(&filter->authors, strdup(pubKey));

    bool matches = filter_matches(filter, event);
    assert(matches);

    GoContext *ctx = go_context_background();
    Error *err = NULL;
    Relay *relay = nostr_relay_new(ctx, "ws://192.168.1.149:8081", &err);
    assert(relay != NULL);
    assert(err == NULL);

    assert(nostr_relay_connect(relay, &err));
    assert(nostr_relay_is_connected(relay));

    nostr_relay_publish(relay, event);

    Filters filters = {
        .filters = filter
    };

    Subscription *sub = nostr_relay_prepare_subscription(relay, ctx, &filters);
    nostr_subscription_fire(sub, &err);
    // Immediately unsubscribe; don't assume this disconnects the relay
    nostr_subscription_unsubscribe(sub);

    // Explicitly close the relay to avoid background waits
    nostr_relay_close(relay, &err);

    /* id is owned by event; do not free */
    nostr_event_free(event);
    free_filter(filter);
    nostr_subscription_free(sub);
    nostr_relay_free(relay);
    go_context_free(ctx);

    free(privateKey);
    free(pubKey);

    return 0;
}