#include "envelope.h"
#include "json.h"
#include "relay-private.h"
#include "relay.h"
#include "subscription-private.h"
#include <openssl/ssl.h>

// Subscription-related functions
Subscription *create_subscription(Relay *relay, Filters *filters, const char *label) {
    Subscription *sub = (Subscription *)malloc(sizeof(Subscription));
    if (!sub)
        return NULL;

    sub->priv->label = strdup(label);
    sub->priv->counter = 0;
    sub->relay = relay;
    sub->filters = filters;
    sub->priv->count_result = NULL;
    sub->events = NULL;
    sub->closed_reason = NULL;
    sub->priv->live = false;
    sub->priv->eosed = false;
    sub->priv->closed = false;
    pthread_mutex_init(&sub->priv->sub_mutex, NULL);

    return sub;
}

void free_subscription(Subscription *sub) {
    free(sub->priv->label);
    free(sub->closed_reason);
    go_channel_free(sub->events);
    pthread_mutex_destroy(&sub->priv->sub_mutex);
    free(sub);
}

char *subscription_get_id(Subscription *sub) {
    return sub->id;
}

void *subscription_thread_func(void *arg) {
    Subscription *sub = (Subscription *)arg;
    pthread_mutex_lock(&sub->priv->sub_mutex);

    pthread_mutex_unlock(&sub->priv->sub_mutex);
    return NULL;
}

void subscription_start(void *arg) {
    Subscription *sub = (Subscription *)arg;
    while (!go_context_is_canceled(sub->priv->context)) {
    }
    subscription_unsub(sub);
    // go_channel_close(sub->events)
}

void subscription_dispatch_event(Subscription *sub, NostrEvent *event) {
    go_channel_send(sub->events, event);
}

void subscription_dispatch_eose(Subscription *sub) {
    atomic_store(&sub->priv->eosed, true);
    sub->priv->eosed = true;
}

void subscription_dispatch_closed(Subscription *sub, const char *reason) {
    atomic_store(&sub->priv->closed, true);
    go_channel_send(sub->closed_reason, (void *)reason);
}

void subscription_unsub(Subscription *sub) {
    atomic_store(&sub->priv->live, false);
    pthread_cancel(sub->priv->thread);
    subscription_close(sub);
}

void subscription_close(Subscription *sub) {
    if (relay_is_connected(sub->relay)) {
        ClosedEnvelope *close_msg = (ClosedEnvelope *)create_envelope(ENVELOPE_CLOSED);
        close_msg->subscription_id = strdup(sub->id);
        char *close_b = nostr_envelope_serialize((Envelope *)close_msg);
        // relay_write(sub->relay, close_b);
    }
}

void subscription_sub(Subscription *sub, Filters *filters) {
    sub->filters = filters;
    subscription_fire(sub);
}

// static void *sub_error(void *arg) {
// subscription_cancel();
//}

int subscription_fire(Subscription *subscription, Error **err) {
    if (!subscription || !subscription->relay->connection) {
        *err = new_error(1, "subscription or connection is NULL");
        return -1;
    }

    // Serialize filters into JSON
    char *filters_json = filters_serialize(subscription->filters);
    if (!filters_json) {
        *err = new_error(1, "failed to serialize filters");
        return -1;
    }

    // Construct the subscription message
    char *sub_id_str = malloc(32);  // Allocate memory for the subscription ID string
    snprintf(sub_id_str, 32, "\"REQ\",\"%s\",%s", subscription->id, filters_json);

    // Send the subscription request via the relay
    GoChannel* write_channel = relay_write(subscription->relay, sub_id_str);
    free(filters_json);
    free(sub_id_str);

    // Wait for a response
    Error *write_err = NULL;
    go_channel_receive(write_channel, &write_err);
    if (write_err) {
        *err = write_err;
        return -1;
    }

    return 0;
}
