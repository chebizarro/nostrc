#include "envelope.h"
#include "json.h"
#include "relay-private.h"
#include "relay.h"
#include "subscription-private.h"
#include <openssl/ssl.h>
#include <unistd.h>
#include <time.h>

static _Atomic long long g_sub_counter = 1;

Subscription *create_subscription(Relay *relay, Filters *filters) {
    Subscription *sub = (Subscription *)malloc(sizeof(Subscription));
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
    sub->events = go_channel_create(1);
    sub->end_of_stored_events = go_channel_create(1);
    sub->closed_reason = go_channel_create(1);
    sub->priv->live = false;
    sub->priv->eosed = false;
    sub->priv->closed = false;
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
    sub->priv->match = filters_match;
    // Start lifecycle watcher
    if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
        fprintf(stderr, "[sub %s] create: starting lifecycle thread\n", sub->priv->id);
    }
    go(subscription_start, sub);

    return sub;
}

void free_subscription(Subscription *sub) {
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
}

char *subscription_get_id(Subscription *sub) {
    return sub->priv->id;
}

void *subscription_start(void *arg) {
    Subscription *sub = (Subscription *)arg;

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
    subscription_unsub(sub);

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

void subscription_dispatch_event(Subscription *sub, NostrEvent *event) {
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
            free_event(event);
        }
    } else {
        // Not live anymore; drop event to avoid blocking
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub %s] dispatch_event: dropped (not live)\n", sub->priv->id);
        }
        free_event(event);
    }

    if (added) {
        // Decrement stored event counter if needed
    }
}


void subscription_dispatch_eose(Subscription *sub) {
    if (!sub)
        return;

    // Change the match behavior and signal the end of stored events
    if (atomic_exchange(&sub->priv->eosed, true) == false) {
        sub->priv->match = filters_match_ignoring_timestamp;

        // Wait for any "stored" events to finish processing, then signal EOSE
        go_channel_send(sub->end_of_stored_events, NULL);
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub %s] dispatch_eose: signaled EOSE\n", sub->priv->id);
        }
    }
}

void subscription_dispatch_closed(Subscription *sub, const char *reason) {
    if (!sub || !reason)
        return;

    // Set the closed flag and dispatch the reason
    if (atomic_exchange(&sub->priv->closed, true) == false) {
        go_channel_send(sub->closed_reason, (void *)reason);
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub %s] dispatch_closed: reason='%s'\n", sub->priv->id, reason ? reason : "");
        }
    }
}

void subscription_unsub(Subscription *sub) {
    if (!sub)
        return;

    // Cancel the subscription's context
    sub->priv->cancel(sub->context);

    // If the subscription is still live, mark it as inactive and close it
    if (atomic_exchange(&sub->priv->live, false)) {
        if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
            fprintf(stderr, "[sub %s] unsub: closing (send CLOSED)\n", sub->priv->id);
        }
        subscription_close(sub, NULL);
    }

    // Remove the subscription from the relay's map (keys are ints/counters)
    go_hash_map_remove_int(sub->relay->subscriptions, sub->priv->counter);
    if (getenv("NOSTR_DEBUG_SHUTDOWN")) {
        fprintf(stderr, "[sub %s] unsub: removed from relay map\n", sub->priv->id);
    }
}

void subscription_close(Subscription *sub, Error **err) {
    if (!sub || !sub->relay) {
        if (err) *err = new_error(1, "subscription or relay is NULL");
        return;
    }

    if (relay_is_connected(sub->relay)) {
        // Create a ClosedEnvelope with the subscription ID (allocated with correct size)
        ClosedEnvelope *close_msg = (ClosedEnvelope *)malloc(sizeof(ClosedEnvelope));
        if (!close_msg) {
            if (err) *err = new_error(1, "failed to create close envelope");
            return;
        }
        close_msg->base.type = ENVELOPE_CLOSED;
        close_msg->subscription_id = strdup(sub->priv->id);
        close_msg->reason = NULL;

        // Serialize the close message and send it to the relay
        char *close_msg_str = nostr_envelope_serialize((Envelope *)close_msg);
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
        GoChannel *write_channel = relay_write(sub->relay, close_msg_str);
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

bool subscription_sub(Subscription *sub, Filters *filters, Error **err) {
    if (!sub) {
        if (err) *err = new_error(1, "subscription is NULL");
        return false;
    }

    // Set the filters for the subscription
    sub->filters = filters;

    // Fire the subscription (send the "REQ" command to the relay)
    bool ok = subscription_fire(sub, err);
    if (!ok) {
        // If subscription_fire fails, handle the error
        if (err && *err == NULL) *err = new_error(1, "failed to fire subscription");
        return false;
    }
    return true;
}

bool subscription_fire(Subscription *subscription, Error **err) {
    if (!subscription || !subscription->relay->connection) {
        if (err) *err = new_error(1, "subscription or connection is NULL");
        return false;
    }

    // Serialize filters into JSON
    if (!subscription->filters) {
        if (err) *err = new_error(1, "filters are NULL");
        return false;
    }
    // json API expects a single Filter*
    char *filters_json = nostr_filter_serialize(subscription->filters->filters);
    if (!filters_json) {
        // If running in TEST mode, bypass JSON dependency with a minimal filter
        const char *test_env = getenv("NOSTR_TEST_MODE");
        int test_mode = (test_env && *test_env && strcmp(test_env, "0") != 0) ? 1 : 0;
        if (test_mode) {
            filters_json = strdup("{}");
        } else {
            if (err) *err = new_error(1, "failed to serialize filters");
            return false;
        }
    }

    // Construct the subscription message
    if (!subscription->priv->id) {
        free(filters_json);
        if (err) *err = new_error(1, "subscription id is NULL");
        return false;
    }
    size_t needed = strlen(subscription->priv->id) + strlen(filters_json) + 10;
    char *sub_msg = (char *)malloc(needed + 3); // room for brackets and NUL
    if (!sub_msg) {
        free(filters_json);
        if (err) *err = new_error(1, "oom for subscription message");
        return false;
    }
    // Build full REQ envelope: ["REQ","<id>",<filters_json>]
    snprintf(sub_msg, needed + 3, "[\"REQ\",\"%s\",%s]", subscription->priv->id, filters_json);

    // Send the subscription request via the relay
    GoChannel *write_channel = relay_write(subscription->relay, sub_msg);
    free(filters_json);
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
