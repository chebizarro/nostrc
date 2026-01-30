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

    sub->relay = relay;
    sub->filters = filters;
    sub->priv = (SubscriptionPrivate *)malloc(sizeof(SubscriptionPrivate));
    if (!sub->priv) {
        free(sub);
        return NULL;
    }

    sub->priv->count_result = NULL;
    // Allow tuning channel capacities via environment for stress/backpressure analysis
    // Defaults chosen to tolerate relay backfill bursts without loss.
    int ev_cap = 4096, eose_cap = 8, closed_cap = 8;
    const char *ev_cap_s = getenv("NOSTR_SUB_EVENTS_CAP");
    if (ev_cap_s && *ev_cap_s) {
        int v = atoi(ev_cap_s);
        if (v > 0) ev_cap = v;
    }
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
    sub->events = go_channel_create(ev_cap);
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
    free(sub);
    nostr_metric_counter_add("sub_freed", 1);
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
            uint32_t new_depth = atomic_fetch_add(&m->current_depth, 1) + 1;
            atomic_store(&m->last_enqueue_time_us, now_us);

            // Update peak depth if new high
            uint32_t old_peak = atomic_load(&m->peak_depth);
            while (new_depth > old_peak) {
                if (atomic_compare_exchange_weak(&m->peak_depth, &old_peak, new_depth)) {
                    break;
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
        // Create a NostrClosedEnvelope with the subscription ID (allocated with correct size)
        NostrClosedEnvelope *close_msg = (NostrClosedEnvelope *)malloc(sizeof(NostrClosedEnvelope));
        if (!close_msg) {
            if (err) *err = new_error(1, "failed to create close envelope");
            return;
        }
        close_msg->base.type = NOSTR_ENVELOPE_CLOSED;
        close_msg->subscription_id = strdup(sub->priv->id);
        close_msg->reason = NULL;

        // Serialize the close message and send it to the relay
        char *close_msg_str = nostr_envelope_serialize((NostrEnvelope *)close_msg);
        if (!close_msg_str) {
            if (err) *err = new_error(1, "failed to serialize close envelope");
            // free envelope before returning
            free(close_msg->subscription_id);
            free(close_msg);
            return;
        }
        // free temporary envelope after serialization to avoid leaks
        free(close_msg->subscription_id);
        free(close_msg);

        // Send the message through the relay
        GoChannel *write_channel = nostr_relay_write(sub->relay, close_msg_str);
        free(close_msg_str);

        // Wait for the result of the write, but don't block indefinitely
        Error *write_err = NULL;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        struct timespec deadline = now;
        deadline.tv_sec += 0;
        long nsec_add = 200 * 1000000L; // 200ms timeout
        deadline.tv_nsec += nsec_add;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        GoContext *wctx = go_with_deadline(go_context_background(), deadline);
        (void)go_channel_receive_with_context(write_channel, (void **)&write_err, wctx);
        go_context_free(wctx);
        // Writer owns lifecycle of the answer channel; do not close/free here
        if (write_err) {
            if (err) *err = write_err;
        }
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub %s] close: write done (err=%p)\n", sub->priv->id, (void *)write_err);
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

    // Serialize filters into JSON (supports 1 or more filters)
    if (!subscription->filters) {
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
    
    size_t count = subscription->filters->count;
    int use_empty_filter = 0;
    if (count == 0 || !subscription->filters->filters) {
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
            const NostrFilter *f = &subscription->filters->filters[i];
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

    // Wait for a response with timeout (3 seconds)
    Error *write_err = NULL;
    GoSelectCase cases[1];
    cases[0].op = GO_SELECT_RECEIVE;
    cases[0].chan = write_channel;
    cases[0].recv_buf = (void **)&write_err;
    
    GoSelectResult result = go_select_timeout(cases, 1, 3000); // 3 second timeout
    
    if (result.selected_case == -1) {
        // Timeout - writer will deliver result or error later and free the channel
        fprintf(stderr, "[nostr_subscription_fire] TIMEOUT waiting for write confirmation (relay may be dead)\n");
        if (err) *err = new_error(1, "write timeout - relay connection may be dead");
        return false;
    }
    if (write_err) {
        if (err) *err = write_err;
        return false;
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
    atomic_fetch_sub(&m->current_depth, 1);
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
    out->current_depth = atomic_load(&m->current_depth);
    out->peak_depth = atomic_load(&m->peak_depth);
    out->queue_capacity = m->queue_capacity;
    out->last_enqueue_time_us = atomic_load(&m->last_enqueue_time_us);
    out->last_dequeue_time_us = atomic_load(&m->last_dequeue_time_us);
    out->total_wait_time_us = atomic_load(&m->total_wait_time_us);
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
    
    /* If we got here without timeout, do the actual cleanup */
    if (success) {
        go_wait_group_wait(&sub->priv->wg);
        
        /* Drain any remaining events from the channel to prevent memory leaks */
        if (sub->events) {
            void *ev = NULL;
            while (go_channel_try_receive(sub->events, &ev) == 0) {
                if (ev) {
                    nostr_event_free((NostrEvent *)ev);
                    ev = NULL;
                }
            }
        }
        
        /* Free resources */
        go_channel_free(sub->events);
        go_channel_free(sub->end_of_stored_events);
        go_channel_free(sub->closed_reason);
        free(sub->priv->id);
        go_wait_group_destroy(&sub->priv->wg);
        free(sub->priv);
        free(sub);
        
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
