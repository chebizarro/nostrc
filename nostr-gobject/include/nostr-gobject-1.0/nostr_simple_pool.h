#ifndef NOSTR_SIMPLE_POOL_H
#define NOSTR_SIMPLE_POOL_H

#include <glib-object.h>
#include <gio/gio.h>
#include "nostr-simple-pool.h"
#include "nostr_relay.h"

/* Forward declaration for query batcher (nostrc-ozlp) */
typedef struct _NostrQueryBatcher NostrQueryBatcher;

/* Define GNostrSimplePool GObject (wrapper around core NostrSimplePool) */
#define GNOSTR_TYPE_SIMPLE_POOL (gnostr_simple_pool_get_type())
G_DECLARE_FINAL_TYPE(GNostrSimplePool, gnostr_simple_pool, GNOSTR, SIMPLE_POOL, GObject)

struct _GNostrSimplePool {
    GObject parent_instance;
    NostrSimplePool *pool;              /* core handle */
    NostrQueryBatcher *batcher;         /* nostrc-ozlp: query batcher */
    gboolean batching_enabled;          /* nostrc-ozlp: whether batching is active */
};

/* GObject convenience API (prefixed with gnostr_ to avoid clashes with core
 * libnostr C API which uses nostr_simple_pool_*). */
GNostrSimplePool *gnostr_simple_pool_new(void);
void gnostr_simple_pool_add_relay(GNostrSimplePool *self, NostrRelay *relay);
GPtrArray *gnostr_simple_pool_query_sync(GNostrSimplePool *self, NostrFilter *filter, GError **error);

/* Async API for live subscribe-many and one-off backfill */
void gnostr_simple_pool_subscribe_many_async(GNostrSimplePool *self,
                                             const char **urls,
                                             size_t url_count,
                                             NostrFilters *filters,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback cb,
                                             gpointer user_data);
gboolean gnostr_simple_pool_subscribe_many_finish(GNostrSimplePool *self,
                                                  GAsyncResult *res,
                                                  GError **error);

/* One-shot query API */
void gnostr_simple_pool_query_single_async(GNostrSimplePool *self,
                                          const char **urls,
                                          size_t url_count,
                                          const NostrFilter *filter,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);

GPtrArray *gnostr_simple_pool_query_single_finish(GNostrSimplePool *self,
                                                 GAsyncResult *res,
                                                 GError **error);

/* NIP-45 COUNT query - returns event count from relays.
 * @self: The pool
 * @urls: Array of relay URLs to query
 * @url_count: Number of URLs
 * @filter: Filter for the count query
 * @cancellable: Optional cancellable
 * @callback: Async callback
 * @user_data: User data for callback
 *
 * Unlike query_single which returns events, this returns only the count.
 * Useful for displaying reaction counts without fetching full events.
 * Requires relay support for NIP-45.
 */
void gnostr_simple_pool_count_async(GNostrSimplePool *self,
                                    const char **urls,
                                    size_t url_count,
                                    const NostrFilter *filter,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);

/* Returns the count from a NIP-45 COUNT query.
 * @self: The pool
 * @res: Async result
 * @error: Error output
 * Returns: Event count, or -1 on error
 */
gint64 gnostr_simple_pool_count_finish(GNostrSimplePool *self,
                                       GAsyncResult *res,
                                       GError **error);

/* One-shot query with streaming events via "events" signal.
 * Like query_single_async but emits events via the "events" signal as they
 * arrive, instead of only returning them all at the end. Useful for discovery
 * queries where you want to show results progressively. Connections are
 * closed after EOSE (not pooled). */
void gnostr_simple_pool_query_single_streaming_async(GNostrSimplePool *self,
                                                      const char **urls,
                                                      size_t url_count,
                                                      const NostrFilter *filter,
                                                      GCancellable *cancellable,
                                                      GAsyncReadyCallback callback,
                                                      gpointer user_data);

/* Background paginator with interval (emits "events" signal with GPtrArray* batches).
 * Starts a background thread that repeatedly issues one-shot subscriptions using the
 * provided filter, advancing the filter's `until` based on the smallest created_at
 * seen in each page, and sleeping for `interval_ms` between pages. The thread exits
 * when no new (non-duplicate) events are observed in a page or when cancelled.
 *
 * The async setup completes immediately; connect to the "events" signal to receive
 * batches of NostrEvent* as they arrive. Call the _finish() in the async callback
 * to confirm setup success.
 */
void gnostr_simple_pool_paginate_with_interval_async(GNostrSimplePool *self,
                                                     const char **urls,
                                                     size_t url_count,
                                                     const NostrFilter *filter,
                                                     guint interval_ms,
                                                     GCancellable *cancellable,
                                                     GAsyncReadyCallback cb,
                                                     gpointer user_data);

gboolean gnostr_simple_pool_paginate_with_interval_finish(GNostrSimplePool *self,
                                                          GAsyncResult *res,
                                                          GError **error);

/* Demand-driven batch fetch of kind-0 profiles by authors. Collects all profile
 * events from provided relays until EOSE per relay and returns a GPtrArray of
 * serialized JSON strings (char*) representing events. The array elements must
 * be freed with g_free() by the caller. Results are deduplicated by event id. */
void gnostr_simple_pool_fetch_profiles_by_authors_async(GNostrSimplePool *self,
                                                        const char **urls,
                                                        size_t url_count,
                                                        const char *const *authors,
                                                        size_t author_count,
                                                        int limit,
                                                        GCancellable *cancellable,
                                                        GAsyncReadyCallback cb,
                                                        gpointer user_data);

GPtrArray *gnostr_simple_pool_fetch_profiles_by_authors_finish(GNostrSimplePool *self,
                                                               GAsyncResult *res,
                                                               GError **error);

/* Relay connection status check */
gboolean gnostr_simple_pool_is_relay_connected(GNostrSimplePool *self, const char *url);

/* Get list of relay URLs currently in the pool */
GPtrArray *gnostr_simple_pool_get_relay_urls(GNostrSimplePool *self);

/* --- Queue Health Metrics API (nostrc-sjv) --- */

/**
 * GnostrQueueMetrics:
 *
 * Aggregated queue health metrics snapshot for a pool.
 * Combines metrics from all active subscriptions.
 */
typedef struct {
    guint64 events_enqueued;      /**< Total events added to queues */
    guint64 events_dequeued;      /**< Total events processed */
    guint64 events_dropped;       /**< Total events dropped */
    guint32 current_depth;        /**< Sum of current queue depths */
    guint32 peak_depth;           /**< Max peak depth across subscriptions */
    guint32 total_capacity;       /**< Sum of queue capacities */
    gint64 last_enqueue_time_us;  /**< Most recent enqueue timestamp */
    gint64 last_dequeue_time_us;  /**< Most recent dequeue timestamp */
    guint64 total_wait_time_us;   /**< Cumulative wait time across all queues */
    guint subscription_count;     /**< Number of active subscriptions */
} GnostrQueueMetrics;

/**
 * gnostr_simple_pool_get_queue_metrics:
 * @self: The pool
 * @out: (out): Aggregated metrics output
 *
 * Gets aggregated queue health metrics from all active subscriptions in the pool.
 *
 * Derived metrics (calculate from snapshot):
 * - Drop rate: events_dropped / events_enqueued (target: < 0.1%)
 * - Queue utilization: current_depth / total_capacity (target: < 80%)
 * - Avg latency: total_wait_time_us / events_dequeued (target: < 100ms)
 * - Throughput: events_dequeued / time_window (events/sec)
 */
void gnostr_simple_pool_get_queue_metrics(GNostrSimplePool *self, GnostrQueueMetrics *out);

/* --- Live Relay Switching (nostrc-36y.4) --- */

/* Remove a relay from the pool by URL. Disconnects and frees the relay.
 * @param self  The pool
 * @param url   The relay URL to remove
 * @return TRUE if the relay was found and removed, FALSE otherwise */
gboolean gnostr_simple_pool_remove_relay(GNostrSimplePool *self, const char *url);

/* Disconnect all relays in the pool without removing them.
 * Useful before reconfiguring the relay list.
 * @param self  The pool */
void gnostr_simple_pool_disconnect_all_relays(GNostrSimplePool *self);

/* Synchronize pool relays with a new URL list.
 * Removes relays not in the new list, adds new relays.
 * @param self      The pool
 * @param urls      Array of relay URLs
 * @param url_count Number of URLs in array */
void gnostr_simple_pool_sync_relays(GNostrSimplePool *self, const char **urls, size_t url_count);

/* --- Query Batching API (nostrc-ozlp) --- */

/**
 * gnostr_simple_pool_set_batching_enabled:
 * @self: The pool
 * @enabled: Whether to enable batching
 *
 * Enables or disables query batching. When enabled, multiple query_single_async
 * calls to the same relay within a short window are batched into a single
 * subscription with combined filters. Results are demultiplexed back to original
 * callers. This reduces subscription overhead when multiple components query
 * the same relays simultaneously (e.g., thread builder).
 *
 * Default: Disabled (for backward compatibility)
 */
void gnostr_simple_pool_set_batching_enabled(GNostrSimplePool *self, gboolean enabled);

/**
 * gnostr_simple_pool_get_batching_enabled:
 * @self: The pool
 *
 * Returns: Whether batching is currently enabled
 */
gboolean gnostr_simple_pool_get_batching_enabled(GNostrSimplePool *self);

/**
 * gnostr_simple_pool_set_batch_window_ms:
 * @self: The pool
 * @window_ms: Batch window in milliseconds (1-1000, default: 75)
 *
 * Sets the batching time window. Requests arriving within this window
 * are batched together. Requires batching to be enabled.
 */
void gnostr_simple_pool_set_batch_window_ms(GNostrSimplePool *self, guint window_ms);

/**
 * gnostr_simple_pool_get_batch_window_ms:
 * @self: The pool
 *
 * Returns: The current batch window in milliseconds
 */
guint gnostr_simple_pool_get_batch_window_ms(GNostrSimplePool *self);

#endif // NOSTR_SIMPLE_POOL_H