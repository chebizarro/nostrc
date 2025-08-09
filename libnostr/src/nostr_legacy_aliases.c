#include "relay.h"
#include "nostr-relay.h"

// Temporary legacy compatibility layer mapping old API to new nostr_relay_* names.
// This allows existing tests/examples to link while call sites are migrated.

Relay *new_relay(GoContext *context, const char *url, Error **err) {
    return (Relay *)nostr_relay_new(context, url, err);
}

void free_relay(Relay *relay) {
    nostr_relay_free(relay);
}

bool relay_connect(Relay *relay, Error **err) {
    return nostr_relay_connect(relay, err);
}

void relay_disconnect(Relay *relay) {
    nostr_relay_disconnect(relay);
}

bool relay_close(Relay *relay, Error **err) {
    return nostr_relay_close(relay, err);
}

bool relay_is_connected(Relay *relay) {
    return nostr_relay_is_connected(relay);
}

bool relay_subscribe(Relay *relay, GoContext *ctx, Filters *filters, Error **err) {
    return nostr_relay_subscribe(relay, ctx, filters, err);
}

Subscription *relay_prepare_subscription(Relay *relay, GoContext *ctx, Filters *filters) {
    return nostr_relay_prepare_subscription(relay, ctx, filters);
}

void relay_publish(Relay *relay, NostrEvent *event) {
    nostr_relay_publish(relay, event);
}

void relay_auth(Relay *relay, void (*sign)(NostrEvent *, Error **), Error **err) {
    nostr_relay_auth(relay, sign, err);
}

int64_t relay_count(Relay *relay, GoContext *ctx, Filter *filter, Error **err) {
    return nostr_relay_count(relay, ctx, filter, err);
}

GoChannel *relay_write(Relay *relay, char *msg) {
    return nostr_relay_write(relay, msg);
}
