#ifndef NOSTR_SUBSCRIPTION_H
#define NOSTR_SUBSCRIPTION_H

#include "filter.h"
#include "relay.h"

typedef struct _SubscriptionPrivate SubscriptionPrivate;

typedef struct _Subscription {
    SubscriptionPrivate *priv;
    Relay *relay;
    Filters *filters;
    GoChannel *events;
    GoChannel *end_of_stored_events;
    GoChannel *closed_reason;
    GoContext *context;
} Subscription;

Subscription *create_subscription(Relay *relay, Filters *filters);
void free_subscription(Subscription *sub);
char *subscription_get_id(Subscription *sub);
void subscription_unsub(Subscription *sub);
void subscription_close(Subscription *sub, Error **err);
bool subscription_sub(Subscription *sub, Filters *filters, Error **err);
bool subscription_fire(Subscription *sub, Error **err);

#endif // NOSTR_SUBSCRIPTION_H