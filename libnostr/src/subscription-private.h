#ifndef NOSTR_SUBSCRIPTION_PRIVATE_H
#define NOSTR_SUBSCRIPTION_PRIVATE_H

#include "nostr-event.h"
#include "nostr-filter.h"
#include "go.h"
#include "nostr-relay.h"
#include <stdatomic.h>
#include <stdint.h>

/**
 * QueueMetrics - Per-subscription queue health instrumentation
 *
 * Tracks queue state to establish baseline before optimization.
 * All atomic fields are safe to read from any thread.
 */
typedef struct {
    _Atomic uint64_t events_enqueued;      // Total events added to queue
    _Atomic uint64_t events_dequeued;      // Total events processed (consumer-reported)
    _Atomic uint64_t events_dropped;       // Total events dropped (queue full)
    _Atomic uint32_t current_depth;        // Current queue size
    _Atomic uint32_t peak_depth;           // High water mark
    uint32_t queue_capacity;               // Max queue size (immutable after init)
    _Atomic int64_t last_enqueue_time_us;  // Timestamp of last enqueue (microseconds)
    _Atomic int64_t last_dequeue_time_us;  // Timestamp of last dequeue (microseconds)
    _Atomic uint64_t total_wait_time_us;   // Cumulative time events spent in queue
    _Atomic int64_t created_time_us;       // When subscription was created
} QueueMetrics;

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

    // Queue health metrics (nostrc-sjv)
    QueueMetrics metrics;
} SubscriptionPrivate;

struct NostrSubscription; /* forward */
struct NostrSubscription *nostr_subscription_new(NostrRelay *relay, NostrFilters *filters);
void *nostr_subscription_start(void *arg);
void nostr_subscription_dispatch_event(struct NostrSubscription *sub, NostrEvent *event);
void nostr_subscription_dispatch_eose(struct NostrSubscription *sub);
void nostr_subscription_dispatch_closed(struct NostrSubscription *sub, const char *reason);

#endif // NOSTR_SUBSCRIPTION_PRIVATE_H
