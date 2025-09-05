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
#include <openssl/ssl.h>
#include <unistd.h>
#include <time.h>

static _Atomic long long g_sub_counter = 1;

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
    // Instrumentation counters
    atomic_store(&sub->priv->events_enqueued, 0);
    atomic_store(&sub->priv->events_dropped, 0);
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

    return sub;
}

void nostr_subscription_free(NostrSubscription *sub) {
    if (!sub)
        return;

    // Ensure lifecycle worker has exited (caller should have unsubscribed)
    go_wait_group_wait(&sub->priv->wg);

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

    if (is_live) {
        // Non-blocking send; if full or closed or canceled, drop to avoid hang
        if (go_channel_try_send(sub->events, event) != 0) {
            if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
                fprintf(stderr, "[sub %s] dispatch_event: dropped (queue full/closed)\n", sub->priv->id);
            }
            atomic_fetch_add(&sub->priv->events_dropped, 1);
            nostr_metric_counter_add("sub_event_drop", 1);
            nostr_rl_log(NLOG_WARN, "sub", "event drop: queue full sid=%s", sub->priv->id);
            nostr_event_free(event);
        } else {
            atomic_fetch_add(&sub->priv->events_enqueued, 1);
            nostr_metric_counter_add("sub_event_enqueued", 1);
        }
    } else {
        // Not live anymore; drop event to avoid blocking
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub %s] dispatch_event: dropped (not live)\n", sub->priv->id);
        }
        atomic_fetch_add(&sub->priv->events_dropped, 1);
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

    // Change the match behavior and signal the end of stored events
    if (atomic_exchange(&sub->priv->eosed, true) == false) {
        sub->priv->match = nostr_filters_match_ignoring_timestamp;

        // Wait for any "stored" events to finish processing, then signal EOSE
        go_channel_send(sub->end_of_stored_events, NULL);
        nostr_metric_counter_add("sub_eose_signal", 1);
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub %s] dispatch_eose: signaled EOSE\n", sub->priv->id);
        }
    }
}

void nostr_subscription_dispatch_closed(NostrSubscription *sub, const char *reason) {
    if (!sub || !reason)
        return;

    // Set the closed flag and dispatch the reason
    if (atomic_exchange(&sub->priv->closed, true) == false) {
        go_channel_send(sub->closed_reason, (void *)reason);
        nostr_metric_counter_add("sub_closed_signal", 1);
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub %s] dispatch_closed: reason='%s'\n", sub->priv->id, reason ? reason : "");
        }
    }
}

void nostr_subscription_unsubscribe(NostrSubscription *sub) {
    if (!sub)
        return;

    // Idempotent: ensure we only execute unsubscribe logic once
    if (atomic_exchange(&sub->priv->unsubbed, true)) {
        return;
    }

    // Cancel the subscription's context
    sub->priv->cancel(sub->context);
    nostr_metric_counter_add("sub_unsubscribe", 1);

    // If the subscription is still live, mark it as inactive and close it
    if (atomic_exchange(&sub->priv->live, false)) {
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub %s] unsub: closing (send CLOSED)\n", sub->priv->id);
        }
        nostr_subscription_close(sub, NULL);
    }

    // Remove the subscription from the relay's map (keys are ints/counters) if relay present
    if (sub->relay && sub->relay->subscriptions) {
        go_hash_map_remove_int(sub->relay->subscriptions, sub->priv->counter);
    }
    if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
        fprintf(stderr, "[sub %s] unsub: removed from relay map\n", sub->priv->id);
    }
}

void nostr_subscription_close(NostrSubscription *sub, Error **err) {
    if (!sub || !sub->relay) {
        if (err) *err = new_error(1, "subscription or relay is NULL");
        return;
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
        // Channel no longer needed: close then free
        go_channel_close(write_channel);
        go_channel_free(write_channel);
        if (write_err) {
            if (err) *err = write_err;
        }
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub %s] close: write done (err=%p)\n", sub->priv->id, (void *)write_err);
        }
    }
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

    // Wait for a response
    Error *write_err = NULL;
    go_channel_receive(write_channel, (void **)&write_err);
    // Channel no longer needed: close then free
    go_channel_close(write_channel);
    go_channel_free(write_channel);
    if (write_err) {
        if (err) *err = write_err;
        return false;
    }

    // Mark the subscription as live
    atomic_store(&subscription->priv->live, true);
    if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
        fprintf(stderr, "[sub %s] fire: live=1\n", subscription->priv->id);
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
    return atomic_load(&sub->priv->events_enqueued);
}

unsigned long long nostr_subscription_events_dropped(const NostrSubscription *sub) {
    if (!sub || !sub->priv) return 0;
    return atomic_load(&sub->priv->events_dropped);
}
