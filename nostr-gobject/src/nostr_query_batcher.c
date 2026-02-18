/*
 * nostr_query_batcher.c - Relay Query Subscription Batcher Implementation
 *
 * nostrc-ozlp
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nostr_query_batcher.h"
#include "nostr_simple_pool.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-event.h"
#include "nostr-simple-pool.h"  /* Core pool API */
#include "nostr-error.h"          /* NOSTR_ERROR domain */
#include "json.h"               /* nostr_event_serialize */
#include "error.h"              /* Error, free_error */
#include "channel.h"            /* go_channel_try_receive */
#include "select.h"             /* go_select_timeout */
#include "context.h"            /* go_context_background */
#include <string.h>

/* Default batch window in milliseconds */
#define DEFAULT_BATCH_WINDOW_MS 75

/* Maximum batch window */
#define MAX_BATCH_WINDOW_MS 1000

/* Timeout for waiting on EOSE (30 seconds) */
#define EOSE_TIMEOUT_MS 30000

/* Forward declarations */
static void fire_batch(NostrQueryBatcher *batcher, RelayBatch *batch);
static void complete_request(BatchedRequest *req, GError *error);
static void relay_batch_free(RelayBatch *batch);
static gboolean on_batch_flush_timeout(gpointer user_data);

/*
 * BatchedRequest lifecycle
 */

static BatchedRequest *batched_request_new(const NostrFilter *filter,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data) {
    BatchedRequest *req = g_new0(BatchedRequest, 1);
    req->filter = nostr_filter_copy(filter);
    req->callback = callback;
    req->user_data = user_data;
    req->cancellable = cancellable;  /* weak ref */
    req->results = g_ptr_array_new_with_free_func(g_free);
    req->submit_time_us = g_get_monotonic_time();
    return req;
}

static void batched_request_free(BatchedRequest *req) {
    if (!req) return;

    /* Disconnect cancellation handler if still connected */
    if (req->cancellable && req->cancel_handler_id > 0) {
        g_cancellable_disconnect(req->cancellable, req->cancel_handler_id);
    }

    if (req->filter) nostr_filter_free(req->filter);
    if (req->results) g_ptr_array_unref(req->results);
    g_free(req);
}

/*
 * RelayBatch lifecycle
 */

static RelayBatch *relay_batch_new(const char *relay_url) {
    RelayBatch *batch = g_new0(RelayBatch, 1);
    batch->relay_url = g_strdup(relay_url);
    batch->requests = g_ptr_array_new_with_free_func((GDestroyNotify)batched_request_free);
    batch->batch_start_time_us = g_get_monotonic_time();
    return batch;
}

static void relay_batch_free(RelayBatch *batch) {
    if (!batch) return;

    g_free(batch->relay_url);
    if (batch->requests) g_ptr_array_unref(batch->requests);
    if (batch->combined_filters) nostr_filters_free(batch->combined_filters);
    /* Note: subscription cleanup is handled by drain thread */
    g_free(batch);
}

/*
 * Filter combination
 */

static NostrFilters *combine_filters(GPtrArray *requests) {
    NostrFilters *combined = nostr_filters_new();

    for (guint i = 0; i < requests->len; i++) {
        BatchedRequest *req = g_ptr_array_index(requests, i);
        if (!req->cancelled && req->filter) {
            NostrFilter *copy = nostr_filter_copy(req->filter);
            nostr_filters_add(combined, copy);
            /* nostr_filters_add takes ownership and zeros the source */
        }
    }

    return combined;
}

/*
 * Check if all requests in batch are cancelled
 */

static gboolean all_requests_cancelled(RelayBatch *batch) {
    for (guint i = 0; i < batch->requests->len; i++) {
        BatchedRequest *req = g_ptr_array_index(batch->requests, i);
        if (!req->cancelled && !req->completed) {
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * Complete a request with results (or error)
 */

static void complete_request(BatchedRequest *req, GError *error) {
    if (req->completed) return;
    req->completed = TRUE;

    /* Disconnect cancellation handler */
    if (req->cancellable && req->cancel_handler_id > 0) {
        g_cancellable_disconnect(req->cancellable, req->cancel_handler_id);
        req->cancel_handler_id = 0;
    }

    /* Create a simple GTask to hold the result */
    GTask *task = g_task_new(NULL, req->cancellable, req->callback, req->user_data);

    if (error) {
        g_task_return_error(task, g_error_copy(error));
    } else {
        /* Transfer results ownership to caller */
        GPtrArray *results = req->results;
        req->results = NULL;  /* Caller takes ownership */
        g_task_return_pointer(task, results, (GDestroyNotify)g_ptr_array_unref);
    }

    g_object_unref(task);
}

/*
 * Complete all requests in a batch
 */

static void complete_all_requests(RelayBatch *batch, GError *error) {
    for (guint i = 0; i < batch->requests->len; i++) {
        BatchedRequest *req = g_ptr_array_index(batch->requests, i);
        if (!req->completed && !req->cancelled) {
            complete_request(req, error);
        }
    }
}

/*
 * Dispatch an event to all matching callers
 */

static void dispatch_event_to_callers(NostrQueryBatcher *batcher, RelayBatch *batch, NostrEvent *event) {
    for (guint i = 0; i < batch->requests->len; i++) {
        BatchedRequest *req = g_ptr_array_index(batch->requests, i);
        if (req->cancelled || req->completed) continue;

        /* Check if event matches this caller's filter */
        if (nostr_filter_matches(req->filter, event)) {
            char *json = nostr_event_serialize(event);
            if (json) {
                g_ptr_array_add(req->results, json);
                g_atomic_int_inc((volatile gint *)&batcher->total_events_demuxed);
            }
        }
    }
}

/*
 * Event drain thread - runs in background, drains events from subscription
 */

typedef struct {
    NostrQueryBatcher *batcher;
    RelayBatch *batch;
} BatchDrainCtx;

static gpointer batch_drain_thread(gpointer user_data) {
    BatchDrainCtx *ctx = user_data;
    NostrQueryBatcher *batcher = ctx->batcher;
    RelayBatch *batch = ctx->batch;
    NostrSubscription *sub = batch->subscription;

    if (!sub) {
        g_free(ctx);
        return NULL;
    }

    GoChannel *ch_events = nostr_subscription_get_events_channel(sub);
    GoChannel *ch_eose = nostr_subscription_get_eose_channel(sub);

    gint64 start_time = g_get_monotonic_time();
    gboolean got_eose = FALSE;

    /* Build select cases for event-driven waiting.
     * Case 0: receive from events channel
     * Case 1: receive from EOSE channel (if available) */
    gint eose_case_idx = -1;
    GoSelectCase cases[2];
    size_t num_cases = 0;

    if (ch_events) {
        cases[num_cases] = (GoSelectCase){
            .op = GO_SELECT_RECEIVE, .chan = ch_events,
            .value = NULL, .recv_buf = NULL
        };
        num_cases++;
    }
    if (ch_eose) {
        eose_case_idx = (gint)num_cases;
        cases[num_cases] = (GoSelectCase){
            .op = GO_SELECT_RECEIVE, .chan = ch_eose,
            .value = NULL, .recv_buf = NULL
        };
        num_cases++;
    }

    while (!got_eose && !batcher->disposing && num_cases > 0) {
        /* Check timeout */
        gint64 elapsed_ms = (g_get_monotonic_time() - start_time) / 1000;
        if (elapsed_ms > EOSE_TIMEOUT_MS) {
            g_warning("[BATCHER] Timeout waiting for EOSE on %s", batch->relay_url);
            break;
        }

        /* Check if all requests cancelled */
        g_mutex_lock(&batcher->mutex);
        gboolean all_cancelled = all_requests_cancelled(batch);
        g_mutex_unlock(&batcher->mutex);

        if (all_cancelled) {
            g_debug("[BATCHER] All requests cancelled, stopping drain for %s", batch->relay_url);
            break;
        }

        /* Block on channels with a bounded timeout so we can re-check
         * cancellation and EOSE timeout periodically. */
        gint64 remaining_ms = EOSE_TIMEOUT_MS - elapsed_ms;
        uint64_t select_ms = (uint64_t)(remaining_ms < 100 ? remaining_ms : 100);

        void *msg = NULL;
        cases[0].recv_buf = (void **)&msg;  /* Reset recv buffer each iteration */

        GoSelectResult result = go_select_timeout(cases, num_cases, select_ms);

        if (result.selected_case == 0 && ch_events) {
            /* Received an event */
            if (msg) {
                NostrEvent *event = (NostrEvent *)msg;
                g_mutex_lock(&batcher->mutex);
                dispatch_event_to_callers(batcher, batch, event);
                g_mutex_unlock(&batcher->mutex);
                nostr_event_free(event);
            }
        } else if (result.selected_case == eose_case_idx && eose_case_idx >= 0) {
            got_eose = TRUE;
            g_debug("[BATCHER] Got EOSE for %s", batch->relay_url);

            /* Drain any remaining buffered events */
            void *drain_msg = NULL;
            while (ch_events && go_channel_try_receive(ch_events, &drain_msg) == 0) {
                if (drain_msg) {
                    NostrEvent *event = (NostrEvent *)drain_msg;
                    g_mutex_lock(&batcher->mutex);
                    dispatch_event_to_callers(batcher, batch, event);
                    g_mutex_unlock(&batcher->mutex);
                    nostr_event_free(event);
                    drain_msg = NULL;
                }
            }
        }
        /* else: timeout — loop back to re-check conditions */
    }

    /* Complete all pending requests */
    g_mutex_lock(&batcher->mutex);
    complete_all_requests(batch, NULL);
    g_mutex_unlock(&batcher->mutex);

    /* Cleanup subscription */
    Error *err = NULL;
    nostr_subscription_close(sub, &err);
    if (err) {
        g_warning("[BATCHER] Error closing subscription: %s", err->message);
        free_error(err);
    }
    nostr_subscription_free(sub);
    batch->subscription = NULL;

    /* Remove batch from pending and free (skip if batcher is disposing -
     * the hash table will be destroyed by nostr_query_batcher_free) */
    if (!batcher->disposing) {
        g_mutex_lock(&batcher->mutex);
        g_hash_table_remove(batcher->pending_batches, batch->relay_url);
        g_mutex_unlock(&batcher->mutex);
    }

    g_free(ctx);
    return NULL;
}

/*
 * Helper to find relay in pool (must be called with pool mutex held in thread-safe manner)
 * Note: This is a workaround until proper relay accessor API is added
 */
static NostrRelay *find_relay_in_pool(NostrSimplePool *pool, const char *url) {
    if (!pool || !url) return NULL;
    for (size_t i = 0; i < pool->relay_count; i++) {
        if (pool->relays[i] && g_strcmp0(pool->relays[i]->url, url) == 0) {
            return pool->relays[i];
        }
    }
    return NULL;
}

/*
 * Fire a batch - create subscription and start drain thread
 */

static void fire_batch(NostrQueryBatcher *batcher, RelayBatch *batch) {
    if (batch->fired) return;
    batch->fired = TRUE;

    g_atomic_int_inc((volatile gint *)&batcher->total_batches);

    g_debug("[BATCHER] Firing batch for %s with %u requests",
            batch->relay_url, batch->requests->len);

    /* Combine all filters */
    batch->combined_filters = combine_filters(batch->requests);

    if (batch->combined_filters->count == 0) {
        g_debug("[BATCHER] All requests cancelled, skipping %s", batch->relay_url);
        g_mutex_lock(&batcher->mutex);
        complete_all_requests(batch, NULL);
        g_hash_table_remove(batcher->pending_batches, batch->relay_url);
        g_mutex_unlock(&batcher->mutex);
        return;
    }

    /* Ensure relay is in pool and connected */
    NostrSimplePool *core_pool = batcher->pool->pool;
    nostr_simple_pool_ensure_relay(core_pool, batch->relay_url);

    /* Find the relay in pool - MUST hold mutex during relay array access
     * to prevent heap-use-after-free when ensure_relay reallocs the array
     * (nostrc-kii3: same fix pattern as nostrc-pivi) */
    pthread_mutex_lock(&core_pool->pool_mutex);
    NostrRelay *relay = find_relay_in_pool(core_pool, batch->relay_url);
    pthread_mutex_unlock(&core_pool->pool_mutex);

    if (!relay) {
        g_autoptr(GError) gerr = g_error_new(NOSTR_ERROR, NOSTR_ERROR_NOT_FOUND,
                                    "Relay %s not found in pool after ensure",
                                    batch->relay_url);

        g_mutex_lock(&batcher->mutex);
        complete_all_requests(batch, gerr);
        g_hash_table_remove(batcher->pending_batches, batch->relay_url);
        g_mutex_unlock(&batcher->mutex);
        return;
    }

    /* Create subscription with combined filters */
    GoContext *bg = go_context_background();
    batch->subscription = nostr_relay_prepare_subscription(relay, bg, batch->combined_filters);

    if (!batch->subscription) {
        g_autoptr(GError) gerr = g_error_new_literal(NOSTR_ERROR, NOSTR_ERROR_INVALID_STATE,
                                            "Failed to prepare subscription");
        g_mutex_lock(&batcher->mutex);
        complete_all_requests(batch, gerr);
        g_hash_table_remove(batcher->pending_batches, batch->relay_url);
        g_mutex_unlock(&batcher->mutex);
        return;
    }

    /* Fire the subscription */
    Error *err = NULL;
    if (!nostr_subscription_fire(batch->subscription, &err)) {
        g_autoptr(GError) gerr = g_error_new(NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                                    "Failed to fire subscription: %s",
                                    err ? err->message : "unknown error");
        if (err) free_error(err);

        nostr_subscription_free(batch->subscription);
        batch->subscription = NULL;

        g_mutex_lock(&batcher->mutex);
        complete_all_requests(batch, gerr);
        g_hash_table_remove(batcher->pending_batches, batch->relay_url);
        g_mutex_unlock(&batcher->mutex);
        return;
    }

    /* Start drain thread */
    BatchDrainCtx *drain_ctx = g_new0(BatchDrainCtx, 1);
    drain_ctx->batcher = batcher;
    drain_ctx->batch = batch;

    batch->drain_thread = g_thread_new("batcher-drain", batch_drain_thread, drain_ctx);
    /* Note: thread ref kept for joining during shutdown */
}

/*
 * Batch flush timeout callback
 */

static gboolean on_batch_flush_timeout(gpointer user_data) {
    NostrQueryBatcher *batcher = user_data;

    g_mutex_lock(&batcher->mutex);
    batcher->flush_timeout_id = 0;

    /* Fire all pending batches */
    GHashTableIter iter;
    gpointer key, value;
    GPtrArray *batches_to_fire = g_ptr_array_new();

    g_hash_table_iter_init(&iter, batcher->pending_batches);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        RelayBatch *batch = value;
        if (!batch->fired) {
            g_ptr_array_add(batches_to_fire, batch);
        }
    }

    g_mutex_unlock(&batcher->mutex);

    /* Fire batches outside of lock to avoid deadlock */
    for (guint i = 0; i < batches_to_fire->len; i++) {
        RelayBatch *batch = g_ptr_array_index(batches_to_fire, i);
        fire_batch(batcher, batch);
    }

    g_ptr_array_unref(batches_to_fire);

    return G_SOURCE_REMOVE;
}

/*
 * Schedule batch flush
 */

static void schedule_batch_flush(NostrQueryBatcher *batcher) {
    /* Already scheduled? */
    if (batcher->flush_timeout_id != 0) return;

    batcher->flush_timeout_id = g_timeout_add(batcher->batch_window_ms,
                                               on_batch_flush_timeout,
                                               batcher);
}

/*
 * Cancellation handler
 */

static void on_request_cancelled(GCancellable *cancellable, gpointer user_data) {
    BatchedRequest *req = user_data;
    (void)cancellable;

    /* Mark as cancelled - completion happens in drain thread */
    req->cancelled = TRUE;
}

/*
 * Public API
 */

NostrQueryBatcher *nostr_query_batcher_new(GNostrSimplePool *pool) {
    g_return_val_if_fail(pool != NULL, NULL);

    NostrQueryBatcher *batcher = g_new0(NostrQueryBatcher, 1);
    batcher->pool = pool;  /* weak ref */
    batcher->batch_window_ms = DEFAULT_BATCH_WINDOW_MS;
    batcher->pending_batches = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                      NULL,  /* key is owned by batch */
                                                      (GDestroyNotify)relay_batch_free);
    g_mutex_init(&batcher->mutex);

    return batcher;
}

void nostr_query_batcher_free(NostrQueryBatcher *batcher) {
    if (!batcher) return;

    batcher->disposing = TRUE;

    /* Cancel flush timeout */
    if (batcher->flush_timeout_id != 0) {
        g_source_remove(batcher->flush_timeout_id);
        batcher->flush_timeout_id = 0;
    }

    /* Collect drain threads to join (must do outside of mutex to avoid deadlock) */
    GPtrArray *threads_to_join = g_ptr_array_new();

    g_mutex_lock(&batcher->mutex);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, batcher->pending_batches);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        RelayBatch *batch = value;
        if (batch->drain_thread) {
            g_ptr_array_add(threads_to_join, g_thread_ref(batch->drain_thread));
        }
        /* Complete all pending requests with cancellation */
        g_autoptr(GError) gerr2 = g_error_new_literal(NOSTR_ERROR, NOSTR_ERROR_INVALID_STATE,
                                           "Batcher disposed");
        complete_all_requests(batch, gerr2);
    }
    g_mutex_unlock(&batcher->mutex);

    /* Wait for all drain threads to complete */
    for (guint i = 0; i < threads_to_join->len; i++) {
        GThread *thread = g_ptr_array_index(threads_to_join, i);
        g_thread_join(thread);  /* This also unrefs the thread */
    }
    g_ptr_array_unref(threads_to_join);

    g_hash_table_destroy(batcher->pending_batches);
    g_mutex_clear(&batcher->mutex);
    g_free(batcher);
}

void nostr_query_batcher_submit(NostrQueryBatcher *batcher,
                                const char *relay_url,
                                const NostrFilter *filter,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data) {
    g_return_if_fail(batcher != NULL);
    g_return_if_fail(relay_url != NULL);
    g_return_if_fail(filter != NULL);
    g_return_if_fail(callback != NULL);

    g_atomic_int_inc((volatile gint *)&batcher->total_requests);

    g_mutex_lock(&batcher->mutex);

    /* Get or create batch for this relay */
    RelayBatch *batch = g_hash_table_lookup(batcher->pending_batches, relay_url);
    if (!batch) {
        batch = relay_batch_new(relay_url);
        g_hash_table_insert(batcher->pending_batches, batch->relay_url, batch);
    }

    /* Create request */
    BatchedRequest *req = batched_request_new(filter, cancellable, callback, user_data);

    /* Set up cancellation handler */
    if (cancellable) {
        req->cancel_handler_id = g_cancellable_connect(cancellable,
                                                        G_CALLBACK(on_request_cancelled),
                                                        req, NULL);
    }

    g_ptr_array_add(batch->requests, req);

    /* Schedule flush if not already scheduled */
    schedule_batch_flush(batcher);

    g_mutex_unlock(&batcher->mutex);

    g_debug("[BATCHER] Submitted request to %s (batch now has %u requests)",
            relay_url, batch->requests->len);
}

void nostr_query_batcher_set_window_ms(NostrQueryBatcher *batcher, guint window_ms) {
    g_return_if_fail(batcher != NULL);

    if (window_ms < 1) window_ms = 1;
    if (window_ms > MAX_BATCH_WINDOW_MS) window_ms = MAX_BATCH_WINDOW_MS;

    batcher->batch_window_ms = window_ms;
}

guint nostr_query_batcher_get_window_ms(NostrQueryBatcher *batcher) {
    g_return_val_if_fail(batcher != NULL, 0);
    return batcher->batch_window_ms;
}

void nostr_query_batcher_flush(NostrQueryBatcher *batcher) {
    g_return_if_fail(batcher != NULL);

    /* Cancel scheduled flush */
    if (batcher->flush_timeout_id != 0) {
        g_source_remove(batcher->flush_timeout_id);
        batcher->flush_timeout_id = 0;
    }

    /* Fire all pending batches */
    on_batch_flush_timeout(batcher);
}

guint nostr_query_batcher_get_pending_count(NostrQueryBatcher *batcher) {
    g_return_val_if_fail(batcher != NULL, 0);

    guint count = 0;
    g_mutex_lock(&batcher->mutex);

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, batcher->pending_batches);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        RelayBatch *batch = value;
        if (!batch->fired) {
            count += batch->requests->len;
        }
    }

    g_mutex_unlock(&batcher->mutex);
    return count;
}

void nostr_query_batcher_get_metrics(NostrQueryBatcher *batcher, NostrBatcherMetrics *out) {
    g_return_if_fail(batcher != NULL);
    g_return_if_fail(out != NULL);

    out->total_requests = batcher->total_requests;
    out->total_batches = batcher->total_batches;
    out->total_events_demuxed = batcher->total_events_demuxed;

    if (out->total_batches > 0) {
        out->avg_requests_per_batch = (gdouble)out->total_requests / (gdouble)out->total_batches;
    } else {
        out->avg_requests_per_batch = 0.0;
    }
}
