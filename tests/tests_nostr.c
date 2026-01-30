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

    char *privateKey = nostr_key_generate_private();
    char *pubKey = nostr_key_get_public(privateKey);

    NostrEvent *event = nostr_event_new();
    assert(event != NULL);

    event->pubkey = strdup(pubKey);
    event->created_at = (NostrTimestamp)nostr_timestamp_now();
    event->kind = 1;
    event->content = strdup("Hello, Nostr!");

    char *id = nostr_event_get_id(event);
    assert(id != NULL);
    free(id);

    int res = nostr_event_sign(event, privateKey);
    assert(res == 0);

    bool verified = nostr_event_check_signature(event);
    assert(verified);

    NostrFilter *filter = nostr_filter_new();
    string_array_add(&filter->authors, strdup(pubKey));

    bool matches = nostr_filter_matches(filter, event);
    assert(matches);

    GoContext *ctx = go_context_background();
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(ctx, "ws://192.168.1.149:8081", &err);
    assert(relay != NULL);
    assert(err == NULL);

    assert(nostr_relay_connect(relay, &err));
    assert(nostr_relay_is_connected(relay));

    nostr_relay_publish(relay, event);

    NostrFilters filters = {
        .filters = filter
    };

    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, &filters);
    nostr_subscription_fire(sub, &err);
    // Immediately unsubscribe; don't assume this disconnects the relay
    nostr_subscription_unsubscribe(sub);

    // Explicitly close the relay to avoid background waits
    nostr_relay_close(relay, &err);

    /* id is owned by event; do not free */
    nostr_event_free(event);
    nostr_filter_free(filter);
    nostr_subscription_free(sub);
    nostr_relay_free(relay);
    go_context_free(ctx);

    free(privateKey);
    free(pubKey);

    return 0;
}