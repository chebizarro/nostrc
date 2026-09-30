#ifndef NOSTR_SUBSCRIPTION_PRIVATE_H
#define NOSTR_SUBSCRIPTION_PRIVATE_H

#include "nostr-event.h"
#include "nostr-filter.h"
#include "go.h"
#include "nostr-relay.h"
#include <stdatomic.h>
#include <stdint.h>

/* ========================================================================
 * Queue Capacity Configuration (nostrc-3g8)
 * ======================================================================== */

#define NOSTR_QUEUE_CAPACITY_DEFAULT 4096   /* Default queue size */
#define NOSTR_QUEUE_CAPACITY_MIN     256    /* Minimum queue size */
#define NOSTR_QUEUE_CAPACITY_MAX     16384  /* Maximum queue size */
#define NOSTR_QUEUE_GROW_THRESHOLD   80     /* Grow when utilization exceeds 80% */
#define NOSTR_QUEUE_SHRINK_THRESHOLD 25     /* Shrink when utilization below 25% */
#define NOSTR_QUEUE_SHRINK_DELAY_SEC 30     /* Wait 30s before shrinking */

/* Global adaptive capacity state - tracks historical usage for new subscriptions */
typedef struct {
    _Atomic uint32_t suggested_capacity;     /* Capacity to use for new subscriptions */
    _Atomic uint32_t max_observed_peak;      /* Highest peak depth seen across all subs */
    _Atomic int64_t last_capacity_adjust_us; /* When capacity was last adjusted */
} AdaptiveCapacityState;

/* Get the global adaptive capacity state */
AdaptiveCapacityState *nostr_subscription_get_adaptive_state(void);

/* Suggest capacity for a new subscription based on historical usage */
uint32_t nostr_subscription_suggest_capacity(void);

/* Report peak usage to adaptive state (called periodically or on high utilization) */
void nostr_subscription_report_peak_usage(uint32_t peak_depth, uint32_t capacity);

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

/* Fixed-size FIFO hash cache. An evicted ID may be delivered again on replay,
 * but an unseen same-second ID is never rejected by the dedup cache. */
#define SEEN_CURSOR_CAPACITY 1024
#define SEEN_CURSOR_BUCKETS 2048

typedef struct {
    char id[65];
    uint16_t next; /* index + 1; zero means end of chain */
} SeenCursorEvent;

typedef struct {
    SeenCursorEvent entries[SEEN_CURSOR_CAPACITY];
    uint16_t buckets[SEEN_CURSOR_BUCKETS];
    uint16_t count;
    uint16_t next_evict;
} SeenCursorEvents;

typedef struct _SubscriptionPrivate {
    int counter;
    char *id;
    GoChannel *count_result;

    bool (*match)(NostrFilters*, NostrEvent*);
    _Atomic bool live;
    _Atomic bool eosed;
    _Atomic bool closed;
    _Atomic bool unsubbed;
    _Atomic bool events_channel_closed;
    _Atomic int64_t last_seen_created_at;
    SeenCursorEvents *seen_cursor_events; /* guarded by sub_mutex */
    int64_t replay_boundary_created_at;  /* current REQ's inclusive since, guarded by sub_mutex */
    SeenCursorEvents *replay_boundary_events; /* guarded by sub_mutex */
    CancelFunc cancel;

    /* Refcount for safe concurrent access (nostrc-nr96).
     * Starts at 1, incremented by ref(), decremented by unref().
     * Actual free happens when refcount drops to 0. */
    _Atomic int refcount;

    LongAdder *stored_event_counter;

    nsync_mu sub_mutex;
    // Wait for lifecycle thread to exit before freeing
    GoWaitGroup wg;

    // Queue health metrics (nostrc-sjv)
    QueueMetrics metrics;

    /* nostrc-prqu.4: set once the subscription has been inserted into
     * relay->subscriptions (the map message_loop dispatches EVENT/EOSE/CLOSED
     * from). Registration is one-shot: once nostr_subscription_free() or the
     * async cleanup removes the entry, a late refire must not resurrect it. */
    _Atomic bool registered;

    /* nostrc-jw23: sub->filters came through nostr_subscription_set_filters()
     * ("takes full ownership") and are freed with the subscription. Filters
     * given to nostr_subscription_new() are borrowed. Guarded by sub_mutex. */
    bool owns_filters;
} SubscriptionPrivate;

struct NostrSubscription; /* forward */
struct NostrSubscription *nostr_subscription_new(NostrRelay *relay, NostrFilters *filters);
void *nostr_subscription_start(void *arg);
void nostr_subscription_dispatch_event(struct NostrSubscription *sub, NostrEvent *event);
void nostr_subscription_dispatch_eose(struct NostrSubscription *sub);
int64_t nostr_subscription_prepare_refire(struct NostrSubscription *sub);
void nostr_subscription_dispatch_closed(struct NostrSubscription *sub, const char *reason);
bool nostr_subscription_refire_since(struct NostrSubscription *sub, int64_t since, Error **err);
/* Insert @sub into its relay's dispatch map (idempotent, one-shot; takes the
 * relay priv mutex). Called by nostr_relay_prepare_subscription() and lazily
 * by nostr_subscription_fire() for subscriptions made with
 * nostr_subscription_new() (nostrc-prqu.4). */
void nostr_subscription_register_with_relay(struct NostrSubscription *sub);

#endif // NOSTR_SUBSCRIPTION_PRIVATE_H
