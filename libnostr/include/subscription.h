#ifndef NOSTR_SUBSCRIPTION_H
#define NOSTR_SUBSCRIPTION_H

#include "filter.h"
#include "relay.h"

typedef struct _SubscriptionPrivate SubscriptionPrivate;

typedef struct Subscription {
    SubscriptionPrivate *priv;
	char *id;
    Relay *relay;
    Filters *filters;
    GoChannel *events;
    GoChannel *closed_reason;
} Subscription;

Subscription *create_subscription(Relay *relay, Filters *filters, const char *label) MALLOC;
void free_subscription(Subscription *sub);
char *subscription_get_id(Subscription *sub);
void subscription_unsub(Subscription *sub);
void subscription_close(Subscription *sub);
void subscription_sub(Subscription *sub, Filters *filters);
void subscription_fire(Subscription *sub);

#endif // NOSTR_SUBSCRIPTION_H