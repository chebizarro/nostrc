#ifndef __NOSTR_SUBSCRIPTION_H__
#define __NOSTR_SUBSCRIPTION_H__

/* Canonical Nostr subscription API (GLib-friendly C interface). */

#include <stdbool.h>
#include <stdint.h>
#include "nostr-filter.h"
#include "nostr-relay.h"
#include "channel.h"  /* GoChannel */
#include "context.h"  /* GoContext */

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical NostrSubscription type */
typedef struct _SubscriptionPrivate SubscriptionPrivate; /* private struct */
typedef struct NostrSubscription {
    SubscriptionPrivate *priv;
    NostrRelay *relay;
    NostrFilters *filters;
    GoChannel *events;
    GoChannel *end_of_stored_events;
    GoChannel *closed_reason;
    GoContext *context;
} NostrSubscription;


/* GI-facing API (stable symbol names) */
NostrSubscription *nostr_subscription_new(NostrRelay *relay, NostrFilters *filters);
void               nostr_subscription_free(NostrSubscription *sub);
/* Returns newly allocated copy of ID (caller frees). */
char              *nostr_subscription_get_id(NostrSubscription *sub);
void               nostr_subscription_unsubscribe(NostrSubscription *sub);
void               nostr_subscription_close(NostrSubscription *sub, Error **err);
void               nostr_subscription_wait(NostrSubscription *sub);
bool               nostr_subscription_subscribe(NostrSubscription *sub, NostrFilters *filters, Error **err);
bool               nostr_subscription_fire(NostrSubscription *sub, Error **err);

/* Async cleanup API - non-blocking subscription cleanup with timeout */
typedef struct AsyncCleanupHandle AsyncCleanupHandle;

/**
 * nostr_subscription_free_async:
 * @sub: subscription to cleanup
 * @timeout_ms: maximum time to wait for cleanup (0 = no timeout)
 *
 * Initiates async cleanup of subscription. Returns immediately with a handle.
 * Cleanup happens in background with timeout. If timeout expires, subscription leaks.
 *
 * Returns: (transfer full): handle to track cleanup progress, or NULL on error
 */
AsyncCleanupHandle *nostr_subscription_free_async(NostrSubscription *sub, uint64_t timeout_ms);

/**
 * nostr_subscription_cleanup_wait:
 * @handle: cleanup handle from nostr_subscription_free_async
 * @timeout_ms: additional time to wait (0 = check status only)
 *
 * Wait for async cleanup to complete. Can be called multiple times.
 *
 * Returns: true if cleanup completed successfully, false if still in progress or timed out
 */
bool nostr_subscription_cleanup_wait(AsyncCleanupHandle *handle, uint64_t timeout_ms);

/**
 * nostr_subscription_cleanup_abandon:
 * @handle: (transfer full): cleanup handle to abandon
 *
 * Abandon cleanup attempt and leak the subscription. Frees the handle.
 * Use when timeout has expired and you want to stop waiting.
 */
void nostr_subscription_cleanup_abandon(AsyncCleanupHandle *handle);

/**
 * nostr_subscription_cleanup_is_complete:
 * @handle: cleanup handle
 *
 * Check if cleanup has completed (non-blocking).
 *
 * Returns: true if cleanup finished (success or timeout), false if still in progress
 */
bool nostr_subscription_cleanup_is_complete(AsyncCleanupHandle *handle);

/**
 * nostr_subscription_cleanup_timed_out:
 * @handle: cleanup handle
 *
 * Check if cleanup timed out (subscription was leaked).
 *
 * Returns: true if cleanup timed out and subscription was leaked
 */
bool nostr_subscription_cleanup_timed_out(AsyncCleanupHandle *handle);

/* Accessors for public fields/state (for future GObject properties) */

/* Identity */
/**
 * nostr_subscription_get_id_const:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): internal ID string
 */
const char *nostr_subscription_get_id_const(const NostrSubscription *sub);

/* Associations */
/**
 * nostr_subscription_get_relay:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): associated relay
 */
NostrRelay   *nostr_subscription_get_relay(const NostrSubscription *sub);
/**
 * nostr_subscription_get_filters:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): associated filters
 */
NostrFilters *nostr_subscription_get_filters(const NostrSubscription *sub);
/**
 * nostr_subscription_set_filters:
 * @sub: (nullable): subscription (no-op if NULL)
 * @filters: (transfer full) (nullable): new filters; previous freed if different
 *
 * Ownership: takes full ownership of @filters.
 */
void          nostr_subscription_set_filters(NostrSubscription *sub, NostrFilters *filters);

/* Channels (transfer none): producer owns them */
/**
 * nostr_subscription_get_events_channel:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): events channel owned by subscription
 */
GoChannel    *nostr_subscription_get_events_channel(const NostrSubscription *sub);
/**
 * nostr_subscription_get_eose_channel:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): EOSE notification channel
 */
GoChannel    *nostr_subscription_get_eose_channel(const NostrSubscription *sub);
/**
 * nostr_subscription_get_closed_channel:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): CLOSED reason channel; carries (const char*)
 */
GoChannel    *nostr_subscription_get_closed_channel(const NostrSubscription *sub);

/* Context */
/**
 * nostr_subscription_get_context:
 * @sub: (nullable): subscription
 *
 * Returns: (transfer none) (nullable): cancellation context for lifecycle
 */
GoContext    *nostr_subscription_get_context(const NostrSubscription *sub);

/* Lifecycle flags */
/**
 * nostr_subscription_is_live:
 * @sub: (nullable): subscription
 *
 * Returns: whether subscription is currently live
 */
bool          nostr_subscription_is_live(const NostrSubscription *sub);
/**
 * nostr_subscription_is_eosed:
 * @sub: (nullable): subscription
 *
 * Returns: whether EOSE has been observed
 */
bool          nostr_subscription_is_eosed(const NostrSubscription *sub);
/**
 * nostr_subscription_is_closed:
 * @sub: (nullable): subscription
 *
 * Returns: whether CLOSED was observed and emitted
 */
bool          nostr_subscription_is_closed(const NostrSubscription *sub);

/* Instrumentation: counters */
/**
 * nostr_subscription_events_enqueued:
 * Returns total number of events successfully enqueued into the subscription's event channel.
 */
unsigned long long nostr_subscription_events_enqueued(const NostrSubscription *sub);
/**
 * nostr_subscription_events_dropped:
 * Returns total number of events dropped at dispatch due to queue full/closed or not-live.
 */
unsigned long long nostr_subscription_events_dropped(const NostrSubscription *sub);

/* ========================================================================
 * Queue Health Metrics API (nostrc-sjv)
 * ======================================================================== */

/**
 * NostrQueueMetrics:
 *
 * Snapshot of queue health metrics for a subscription.
 * Use nostr_subscription_get_queue_metrics() to populate.
 */
typedef struct {
    uint64_t events_enqueued;      /**< Total events added to queue */
    uint64_t events_dequeued;      /**< Total events processed (consumer-reported) */
    uint64_t events_dropped;       /**< Total events dropped (queue full) */
    uint32_t current_depth;        /**< Current queue size */
    uint32_t peak_depth;           /**< High water mark */
    uint32_t queue_capacity;       /**< Max queue size */
    int64_t last_enqueue_time_us;  /**< Timestamp of last enqueue (microseconds since epoch) */
    int64_t last_dequeue_time_us;  /**< Timestamp of last dequeue (microseconds since epoch) */
    uint64_t total_wait_time_us;   /**< Cumulative time events spent in queue */
} NostrQueueMetrics;

/**
 * nostr_subscription_mark_event_consumed:
 * @sub: subscription
 * @enqueue_time_us: optional timestamp when event was enqueued (0 to skip latency tracking)
 *
 * Called by consumers after processing an event from the subscription channel.
 * Updates dequeue counters and latency metrics. The enqueue_time_us parameter
 * allows precise wait-time calculation if the caller tracks when events were queued.
 */
void nostr_subscription_mark_event_consumed(NostrSubscription *sub, int64_t enqueue_time_us);

/**
 * nostr_subscription_get_queue_metrics:
 * @sub: subscription to query
 * @out: (out): metrics output structure
 *
 * Gets a snapshot of queue health metrics. All fields are atomic-safe reads.
 * If sub is NULL, out is zeroed.
 *
 * Derived metrics (calculate from snapshot):
 * - Drop rate: events_dropped / events_enqueued (target: < 0.1%)
 * - Queue utilization: current_depth / queue_capacity (target: < 80%)
 * - Avg latency: total_wait_time_us / events_dequeued (target: < 100ms)
 */
void nostr_subscription_get_queue_metrics(const NostrSubscription *sub, NostrQueueMetrics *out);

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_SUBSCRIPTION_H__ */
