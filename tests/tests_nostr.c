#include "nostr.h"
#include "nostr_jansson.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {

    nostr_set_json_interface(jansson_impl);

    char *privateKey = generate_private_key();
    char *pubKey = get_public_key(privateKey);

    NostrEvent *event = create_event();
    assert(event != NULL);

    event->pubkey = strdup(pubKey);
    event->created_at = Now();
    event->kind = 1;
    event->content = strdup("Hello, Nostr!");

    char *id = event_get_id(event);
    assert(id != NULL);

    int res = event_sign(event, privateKey);
    assert(res == 0);

    bool verified = event_check_signature(event);
    assert(verified);

    Filter *filter = create_filter();
    string_array_add(&filter->authors, strdup(pubKey));

    bool matches = filter_matches(filter, event);
    assert(matches);

    GoContext *ctx = go_context_background();
    Error *err = NULL;
    Relay *relay = new_relay(ctx, "ws://192.168.1.149:8081", &err);
    assert(relay != NULL);
    assert(err == NULL);

    assert(relay_connect(relay, &err));
    assert(relay_is_connected(relay));

    relay_publish(relay, event);

    Filters filters = {
        .filters = filter
    };

    Subscription *sub = create_subscription(relay, &filters);
    subscription_fire(sub, &err);
    // Immediately unsubscribe; don't assume this disconnects the relay
    subscription_unsub(sub);

    // Explicitly close the relay to avoid background waits
    relay_close(relay, &err);

    free(id);
    free_event(event);
    free_filter(filter);
    free_subscription(sub);
    free_relay(relay);
    go_context_free(ctx);

    free(privateKey);
    free(pubKey);

    return 0;
}