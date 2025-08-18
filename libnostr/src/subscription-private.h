#ifndef NOSTR_SUBSCRIPTION_PRIVATE_H
#define NOSTR_SUBSCRIPTION_PRIVATE_H

#include "nostr-event.h"
#include "nostr-filter.h"
#include "go.h"
#include "nostr-relay.h"
#include <stdatomic.h>

typedef struct _SubscriptionPrivate {
    int counter;
    char *id;
    GoChannel *count_result;

    bool (*match)(NostrFilters*, NostrEvent*);
    _Atomic bool live;
    _Atomic bool eosed;
    _Atomic bool closed;
    _Atomic bool unsubbed;
    CancelFunc cancel;

    LongAdder *stored_event_counter;

    nsync_mu sub_mutex;
    // Wait for lifecycle thread to exit before freeing
    GoWaitGroup wg;
    // Instrumentation: counters for events
    _Atomic unsigned long long events_enqueued;
    _Atomic unsigned long long events_dropped;
} SubscriptionPrivate;

struct NostrSubscription; /* forward */
struct NostrSubscription *nostr_subscription_new(NostrRelay *relay, NostrFilters *filters);
void *nostr_subscription_start(void *arg);
void nostr_subscription_dispatch_event(struct NostrSubscription *sub, NostrEvent *event);
void nostr_subscription_dispatch_eose(struct NostrSubscription *sub);
void nostr_subscription_dispatch_closed(struct NostrSubscription *sub, const char *reason);

#endif // NOSTR_SUBSCRIPTION_PRIVATE_H
