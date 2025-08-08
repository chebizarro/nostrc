#ifndef NOSTR_SUBSCRIPTION_PRIVATE_H
#define NOSTR_SUBSCRIPTION_PRIVATE_H

#include "event.h"
#include "filter.h"
#include "go.h"
#include "relay.h"
#include "subscription.h"
#include <stdatomic.h>

typedef struct _SubscriptionPrivate {
    int counter;
    char *id;
    GoChannel *count_result;

    bool (*match)(Filters*, NostrEvent*);
    _Atomic bool live;
    _Atomic bool eosed;
    _Atomic bool closed;
    CancelFunc cancel;

    LongAdder *stored_event_counter;

    nsync_mu sub_mutex;
    // Wait for lifecycle thread to exit before freeing
    GoWaitGroup wg;
} SubscriptionPrivate;

Subscription *create_subscription(Relay *relay, Filters *filters);
void *subscription_start(void *arg);
void subscription_dispatch_event(Subscription *sub, NostrEvent *event);
void subscription_dispatch_eose(Subscription *sub);
void subscription_dispatch_closed(Subscription *sub, const char *reason);

#endif // NOSTR_SUBSCRIPTION_PRIVATE_H
