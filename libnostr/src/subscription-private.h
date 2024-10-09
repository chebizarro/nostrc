#ifndef NOSTR_SUBSCRIPTION_PRIVATE_H
#define NOSTR_SUBSCRIPTION_PRIVATE_H

#include "event.h"
#include "filter.h"
#include "go.h"
#include "relay.h"
#include "subscription.h"
#include <stdatomic.h>

typedef struct _SubscriptionPrivate {
    char *label;
    int counter;

    GoChannel *countResult;
    GoContext *context;

    _Atomic bool live;
    _Atomic bool eosed;
    _Atomic bool closed;
    pthread_mutex_t sub_mutex;
    pthread_t thread;
} SubscriptionPrivate;

Subscription *create_subscription(Relay *relay, Filters *filters, const char *label);
void subscription_start(void *arg);
void subscription_dispatch_event(Subscription *sub, NostrEvent *event);
void subscription_dispatch_eose(Subscription *sub);
void subscription_dispatch_closed(Subscription *sub, const char *reason);

#endif // NOSTR_SUBSCRIPTION_PRIVATE_H