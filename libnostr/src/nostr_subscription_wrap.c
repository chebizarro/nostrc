#include "nostr-subscription.h"
#include "subscription.h"
#include "subscription-private.h"
#include "filter.h"
#include "error.h"
#include "channel.h"
#include "context.h"
#include <stdatomic.h>
#include <string.h>

/* Thin, GI-friendly wrappers around legacy Subscription API. */

NostrSubscription *nostr_subscription_new(NostrRelay *relay, NostrFilters *filters) {
    return create_subscription(relay, filters);
}

void nostr_subscription_free(NostrSubscription *sub) {
    if (!sub) return;
    free_subscription(sub);
}

void nostr_subscription_unsubscribe(NostrSubscription *sub) {
    if (!sub) return;
    subscription_unsub(sub);
}

void nostr_subscription_close(NostrSubscription *sub, Error **err) {
    if (!sub) return;
    subscription_close(sub, err);
}

const char *nostr_subscription_get_id_const(const NostrSubscription *sub) {
    if (!sub) return NULL;
    /* subscription_get_id returns a new string; use internal via priv if available */
    return sub->priv ? sub->priv->id : NULL;
}

char *nostr_subscription_get_id(NostrSubscription *sub) {
    if (!sub || !sub->priv || !sub->priv->id) return NULL;
    return strdup(sub->priv->id);
}

NostrRelay *nostr_subscription_get_relay(const NostrSubscription *sub) {
    if (!sub) return NULL;
    return sub->relay;
}

NostrFilters *nostr_subscription_get_filters(const NostrSubscription *sub) {
    if (!sub) return NULL;
    return sub->filters;
}

void nostr_subscription_set_filters(NostrSubscription *sub, NostrFilters *filters) {
    if (!sub) return;
    if (sub->filters == filters) return;
    if (sub->filters) free_filters(sub->filters);
    sub->filters = filters;
}

GoChannel *nostr_subscription_get_events_channel(const NostrSubscription *sub) {
    if (!sub) return NULL;
    return sub->events;
}

GoChannel *nostr_subscription_get_eose_channel(const NostrSubscription *sub) {
    if (!sub) return NULL;
    return sub->end_of_stored_events;
}

GoChannel *nostr_subscription_get_closed_channel(const NostrSubscription *sub) {
    if (!sub) return NULL;
    return sub->closed_reason;
}

GoContext *nostr_subscription_get_context(const NostrSubscription *sub) {
    if (!sub) return NULL;
    return sub->context;
}

bool nostr_subscription_is_live(const NostrSubscription *sub) {
    if (!sub || !sub->priv) return false;
    return atomic_load(&sub->priv->live);
}

bool nostr_subscription_is_eosed(const NostrSubscription *sub) {
    if (!sub || !sub->priv) return false;
    return atomic_load(&sub->priv->eosed);
}

bool nostr_subscription_is_closed(const NostrSubscription *sub) {
    if (!sub || !sub->priv) return false;
    return atomic_load(&sub->priv->closed);
}

bool nostr_subscription_subscribe(NostrSubscription *sub, NostrFilters *filters, Error **err) {
    return subscription_sub(sub, filters, err);
}

bool nostr_subscription_fire(NostrSubscription *sub, Error **err) {
    return subscription_fire(sub, err);
}
