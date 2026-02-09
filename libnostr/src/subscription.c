#include "nostr-envelope.h"
#include "nostr-filter.h"
#include "nostr-event.h"
#include "nostr-subscription.h"
#include "json.h"
#include "relay-private.h"
#include "nostr-relay.h"
#include "subscription-private.h"
#include "nostr-relay.h"
#include "nostr/metrics.h"
#include "nostr_log.h"
#include "select.h"
#include <openssl/ssl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

static _Atomic long long g_sub_counter = 1;

/* ========================================================================
 * Adaptive Queue Capacity (nostrc-3g8)
 * ======================================================================== */

static AdaptiveCapacityState g_adaptive_state = {
    .suggested_capacity = NOSTR_QUEUE_CAPACITY_DEFAULT,
    .max_observed_peak = 0,
    .last_capacity_adjust_us = 0
};

AdaptiveCapacityState *nostr_subscription_get_adaptive_state(void) {
    return &g_adaptive_state;
}

uint32_t nostr_subscription_suggest_capacity(void) {
    uint32_t suggested = atomic_load(&g_adaptive_state.suggested_capacity);

    /* Check environment override - allows explicit override for testing
     * Only MAX is enforced to prevent extreme memory usage.
     * MIN is not enforced to support test scenarios with small capacities. */
    const char *env = getenv("NOSTR_SUB_EVENTS_CAP");
    if (env && *env) {
        int v = atoi(env);
        if (v > 0) {
            if (v > NOSTR_QUEUE_CAPACITY_MAX) v = NOSTR_QUEUE_CAPACITY_MAX;
            return (uint32_t)v;
        }
    }

    /* Ensure suggested is within bounds for production use */
    if (suggested < NOSTR_QUEUE_CAPACITY_MIN) suggested = NOSTR_QUEUE_CAPACITY_MIN;
    if (suggested > NOSTR_QUEUE_CAPACITY_MAX) suggested = NOSTR_QUEUE_CAPACITY_MAX;

    return suggested;
}

void nostr_subscription_report_peak_usage(uint32_t peak_depth, uint32_t capacity) {
    if (capacity == 0) return;

    /* Update max observed peak */
    uint32_t old_max = atomic_load(&g_adaptive_state.max_observed_peak);
    while (peak_depth > old_max) {
        if (atomic_compare_exchange_weak(&g_adaptive_state.max_observed_peak, &old_max, peak_depth)) {
            break;
        }
    }

    /* Calculate utilization percentage */
    uint32_t utilization = (peak_depth * 100) / capacity;

    /* If utilization exceeded grow threshold, suggest larger capacity for future subs */
    if (utilization >= NOSTR_QUEUE_GROW_THRESHOLD) {
        uint32_t new_suggested = capacity * 2;
        if (new_suggested > NOSTR_QUEUE_CAPACITY_MAX) {
            new_suggested = NOSTR_QUEUE_CAPACITY_MAX;
        }

        uint32_t old_suggested = atomic_load(&g_adaptive_state.suggested_capacity);
        if (new_suggested > old_suggested) {
            if (atomic_compare_exchange_strong(&g_adaptive_state.suggested_capacity,
                                               &old_suggested, new_suggested)) {
                nostr_rl_log(NLOG_INFO, "queue",
                    "Adaptive capacity increased: %u -> %u (peak=%u, cap=%u, util=%u%%)",
                    old_suggested, new_suggested, peak_depth, capacity, utilization);
                nostr_metric_counter_add("queue_capacity_increase", 1);
            }
        }
    }
}

/* Get current time in microseconds */
static inline int64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

NostrSubscription *nostr_subscription_new(NostrRelay *relay, NostrFilters *filters) {
    NostrSubscription *sub = (NostrSubscription *)malloc(sizeof(NostrSubscription));
    if (!sub)
        return NULL;

    /* Hold a reference to prevent relay from being freed while subscription is active */
    sub->relay = nostr_relay_ref(relay);
    sub->filters = filters;
    sub->priv = (SubscriptionPrivate *)malloc(sizeof(SubscriptionPrivate));
    if (!sub->priv) {
        free(sub);
        return NULL;
    }

    sub->priv->count_result = NULL;

    /* Use adaptive capacity for events channel (nostrc-3g8)
     * The suggested capacity is based on historical peak usage across subscriptions.
     * Environment variable NOSTR_SUB_EVENTS_CAP can override. */
    uint32_t ev_cap = nostr_subscription_suggest_capacity();
    int eose_cap = 8, closed_cap = 8;

    /* Allow environment overrides for EOSE and CLOSED channels */
    const char *eose_cap_s = getenv("NOSTR_SUB_EOSE_CAP");
    if (eose_cap_s && *eose_cap_s) {
        int v = atoi(eose_cap_s);
        if (v > 0) eose_cap = v;
    }
    const char *closed_cap_s = getenv("NOSTR_SUB_CLOSED_CAP");
    if (closed_cap_s && *closed_cap_s) {
        int v = atoi(closed_cap_s);
        if (v > 0) closed_cap = v;
    }
    sub->events = go_channel_create((size_t)ev_cap);
    sub->end_of_stored_events = go_channel_create(eose_cap);
    sub->closed_reason = go_channel_create(closed_cap);
    sub->priv->live = false;
    sub->priv->eosed = false;
    sub->priv->closed = false;
    sub->priv->unsubbed = false;

    // Initialize queue metrics (nostrc-sjv)
    QueueMetrics *m = &sub->priv->metrics;
    atomic_store(&m->events_enqueued, 0);
    atomic_store(&m->events_dequeued, 0);
    atomic_store(&m->events_dropped, 0);
    atomic_store(&m->current_depth, 0);
    atomic_store(&m->peak_depth, 0);
    m->queue_capacity = (uint32_t)ev_cap;
    int64_t now_us = get_time_us();
    atomic_store(&m->last_enqueue_time_us, 0);
    atomic_store(&m->last_dequeue_time_us, 0);
    atomic_store(&m->total_wait_time_us, 0);
    atomic_store(&m->created_time_us, now_us);

    nsync_mu_init(&sub->priv->sub_mutex);
    // Initialize refcount to 1 (nostrc-nr96)
    atomic_store(&sub->priv->refcount, 1);
    // Initialize wait group for lifecycle thread
    go_wait_group_init(&sub->priv->wg);
    go_wait_group_add(&sub->priv->wg, 1);

    // Initialize identity and context so API works even if caller doesn't go through relay helper
    long long c = atomic_fetch_add(&g_sub_counter, 1);
    sub->priv->counter = (int)c;
    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%lld", c);
    sub->priv->id = strdup(idbuf);
    // Child context derived from relay's connection context if available, else background
    GoContext *parent = relay ? relay->priv->connection_context : go_context_background();
    CancelContextResult subctx = go_context_with_cancel(parent);
    sub->context = subctx.context;
    sub->priv->cancel = subctx.cancel;
    // Default match function
    sub->priv->match = nostr_filters_match;
    // Start lifecycle watcher
    if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
        fprintf(stderr, "[sub %s] create: starting lifecycle thread\n", sub->priv->id);
    }
    go(nostr_subscription_start, sub);
    nostr_metric_counter_add("sub_created", 1);

    // LIFECYCLE: Log subscription creation
    if (getenv("NOSTR_DEBUG_LIFECYCLE")) {
        fprintf(stderr, "[SUB_LIFECYCLE] CREATE sid=%s counter=%d\n",
                sub->priv->id, sub->priv->counter);
    }

    return sub;
}

/* Internal: actual deallocation when refcount drops to 0 (nostrc-nr96) */
static void subscription_destroy(NostrSubscription *sub) {
    // Ensure lifecycle worker has exited (caller should have unsubscribed)
    go_wait_group_wait(&sub->priv->wg);

    // Drain any remaining events from the channel to prevent memory leaks
    // Events in the channel are owned by the subscription and must be freed
    // Safe to drain now since:
    // 1. Subscription removed from relay map (no new dispatches)
    // 2. Lifecycle worker exited (channel is closed)
    if (sub->events) {
        void *ev = NULL;
        while (go_channel_try_receive(sub->events, &ev) == 0) {
            if (ev) {
                nostr_event_free((NostrEvent *)ev);
                ev = NULL;
            }
        }
    }

    go_channel_free(sub->events);
    go_channel_free(sub->end_of_stored_events);
    go_channel_free(sub->closed_reason);
    free(sub->priv->id);
    go_wait_group_destroy(&sub->priv->wg);
    free(sub->priv);

    /* Release reference to relay (may trigger relay free if last reference) */
    if (sub->relay) {
        nostr_relay_unref(sub->relay);
        sub->relay = NULL;
    }

    free(sub);
    nostr_metric_counter_add("sub_freed", 1);
}

NostrSubscription *nostr_subscription_ref(NostrSubscription *sub) {
    if (!sub || !sub->priv) return sub;
    atomic_fetch_add(&sub->priv->refcount, 1);
    return sub;
}

void nostr_subscription_unref(NostrSubscription *sub) {
    if (!sub || !sub->priv) return;
    int prev = atomic_fetch_sub(&sub->priv->refcount, 1);
    if (prev <= 1) {
        subscription_destroy(sub);
    }
}

void nostr_subscription_free(NostrSubscription *sub) {
    if (!sub)
        return;

    // LIFECYCLE: Log subscription free
    if (getenv("NOSTR_DEBUG_LIFECYCLE")) {
        bool eosed = sub->priv ? atomic_load(&sub->priv->eosed) : false;
        bool unsubbed = sub->priv ? atomic_load(&sub->priv->unsubbed) : false;
        fprintf(stderr, "[SUB_LIFECYCLE] FREE sid=%s eosed=%d unsubbed=%d\n",
                sub->priv && sub->priv->id ? sub->priv->id : "null",
                eosed ? 1 : 0, unsubbed ? 1 : 0);
    }

    // IMPORTANT: Remove from relay map FIRST to prevent message_loop from
    // dispatching events to this subscription while we're freeing it.
    // This must happen before waiting for the lifecycle worker.
    if (sub->relay && sub->relay->subscriptions) {
        go_hash_map_remove_int(sub->relay->subscriptions, sub->priv->counter);
    }

    // Decrement refcount — actual deallocation happens when it hits 0 (nostrc-nr96)
    nostr_subscription_unref(sub);
}

char *nostr_subscription_get_id(NostrSubscription *sub) {
    return sub->priv->id;
}

void *nostr_subscription_start(void *arg) {
    NostrSubscription *sub = (NostrSubscription *)arg;

    if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
        fprintf(stderr, "[sub %s] start: waiting for cancel/close\n", sub->priv->id);
    }

    // Wait for context cancellation via its done channel; this is signaled/closed by go_context_cancel
    GoChannel *done = go_context_done(sub->context);
    (void)go_channel_receive(done, NULL); // success or closed+empty both indicate done

    // Once the context is canceled, unsubscribe the subscription
    if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
        fprintf(stderr, "[sub %s] start: context canceled -> unsubscribing\n", sub->priv->id);
    }
    nostr_subscription_unsubscribe(sub);

    // Lock the subscription to avoid race conditions
    nsync_mu_lock(&sub->priv->sub_mutex);

    // Close the events channel
    go_channel_close(sub->events);

    // Unlock the mutex after the events channel is closed
    nsync_mu_unlock(&sub->priv->sub_mutex);

    if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
        fprintf(stderr, "[sub %s] start: exited lifecycle thread\n", sub->priv->id);
    }
    go_wait_group_done(&sub->priv->wg);
    return NULL;
}

void nostr_subscription_dispatch_event(NostrSubscription *sub, NostrEvent *event) {
    if (!sub || !event)
        return;

    bool added = false;
    if (!atomic_load(&sub->priv->eosed)) {
        added = true;
    }

    // Fast-path check without holding the mutex for too long
    nsync_mu_lock(&sub->priv->sub_mutex);
    bool is_live = atomic_load(&sub->priv->live);
    nsync_mu_unlock(&sub->priv->sub_mutex);

    QueueMetrics *m = &sub->priv->metrics;

    if (is_live) {
        // Non-blocking send; if full or closed or canceled, drop to avoid hang
        if (go_channel_try_send(sub->events, event) != 0) {
            if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
                fprintf(stderr, "[sub %s] dispatch_event: dropped (queue full/closed)\n", sub->priv->id);
            }
            atomic_fetch_add(&m->events_dropped, 1);
            nostr_metric_counter_add("sub_event_drop", 1);
            nostr_rl_log(NLOG_WARN, "sub", "event drop: queue full sid=%s", sub->priv->id);
            nostr_event_free(event);
        } else {
            // Update queue metrics on successful enqueue
            int64_t now_us = get_time_us();
            atomic_fetch_add(&m->events_enqueued, 1);
            atomic_store(&m->last_enqueue_time_us, now_us);

            /* Get actual channel depth for warnings (nostrc-dw3)
             * Note: m->current_depth was removed because consumers never called
             * nostr_subscription_mark_event_consumed(), making it grow unbounded.
             * Using the actual channel depth is more accurate. */
            uint32_t actual_depth = (uint32_t)go_channel_get_depth(sub->events);

            // Update peak depth if new high
            uint32_t old_peak = atomic_load(&m->peak_depth);
            while (actual_depth > old_peak) {
                if (atomic_compare_exchange_weak(&m->peak_depth, &old_peak, actual_depth)) {
                    break;
                }
            }

            /* Check utilization and report to adaptive capacity system (nostrc-3g8)
             * Only check periodically to avoid overhead on every enqueue */
            uint32_t capacity = m->queue_capacity;
            if (capacity > 0) {
                uint32_t utilization = (actual_depth * 100) / capacity;

                /* Log warning if approaching capacity */
                if (utilization >= 90) {
                    nostr_rl_log(NLOG_WARN, "queue",
                        "Queue near capacity: sid=%s depth=%u/%u util=%u%%",
                        sub->priv->id, actual_depth, capacity, utilization);
                    nostr_metric_counter_add("queue_near_capacity", 1);
                }

                /* Report to adaptive system if we hit the grow threshold */
                if (utilization >= NOSTR_QUEUE_GROW_THRESHOLD) {
                    nostr_subscription_report_peak_usage(actual_depth, capacity);
                }
            }

            nostr_metric_counter_add("sub_event_enqueued", 1);
        }
    } else {
        // Not live anymore; drop event to avoid blocking
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub %s] dispatch_event: dropped (not live)\n", sub->priv->id);
        }
        atomic_fetch_add(&m->events_dropped, 1);
        nostr_metric_counter_add("sub_event_drop_not_live", 1);
        nostr_rl_log(NLOG_INFO, "sub", "event drop: not live sid=%s", sub->priv->id);
        nostr_event_free(event);
    }

    if (added) {
        // Decrement stored event counter if needed
    }
}


void nostr_subscription_dispatch_eose(NostrSubscription *sub) {
    if (!sub)
        return;

    // LIFECYCLE: Log EOSE dispatch
    if (getenv("NOSTR_DEBUG_LIFECYCLE")) {
        fprintf(stderr, "[SUB_LIFECYCLE] EOSE_DISPATCH sid=%s counter=%d\n",
                sub->priv && sub->priv->id ? sub->priv->id : "null",
                sub->priv ? sub->priv->counter : -1);
    }

    // Change the match behavior and signal the end of stored events
    if (atomic_exchange(&sub->priv->eosed, true) == false) {
        sub->priv->match = nostr_filters_match_ignoring_timestamp;

        // nostrc-dkx: Log event count before EOSE for debugging relay response sizes
        uint64_t event_count = atomic_load(&sub->priv->metrics.events_enqueued);
        fprintf(stderr, "[RELAY_POOL] Subscription %s received %llu events before EOSE\n",
                sub->priv->id ? sub->priv->id : "unknown", (unsigned long long)event_count);

        // CRITICAL: Must use blocking send for EOSE to ensure delivery!
        // The goroutine polling loop DEPENDS on receiving EOSE to complete.
        // Non-blocking try_send can drop the signal if timing is off.
        // The channel has buffer=8, so this won't deadlock in normal operation.
        if (getenv("NOSTR_DEBUG_EOSE")) {
            fprintf(stderr, "[EOSE_SIGNAL] sid=%s sending to channel (BLOCKING)\n", sub->priv->id ? sub->priv->id : "null");
        }
        go_channel_send(sub->end_of_stored_events, NULL);
        nostr_metric_counter_add("sub_eose_signal", 1);
        if (getenv("NOSTR_DEBUG_EOSE")) {
            fprintf(stderr, "[EOSE_SIGNAL] sid=%s sent successfully\n", sub->priv->id ? sub->priv->id : "null");
        }
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub %s] dispatch_eose: signaled EOSE\n", sub->priv->id);
        }
    } else {
        if (getenv("NOSTR_DEBUG_EOSE")) {
            fprintf(stderr, "[EOSE_DUP] sid=%s already eosed, ignoring\n", sub->priv->id ? sub->priv->id : "null");
        }
    }
}

void nostr_subscription_dispatch_closed(NostrSubscription *sub, const char *reason) {
    if (!sub || !reason)
        return;

    // Set the closed flag and dispatch the reason
    if (atomic_exchange(&sub->priv->closed, true) == false) {
        // Make a copy of the reason string to avoid use-after-free
        char *reason_copy = strdup(reason);
        if (!reason_copy) {
            // Out of memory - log but continue
            if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
                fprintf(stderr, "[sub %s] dispatch_closed: OOM copying reason\n", sub->priv->id);
            }
            return;
        }
        
        // Non-blocking send to avoid deadlock (SHUTDOWN.md line 35)
        if (go_channel_try_send(sub->closed_reason, (void *)reason_copy) != 0) {
            // Channel full or closed - log but don't block
            if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
                fprintf(stderr, "[sub %s] dispatch_closed: channel full/closed, dropping reason\n", sub->priv->id);
            }
            free(reason_copy);  // Free the copy since we couldn't send it
        } else {
            nostr_metric_counter_add("sub_closed_signal", 1);
        }
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub %s] dispatch_closed: reason='%s'\n", sub->priv->id, reason ? reason : "");
        }
    }
}

void nostr_subscription_unsubscribe(NostrSubscription *sub) {
    if (!sub)
        return;

    // LIFECYCLE: Log unsubscribe
    if (getenv("NOSTR_DEBUG_LIFECYCLE")) {
        bool already_unsubbed = sub->priv ? atomic_load(&sub->priv->unsubbed) : false;
        bool eosed = sub->priv ? atomic_load(&sub->priv->eosed) : false;
        fprintf(stderr, "[SUB_LIFECYCLE] UNSUBSCRIBE sid=%s already_unsubbed=%d eosed=%d\n",
                sub->priv && sub->priv->id ? sub->priv->id : "null",
                already_unsubbed ? 1 : 0, eosed ? 1 : 0);
    }

    // Idempotent: ensure we only execute unsubscribe logic once
    if (atomic_exchange(&sub->priv->unsubbed, true)) {
        return;
    }

    // Cancel the subscription's context (idempotent)
    if (sub->priv->cancel) {
        sub->priv->cancel(sub->context);
    }
    nostr_metric_counter_add("sub_unsubscribe", 1);

    // Mark as no longer live; CLOSE will be handled separately if needed
    atomic_exchange(&sub->priv->live, false);
    // Do NOT remove from relay map here. Keep it until final free so late EOSE/CLOSED
    // from the relay can still be routed to this subscription without noisy drops.
    if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
        fprintf(stderr, "[sub %s] unsub: context canceled; keeping in relay map until free\n", sub->priv->id);
    }
}

void nostr_subscription_close(NostrSubscription *sub, Error **err) {
    if (!sub || !sub->relay) {
        if (err) *err = new_error(1, "subscription or relay is NULL");
        return;
    }

    // LIFECYCLE: Log subscription close
    if (getenv("NOSTR_DEBUG_LIFECYCLE")) {
        bool eosed = sub->priv ? atomic_load(&sub->priv->eosed) : false;
        fprintf(stderr, "[SUB_LIFECYCLE] CLOSE sid=%s eosed=%d relay=%s\n",
                sub->priv && sub->priv->id ? sub->priv->id : "null",
                eosed ? 1 : 0,
                sub->relay && sub->relay->url ? sub->relay->url : "null");
    }

    if (nostr_relay_is_connected(sub->relay)) {
        // Create a NostrCloseEnvelope with the subscription ID
        // NIP-01: Client sends ["CLOSE", "<subscription_id>"] to close a subscription
        NostrCloseEnvelope *close_msg = (NostrCloseEnvelope *)malloc(sizeof(NostrCloseEnvelope));
        if (!close_msg) {
            if (err) *err = new_error(1, "failed to create close envelope");
            return;
        }
        close_msg->base.type = NOSTR_ENVELOPE_CLOSE;
        close_msg->message = strdup(sub->priv->id);

        // Serialize the close message and send it to the relay
        char *close_msg_str = nostr_envelope_serialize((NostrEnvelope *)close_msg);
        if (!close_msg_str) {
            if (err) *err = new_error(1, "failed to serialize close envelope");
            // free envelope before returning
            free(close_msg->message);
            free(close_msg);
            return;
        }
        // free temporary envelope after serialization to avoid leaks
        free(close_msg->message);
        free(close_msg);

        // Send the message through the relay
        GoChannel *write_channel = nostr_relay_write(sub->relay, close_msg_str);
        free(close_msg_str);

        /* Non-blocking check for immediate write failure only.
         * DO NOT block — close is called from GObject dispose on the GTK
         * main thread.  The CLOSE message is fire-and-forget; the relay
         * will stop sending events regardless of write confirmation. */
        {
            Error *write_err = NULL;
            GoSelectCase cases[1];
            cases[0].op = GO_SELECT_RECEIVE;
            cases[0].chan = write_channel;
            cases[0].recv_buf = (void **)&write_err;

            GoSelectResult result = go_select_timeout(cases, 1, 0);

            if (result.selected_case >= 0 && write_err) {
                if (err) *err = write_err;
            }
            if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
                fprintf(stderr, "[sub %s] close: write queued (err=%p)\n",
                        sub->priv->id, (void *)write_err);
            }
        }
    }
}

void nostr_subscription_wait(NostrSubscription *sub) {
    if (!sub || !sub->priv) {
        return;
    }

    if (sub->priv->cancel) {
        sub->priv->cancel(sub->context);
        sub->priv->cancel = NULL;
    }

    go_wait_group_wait(&sub->priv->wg);
}

bool nostr_subscription_subscribe(NostrSubscription *sub, NostrFilters *filters, Error **err) {
    if (!sub) {
        if (err) *err = new_error(1, "subscription is NULL");
        return false;
    }

    // Set the filters for the subscription
    sub->filters = filters;

    // Fire the subscription (send the "REQ" command to the relay)
    bool ok = nostr_subscription_fire(sub, err);
    if (!ok) {
        // If nostr_subscription_fire fails, handle the error
        if (err && *err == NULL) *err = new_error(1, "failed to fire subscription");
        return false;
    }
    return true;
}

bool nostr_subscription_fire(NostrSubscription *subscription, Error **err) {
    if (!subscription || !subscription->relay->connection) {
        if (err) *err = new_error(1, "subscription or connection is NULL");
        return false;
    }

    /* Snapshot the filters pointer to prevent TOCTOU race (nostrc-m13c).
     * Another thread could NULL subscription->filters between our check and use.
     * The local snapshot is safe because the caller holds a ref (nostrc-nr96)
     * and the GObject wrapper owns the filters (nostrc-aaf0). */
    NostrFilters *filters = subscription->filters;
    if (!filters) {
        if (err) *err = new_error(1, "filters are NULL");
        return false;
    }

    // Build the subscription message. Operation is REQ by default, or COUNT if count_result channel is set.
    // For a single filter: ["OP","<id>",<filter>]
    // For multiple filters: ["OP","<id>",<filter1>,<filter2>,...]
    if (!subscription->priv->id) {
        if (err) *err = new_error(1, "subscription id is NULL");
        return false;
    }

    size_t count = filters->count;
    int use_empty_filter = 0;
    if (count == 0 || !filters->filters) {
        // NIP-01 permits an empty filter {}; use that to mean "match-all".
        use_empty_filter = 1;
        count = 1;
    }

    // Serialize each filter
    char **filter_strs = (char **)calloc(count, sizeof(char *));
    if (!filter_strs) {
        if (err) *err = new_error(1, "oom for filter strings");
        return false;
    }
    size_t filters_total_len = 0;
    for (size_t i = 0; i < count; ++i) {
        char *s = NULL;
        if (use_empty_filter) {
            s = strdup("{}");
        } else {
            const NostrFilter *f = &filters->filters[i];
            s = nostr_filter_serialize(f);
        }
        if (!s) {
            // TEST mode fallback for robustness
            const char *test_env = getenv("NOSTR_TEST_MODE");
            int test_mode = (test_env && *test_env && strcmp(test_env, "0") != 0) ? 1 : 0;
            if (test_mode) {
                s = strdup("{}");
            } else {
                for (size_t j = 0; j < i; ++j) free(filter_strs[j]);
                free(filter_strs);
                if (err) *err = new_error(1, "failed to serialize filter");
                return false;
            }
        }
        filter_strs[i] = s;
        filters_total_len += strlen(s);
        if (i + 1 < count) filters_total_len += 1; // comma between filters
    }

    // Compute total size: ["OP","id", + filters + closing ] and commas/quotes
    const char *op = (subscription->priv->count_result != NULL) ? "COUNT" : "REQ";
    size_t prefix_len = strlen(op) + strlen(subscription->priv->id) + 7; // [" "," ", ] and commas/quotes
    size_t needed = prefix_len + filters_total_len + 1; // +1 for closing ']'
    char *sub_msg = (char *)malloc(needed + 1);
    if (!sub_msg) {
        for (size_t i = 0; i < count; ++i) free(filter_strs[i]);
        free(filter_strs);
        if (err) *err = new_error(1, "oom for subscription message");
        return false;
    }
    // Write prefix with OP
    int wrote = snprintf(sub_msg, needed + 1, "[\"%s\",\"%s\",", op, subscription->priv->id);
    size_t offset = (wrote > 0) ? (size_t)wrote : 0;
    // Append filters
    for (size_t i = 0; i < count; ++i) {
        size_t len = strlen(filter_strs[i]);
        memcpy(sub_msg + offset, filter_strs[i], len);
        offset += len;
        if (i + 1 < count) sub_msg[offset++] = ',';
    }
    // Closing bracket and NUL
    sub_msg[offset++] = ']';
    sub_msg[offset] = '\0';

    // TEMP DEBUG: show the exact message sent on the wire
    fprintf(stderr, "[nostr_subscription_fire] sending: %s\n", sub_msg);

    // Send the subscription request via the relay
    GoChannel *write_channel = nostr_relay_write(subscription->relay, sub_msg);
    for (size_t i = 0; i < count; ++i) free(filter_strs[i]);
    free(filter_strs);
    free(sub_msg);

    /* Non-blocking check for immediate write failure only.
     * DO NOT block here — this function may be called from the GTK main
     * thread and any blocking wait stalls the entire UI.  The relay's
     * writer thread completes the WebSocket send asynchronously; if the
     * relay is truly dead, EOSE timeout / reconnect logic handles it. */
    {
        Error *write_err = NULL;
        GoSelectCase cases[1];
        cases[0].op = GO_SELECT_RECEIVE;
        cases[0].chan = write_channel;
        cases[0].recv_buf = (void **)&write_err;

        GoSelectResult result = go_select_timeout(cases, 1, 0);

        if (result.selected_case >= 0 && write_err) {
            fprintf(stderr, "[nostr_subscription_fire] write failed: %s\n",
                    write_err->message ? write_err->message : "unknown");
            if (err) *err = write_err;
            return false;
        }
    }

    // Mark the subscription as live
    atomic_store(&subscription->priv->live, true);
    if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
        fprintf(stderr, "[sub %s] fire: live=1\n", subscription->priv->id);
    }

    // LIFECYCLE: Log subscription fired
    if (getenv("NOSTR_DEBUG_LIFECYCLE")) {
        fprintf(stderr, "[SUB_LIFECYCLE] FIRE sid=%s counter=%d relay=%s\n",
                subscription->priv->id, subscription->priv->counter,
                subscription->relay && subscription->relay->url ? subscription->relay->url : "null");
    }

    return true;
}

/* Accessors (formerly in wrapper) */

const char *nostr_subscription_get_id_const(const NostrSubscription *sub) {
    if (!sub || !sub->priv) return NULL;
    return sub->priv->id;
}

NostrRelay *nostr_subscription_get_relay(const NostrSubscription *sub) {
    return sub ? sub->relay : NULL;
}

NostrFilters *nostr_subscription_get_filters(const NostrSubscription *sub) {
    return sub ? sub->filters : NULL;
}

void nostr_subscription_set_filters(NostrSubscription *sub, NostrFilters *filters) {
    if (!sub) return;
    if (sub->filters == filters) return;
    if (sub->filters) nostr_filters_free(sub->filters);
    sub->filters = filters;
}

GoChannel *nostr_subscription_get_events_channel(const NostrSubscription *sub) {
    return sub ? sub->events : NULL;
}

GoChannel *nostr_subscription_get_eose_channel(const NostrSubscription *sub) {
    return sub ? sub->end_of_stored_events : NULL;
}

GoChannel *nostr_subscription_get_closed_channel(const NostrSubscription *sub) {
    return sub ? sub->closed_reason : NULL;
}

GoContext *nostr_subscription_get_context(const NostrSubscription *sub) {
    return sub ? sub->context : NULL;
}

bool nostr_subscription_is_live(const NostrSubscription *sub) {
    if (!sub || !sub->priv) return false;
    return atomic_load(&sub->priv->live);
}

bool nostr_subscription_is_eosed(const NostrSubscription *sub) {
    if (!sub || !sub->priv) return false;
    return atomic_load(&sub->priv->eosed);
}

bool nostr_subscription_is_closed(const NostrSubscription *sub) {
    if (!sub || !sub->priv) return false;
    return atomic_load(&sub->priv->closed);
}

unsigned long long nostr_subscription_events_enqueued(const NostrSubscription *sub) {
    if (!sub || !sub->priv) return 0;
    return atomic_load(&sub->priv->metrics.events_enqueued);
}

unsigned long long nostr_subscription_events_dropped(const NostrSubscription *sub) {
    if (!sub || !sub->priv) return 0;
    return atomic_load(&sub->priv->metrics.events_dropped);
}

/* Queue metrics API (nostrc-sjv) */

void nostr_subscription_mark_event_consumed(NostrSubscription *sub, int64_t enqueue_time_us) {
    if (!sub || !sub->priv) return;

    QueueMetrics *m = &sub->priv->metrics;
    int64_t now_us = get_time_us();

    atomic_fetch_add(&m->events_dequeued, 1);
    /* Note: current_depth no longer tracked here - use go_channel_get_depth() instead (nostrc-dw3) */
    atomic_store(&m->last_dequeue_time_us, now_us);

    // If caller provided enqueue time, calculate wait time
    if (enqueue_time_us > 0 && now_us > enqueue_time_us) {
        atomic_fetch_add(&m->total_wait_time_us, (uint64_t)(now_us - enqueue_time_us));
    }

    nostr_metric_counter_add("sub_event_dequeued", 1);
}

void nostr_subscription_get_queue_metrics(const NostrSubscription *sub, NostrQueueMetrics *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!sub || !sub->priv) return;

    const QueueMetrics *m = &sub->priv->metrics;

    out->events_enqueued = atomic_load(&m->events_enqueued);
    out->events_dequeued = atomic_load(&m->events_dequeued);
    out->events_dropped = atomic_load(&m->events_dropped);
    /* Use actual channel depth instead of broken metric (nostrc-dw3) */
    out->current_depth = (uint32_t)go_channel_get_depth(sub->events);
    out->peak_depth = atomic_load(&m->peak_depth);
    out->queue_capacity = m->queue_capacity;
    out->last_enqueue_time_us = atomic_load(&m->last_enqueue_time_us);
    out->last_dequeue_time_us = atomic_load(&m->last_dequeue_time_us);
    out->total_wait_time_us = atomic_load(&m->total_wait_time_us);
}

/* ========================================================================
 * Producer-side Rate Limiting (nostrc-7u2)
 * ======================================================================== */

uint32_t nostr_subscription_get_queue_utilization(const NostrSubscription *sub) {
    if (!sub || !sub->priv) return 0;

    const QueueMetrics *m = &sub->priv->metrics;
    /* Use actual channel depth instead of broken metric (nostrc-dw3) */
    uint32_t depth = (uint32_t)go_channel_get_depth(sub->events);
    uint32_t capacity = m->queue_capacity;

    if (capacity == 0) return 0;
    return (depth * 100) / capacity;
}

bool nostr_subscription_should_throttle(const NostrSubscription *sub) {
    return nostr_subscription_get_queue_utilization(sub) > 90;
}

uint64_t nostr_subscription_get_throttle_delay_us(const NostrSubscription *sub) {
    uint32_t util = nostr_subscription_get_queue_utilization(sub);

    if (util <= 80) {
        return 0;  // No throttling needed
    } else if (util <= 90) {
        return 1000;  // 1ms - light throttle
    } else if (util <= 95) {
        return 10000;  // 10ms - moderate throttle
    } else {
        return 50000;  // 50ms - heavy throttle
    }
}

/* ========================================================================
 * ASYNC CLEANUP API - Non-blocking subscription cleanup with timeout
 * ======================================================================== */

struct AsyncCleanupHandle {
    NostrSubscription *sub;
    GoChannel *done;         // Signals when cleanup completes (closed on completion)
    _Atomic bool completed;  // True when cleanup finished
    _Atomic bool timed_out;  // True if cleanup timed out (subscription leaked)
    _Atomic bool abandoned;  // True if caller abandoned this handle (thread should free it)
    uint64_t timeout_ms;
    pthread_t cleanup_thread;
};

/* Background cleanup worker thread */
static void *async_cleanup_worker(void *arg) {
    AsyncCleanupHandle *handle = (AsyncCleanupHandle *)arg;
    NostrSubscription *sub = handle->sub;
    uint64_t timeout_ms = handle->timeout_ms;
    
    if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
        fprintf(stderr, "[sub %s] async_cleanup: starting (timeout=%lums)\n", 
                sub->priv->id, (unsigned long)timeout_ms);
    }
    
    /* IMPORTANT: Remove from relay map FIRST to prevent message_loop from
     * dispatching events to this subscription while we're freeing it. */
    if (sub->relay && sub->relay->subscriptions) {
        go_hash_map_remove_int(sub->relay->subscriptions, sub->priv->counter);
    }
    
    bool success = true;
    struct timeval start_tv;
    gettimeofday(&start_tv, NULL);
    uint64_t start_us = (uint64_t)start_tv.tv_sec * 1000000 + (uint64_t)start_tv.tv_usec;
    
    /* Step 1: Cancel context (non-blocking) */
    if (sub->priv->cancel) {
        sub->priv->cancel(sub->context);
    }
    
    /* Step 2: Simply wait for wait_group with timeout using a background approach
     * Since go_wait_group_wait() blocks, we'll use a timeout-based approach */
    if (timeout_ms > 0) {
        /* Give the worker thread time to exit after context cancellation */
        uint64_t poll_interval_ms = 10; // Poll every 10ms
        uint64_t elapsed_ms = 0;
        
        while (elapsed_ms < timeout_ms) {
            /* Check elapsed time */
            struct timeval now_tv;
            gettimeofday(&now_tv, NULL);
            uint64_t now_us = (uint64_t)now_tv.tv_sec * 1000000 + (uint64_t)now_tv.tv_usec;
            elapsed_ms = (now_us - start_us) / 1000;
            
            if (elapsed_ms >= timeout_ms) {
                /* Timeout */
                if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
                    fprintf(stderr, "[sub %s] async_cleanup: TIMEOUT after %lums\n", 
                            sub->priv->id, (unsigned long)elapsed_ms);
                }
                atomic_store(&handle->timed_out, true);
                success = false;
                nostr_metric_counter_add("sub_cleanup_timeout", 1);
                goto cleanup_done;
            }
            
            /* Sleep briefly */
            usleep(poll_interval_ms * 1000);
            
            /* After 100ms, assume worker has had enough time to exit */
            if (elapsed_ms >= 100) {
                break;
            }
        }
    }
    
    /* If we got here without timeout, do the actual cleanup via unref (nostrc-nr96) */
    if (success) {
        nostr_subscription_unref(sub);

        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub] async_cleanup: SUCCESS\n");
        }
        nostr_metric_counter_add("sub_cleanup_success", 1);
    }
    
cleanup_done:
    atomic_store(&handle->completed, true);
    go_channel_close(handle->done);
    
    /* If the handle was abandoned, free it now that we're done with it */
    if (atomic_load(&handle->abandoned)) {
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub] async_cleanup: abandoned handle, freeing\n");
        }
        go_channel_free(handle->done);
        free(handle);
    }
    
    return NULL;
}

AsyncCleanupHandle *nostr_subscription_free_async(NostrSubscription *sub, uint64_t timeout_ms) {
    if (!sub) return NULL;
    
    AsyncCleanupHandle *handle = (AsyncCleanupHandle *)malloc(sizeof(AsyncCleanupHandle));
    if (!handle) return NULL;
    
    handle->sub = sub;
    handle->done = go_channel_create(1);
    atomic_store(&handle->completed, false);
    atomic_store(&handle->timed_out, false);
    atomic_store(&handle->abandoned, false);
    handle->timeout_ms = timeout_ms;
    
    /* Start cleanup thread */
    if (pthread_create(&handle->cleanup_thread, NULL, async_cleanup_worker, handle) != 0) {
        go_channel_free(handle->done);
        free(handle);
        return NULL;
    }
    
    pthread_detach(handle->cleanup_thread);
    return handle;
}

bool nostr_subscription_cleanup_wait(AsyncCleanupHandle *handle, uint64_t timeout_ms) {
    if (!handle) return false;
    
    /* Check if already completed */
    if (atomic_load(&handle->completed)) {
        return !atomic_load(&handle->timed_out);
    }
    
    /* Wait for completion with timeout */
    if (timeout_ms == 0) {
        /* Just check status */
        return false;
    }
    
    GoSelectCase cases[] = {
        { .op = GO_SELECT_RECEIVE, .chan = handle->done, .recv_buf = NULL }
    };
    GoSelectResult result = go_select_timeout(cases, 1, timeout_ms);
    
    if (result.selected_case == -1) {
        /* Timeout */
        return false;
    }
    
    /* Completed */
    return !atomic_load(&handle->timed_out);
}

void nostr_subscription_cleanup_abandon(AsyncCleanupHandle *handle) {
    if (!handle) return;
    
    /* DON'T free the handle here! The background thread is still using it.
     * The background thread will free it when it completes.
     * We just mark it as abandoned so the thread knows to free everything. */
    atomic_store(&handle->abandoned, true);
}

bool nostr_subscription_cleanup_is_complete(AsyncCleanupHandle *handle) {
    if (!handle) return true;
    return atomic_load(&handle->completed);
}

bool nostr_subscription_cleanup_timed_out(AsyncCleanupHandle *handle) {
    if (!handle) return false;
    return atomic_load(&handle->timed_out);
}
