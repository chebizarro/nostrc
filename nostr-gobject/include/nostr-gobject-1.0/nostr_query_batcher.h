/*
 * nostr_query_batcher.h - Relay Query Subscription Batcher
 *
 * Batches multiple one-shot queries to the same relay URL within a time window,
 * combining their filters using OR semantics and demultiplexing results back
 * to the original callers. Reduces subscription overhead when multiple
 * components query the same relays simultaneously.
 *
 * nostrc-ozlp
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NOSTR_QUERY_BATCHER_H
#define NOSTR_QUERY_BATCHER_H

#include <glib.h>
#include <gio/gio.h>
#include "nostr-filter.h"

G_BEGIN_DECLS

/* Forward declarations */
typedef struct _GNostrSimplePool GNostrSimplePool;
typedef struct _NostrQueryBatcher NostrQueryBatcher;

/*
 * BatchedRequest:
 * Represents a single query request from a caller.
 * The batcher collects these, combines their filters, and demultiplexes results.
 */
typedef struct {
    NostrFilter *filter;            /* Deep copy of caller's filter */
    GAsyncReadyCallback callback;   /* Caller's completion callback */
    gpointer user_data;             /* Caller's user data */
    GCancellable *cancellable;      /* Optional cancellable (weak ref) */
    gulong cancel_handler_id;       /* Handler ID for cancellation signal */
    GPtrArray *results;             /* Collected events matching this filter (char* JSON) */
    gboolean cancelled;             /* TRUE if request was cancelled */
    gboolean completed;             /* TRUE if callback was invoked */
    gint64 submit_time_us;          /* Timestamp for metrics */
} BatchedRequest;

/*
 * RelayBatch:
 * Groups all pending requests for a single relay URL.
 * When the batch window expires, fires a single subscription with combined filters.
 */
typedef struct {
    char *relay_url;                /* Relay URL this batch is for */
    GPtrArray *requests;            /* Array of BatchedRequest* */
    NostrFilters *combined_filters; /* OR of all request filters */
    gboolean fired;                 /* TRUE after subscription sent */
    gint64 batch_start_time_us;     /* When first request was added */
    /* Subscription state (set after firing) */
    gpointer subscription;          /* NostrSubscription* (opaque to avoid header dep) */
    GThread *drain_thread;          /* Event drain thread */
} RelayBatch;

/*
 * NostrQueryBatcher:
 * The main batcher component. Attached to a GNostrSimplePool.
 */
struct _NostrQueryBatcher {
    GHashTable *pending_batches;    /* relay_url (char*) -> RelayBatch* */
    GMutex mutex;                   /* Protects all state */
    guint flush_timeout_id;         /* GSource ID for batch flush timer */
    guint batch_window_ms;          /* Batching window in milliseconds (default: 75) */
    GNostrSimplePool *pool;         /* Owning pool (weak ref, do not unref) */
    gboolean disposing;             /* TRUE during shutdown */

    /* Metrics */
    guint64 total_requests;         /* Total requests submitted */
    guint64 total_batches;          /* Total batches fired */
    guint64 total_events_demuxed;   /* Total events routed to callers */
};

/*
 * nostr_query_batcher_new:
 * @pool: The GNostrSimplePool that owns this batcher
 *
 * Creates a new query batcher. The batcher does not own the pool reference.
 *
 * Returns: (transfer full): A new NostrQueryBatcher, free with nostr_query_batcher_free()
 */
NostrQueryBatcher *nostr_query_batcher_new(GNostrSimplePool *pool);

/*
 * nostr_query_batcher_free:
 * @batcher: (nullable): The batcher to free
 *
 * Frees the batcher. Any pending requests are completed with empty results.
 */
void nostr_query_batcher_free(NostrQueryBatcher *batcher);

/*
 * nostr_query_batcher_submit:
 * @batcher: The batcher
 * @relay_url: Relay URL to query
 * @filter: Filter for the query (deep copied)
 * @cancellable: (nullable): Optional cancellable
 * @callback: Async completion callback
 * @user_data: User data for callback
 *
 * Submits a query request to the batcher. The request is added to a pending
 * batch for the given relay URL. When the batch window expires, all pending
 * requests for that relay are combined into a single subscription.
 *
 * The callback receives a GAsyncResult that can be passed to
 * gnostr_simple_pool_query_single_finish() to get the results.
 */
void nostr_query_batcher_submit(NostrQueryBatcher *batcher,
                                const char *relay_url,
                                const NostrFilter *filter,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data);

/*
 * nostr_query_batcher_set_window_ms:
 * @batcher: The batcher
 * @window_ms: Batch window in milliseconds (1-1000, default: 75)
 *
 * Sets the batching time window. Requests arriving within this window
 * are batched together. Smaller windows reduce latency but batch fewer
 * requests; larger windows batch more but add latency.
 */
void nostr_query_batcher_set_window_ms(NostrQueryBatcher *batcher, guint window_ms);

/*
 * nostr_query_batcher_get_window_ms:
 * @batcher: The batcher
 *
 * Returns: The current batch window in milliseconds
 */
guint nostr_query_batcher_get_window_ms(NostrQueryBatcher *batcher);

/*
 * nostr_query_batcher_flush:
 * @batcher: The batcher
 *
 * Immediately flushes all pending batches. Useful for testing or when
 * you need immediate results without waiting for the batch window.
 */
void nostr_query_batcher_flush(NostrQueryBatcher *batcher);

/*
 * nostr_query_batcher_get_pending_count:
 * @batcher: The batcher
 *
 * Returns: Number of pending requests across all batches
 */
guint nostr_query_batcher_get_pending_count(NostrQueryBatcher *batcher);

/*
 * Metrics
 */
typedef struct {
    guint64 total_requests;
    guint64 total_batches;
    guint64 total_events_demuxed;
    gdouble avg_requests_per_batch;
} NostrBatcherMetrics;

void nostr_query_batcher_get_metrics(NostrQueryBatcher *batcher, NostrBatcherMetrics *out);

G_END_DECLS

#endif /* NOSTR_QUERY_BATCHER_H */
