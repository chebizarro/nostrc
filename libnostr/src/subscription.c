#include "envelope.h"
#include "json.h"
#include "relay-private.h"
#include "relay.h"
#include "subscription-private.h"
#include <openssl/ssl.h>
#include <unistd.h>

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
    sub->closed_reason = go_channel_create(1);
    sub->priv->live = false;
    sub->priv->eosed = false;
    sub->priv->closed = false;
    pthread_mutex_init(&sub->priv->sub_mutex, NULL);

    return sub;
}

void free_subscription(Subscription *sub) {
    if (!sub)
        return;

    go_channel_free(sub->events);
    go_channel_free(sub->closed_reason);
    pthread_mutex_destroy(&sub->priv->sub_mutex);
    free(sub->priv);
    free(sub);
}

char *subscription_get_id(Subscription *sub) {
    return sub->priv->id;
}

void *subscription_thread_func(void *arg) {
    Subscription *sub = (Subscription *)arg;
    pthread_mutex_lock(&sub->priv->sub_mutex);

    pthread_mutex_unlock(&sub->priv->sub_mutex);
    return NULL;
}

void *subscription_start(void *arg) {
    Subscription *sub = (Subscription *)arg;

    // Wait for the subscription context to be canceled
    while (!go_context_is_canceled(sub->context)) {
        sleep(1); // Small sleep to avoid busy waiting
    }

    // Once the context is canceled, unsubscribe the subscription
    subscription_unsub(sub);

    // Lock the subscription to avoid race conditions
    pthread_mutex_lock(&sub->priv->sub_mutex);

    // Close the events channel
    go_channel_close(sub->events);

    // Unlock the mutex after the events channel is closed
    pthread_mutex_unlock(&sub->priv->sub_mutex);

    return NULL;
}

void subscription_dispatch_event(Subscription *sub, NostrEvent *event) {
    if (!sub || !event)
        return;

    bool added = false;
    if (!atomic_load(&sub->priv->eosed)) {
        added = true;
        // Increment a "stored" event counter (if needed)
    }

    pthread_mutex_lock(&sub->priv->sub_mutex);

    if (atomic_load(&sub->priv->live)) {
        go_channel_send(sub->events, event);
    }

    pthread_mutex_unlock(&sub->priv->sub_mutex);

    if (added) {
        // Decrement "stored" event counter (if needed)
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
    }
}

void subscription_dispatch_closed(Subscription *sub, const char *reason) {
    if (!sub || !reason)
        return;

    // Set the closed flag and dispatch the reason
    if (atomic_exchange(&sub->priv->closed, true) == false) {
        go_channel_send(sub->closed_reason, (void *)reason);
    }
}

void subscription_unsub(Subscription *sub) {
    if (!sub)
        return;

    // Cancel the subscription's context
    sub->priv->cancel(sub->context);

    // If the subscription is still live, mark it as inactive and close it
    if (atomic_exchange(&sub->priv->live, false)) {
        subscription_close(sub, NULL);
    }

    // Remove the subscription from the relay's map
    go_hash_map_remove_str(sub->relay->subscriptions, sub->priv->id);
}

void subscription_close(Subscription *sub, Error **err) {
    if (!sub || !sub->relay) {
        *err = new_error(1, "subscription or relay is NULL");
        return;
    }

    if (relay_is_connected(sub->relay)) {
        // Create a CloseEnvelope with the subscription ID
        ClosedEnvelope *close_msg = (ClosedEnvelope *)create_envelope(ENVELOPE_CLOSED);
        if (!close_msg) {
            *err = new_error(1, "failed to create close envelope");
            return;
        }
        close_msg->subscription_id = strdup(sub->priv->id);

        // Serialize the close message and send it to the relay
        char *close_msg_str = nostr_envelope_serialize((Envelope *)close_msg);
        if (!close_msg_str) {
            *err = new_error(1, "failed to serialize close envelope");
            return;
        }

        // Send the message through the relay
        GoChannel *write_channel = relay_write(sub->relay, close_msg_str);
        free(close_msg_str);

        // Wait for the result of the write
        Error *write_err = NULL;
        go_channel_receive(write_channel, (void **)write_err);
        if (write_err) {
            *err = write_err;
        }
    }
}

bool subscription_sub(Subscription *sub, Filters *filters, Error **err) {
    if (!sub) {
        *err = new_error(1, "subscription is NULL");
        return false;
    }

    // Set the filters for the subscription
    sub->filters = filters;

    // Fire the subscription (send the "REQ" command to the relay)
    int result = subscription_fire(sub, err);
    if (result < 0) {
        // If subscription_fire fails, handle the error
        *err = new_error(1, "failed to fire subscription");
        return false;
    }
    return true;
}

bool subscription_fire(Subscription *subscription, Error **err) {
    if (!subscription || !subscription->relay->connection) {
        *err = new_error(1, "subscription or connection is NULL");
        return false;
    }

    // Serialize filters into JSON
    char *filters_json = nostr_filter_serialize(subscription->filters);
    if (!filters_json) {
        *err = new_error(1, "failed to serialize filters");
        return false;
    }

    // Construct the subscription message
    char *sub_id_str = malloc(32); // Allocate memory for the subscription ID string
    snprintf(sub_id_str, 32, "\"REQ\",\"%s\",%s", subscription->priv->id, filters_json);

    // Send the subscription request via the relay
    GoChannel *write_channel = relay_write(subscription->relay, sub_id_str);
    free(filters_json);
    free(sub_id_str);

    // Wait for a response
    Error *write_err = NULL;
    go_channel_receive(write_channel, (void *)&write_err);
    if (write_err) {
        *err = write_err;
        return -1;
    }

    // Mark the subscription as live
    subscription->priv->live = true;

    return 0;
}
