#include "nostr.h"
#include "nostr_jansson.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main() {

    nostr_set_json_interface(jansson_impl);

    char *privateKey = generate_private_key();
    char *pubKey = get_public_key(privateKey);

    NostrEvent *event = create_event();
    assert(event != NULL);

    event->pubkey = strdup(pubKey);
    event->created_at = 1234567890;
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
    Error **err;
    Relay *relay = new_relay(ctx, "relay.sharegap.net", err);
    assert(relay != NULL);

    assert(relay_connect(relay, err));
    assert(relay_is_connected(relay));

    relay_publish(relay, event);

    Subscription *sub = create_subscription(relay, create_filters(1));
    subscription_fire(sub, err);
    subscription_unsub(sub);

    //relay_disconnect(relay);
    assert(!relay_is_connected(relay));

    free(id);
    free_event(event);
    free_filter(filter);
    free_relay(relay);
    go_context_free(ctx);
    free_subscription(sub);

    free(privateKey);
    free(pubKey);

    return 0;
}