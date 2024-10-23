#include "relay.h"
#include "envelope.h"
#include "error_codes.h"
#include "json.h"
#include "kinds.h"
#include "relay-private.h"
#include "subscription-private.h"
#include "subscription.h"
#include "utils.h"
#include <unistd.h>

Relay *new_relay(GoContext *context, const char *url, Error **err) {
    if (url == NULL) {
        *err = new_error(1, "invalid relay URL");
        return NULL;
    }

    CancelContextResult cancellabe = go_context_with_cancel(context);

    Relay *relay = (Relay *)calloc(1, sizeof(Relay));
    RelayPrivate *priv = (RelayPrivate *)calloc(1, sizeof(RelayPrivate));
    if (!relay || !priv) {
        *err = new_error(1, "failed to allocate memory for Relay struct");
        return NULL;
    }

    relay->url = strdup(url);
    relay->subscriptions = go_hash_map_create(16);
    relay->assume_valid = false;

    relay->priv = priv;
    nsync_mu_init(&relay->priv->mutex);
    relay->priv->connection_context = cancellabe.context;
    relay->priv->connection_context_cancel = cancellabe.cancel;
    relay->priv->ok_callbacks = go_hash_map_create(16);
    relay->priv->write_queue = go_channel_create(16);
    relay->priv->subscription_channel_close_queue = go_channel_create(16);
    // request_header

    relay->priv->notice_handler = NULL;
    relay->priv->custom_handler = NULL;

    return relay;
}

void free_relay(Relay *relay) {
    if (relay) {
        free(relay->url);
        //if (*relay->connection_error) free_error(*relay->connection_error);
        go_hash_map_destroy(relay->subscriptions);
        go_context_free(relay->priv->connection_context);
        free(relay->priv->challenge);
        go_hash_map_destroy(relay->priv->ok_callbacks);
        go_channel_close(relay->priv->write_queue);
        go_channel_close(relay->priv->subscription_channel_close_queue);
        free(relay->priv);
        free(relay);
    }
}

bool relay_is_connected(Relay *relay) {
    Error *err = go_context_err(relay->priv->connection_context);
    if (err) {
        free_error(err);
        return false;
    }
    return true;
}

bool sub_foreach_unsub(HashKey *key, void *sub) {
    subscription_unsub((Subscription *)sub);
    return true;
}

void *cleanup_routine(void *arg) {
    Relay *r = (Relay *)arg;
    Ticker *t = (Ticker *)(arg + sizeof(Relay));
    // Wait for connection context to be done
    nsync_mu_lock(&r->priv->mutex);
    r->priv->connection_context_cancel(r->priv->connection_context);
    stop_ticker(t);
    connection_close(r->connection);
    r->connection = NULL;
    nsync_mu_unlock(&r->priv->mutex);
    go_hash_map_for_each(r->subscriptions, sub_foreach_unsub);
    return NULL;
}

void *write_operations(void *arg) {
    Relay *r = (Relay *)arg;

    // Initialize the write request
    write_request write_req = {
        .answer = go_channel_create(1),
        .msg = (char *)malloc(4 * sizeof(char))};

    while (true) {
        // Wait for data in the write queue or for the context to be canceled
        nsync_mu_lock(&r->priv->mutex);
        nsync_mu_wait(&r->priv->mutex, &go_channel_has_data, r->priv->write_queue, NULL);
        nsync_mu_unlock(&r->priv->mutex);

        if (go_context_is_canceled(r->priv->connection_context)) {
            break; // Exit the thread if the connection is closed
        }

        // Dequeue and process the write request
        Error *err = NULL;
        nsync_mu_lock(&r->priv->mutex);
        connection_write_message(r->connection, r->priv->connection_context, write_req.msg, &err);
        nsync_mu_unlock(&r->priv->mutex);

        // Handle error and free resources
        if (err != NULL) {
            go_channel_send(write_req.answer, err);
        }
        go_channel_free(write_req.answer); // Free the channel
    }

    return NULL;
}

void *message_loop(void *arg) {
    Relay *r = (Relay *)arg;
    char *buf = (char *)malloc(1024 * sizeof(char)); // Allocate a buffer for reading messages
    Error *err = NULL;                               // Initialize the error pointer

    while (true) {
        // Reset buffer for each loop iteration
        memset(buf, 0, 1024); // Clear the buffer

        nsync_mu_lock(&r->priv->mutex);

        // Read the message into buf, passing the error pointer
        connection_read_message(r->connection, r->priv->connection_context, buf, 1024, &err);

        // Check if an error occurred
        if (err != NULL) {
            r->connection_error = &err;
            nsync_mu_unlock(&r->priv->mutex); // Unlock before closing the connection
            Error *cerr = NULL;
            relay_close(r, &cerr); // Pass the error pointer to relay_close
            break;
        }

        nsync_mu_unlock(&r->priv->mutex); // Unlock after processing the message

        const char *message = buf; // Use the buffer to get the message

        Envelope *envelope = parse_message(message);
        if (!envelope) {
            if (r->priv->custom_handler) {
                r->priv->custom_handler(message); // Call custom handler if no envelope
            }
            continue;
        }

        nsync_mu_lock(&r->priv->mutex); // Lock again for processing the envelope
        switch (envelope->type) {
        case ENVELOPE_NOTICE:
            if (r->priv->notice_handler) {
                r->priv->notice_handler((char *)envelope); // Handle notice
            }
            break;
        case ENVELOPE_AUTH:
            r->priv->challenge = ((AuthEnvelope *)envelope)->challenge; // Handle auth
            break;
        case ENVELOPE_EVENT: {
            EventEnvelope *env = (EventEnvelope *)envelope;
            Subscription *subscription = go_hash_map_get_int(r->subscriptions, sub_id_to_serial(env->subscription_id));
            if (subscription && event_check_signature(env->event)) {
                subscription_dispatch_event(subscription, env->event); // Dispatch event
            }
            break;
        }
        case ENVELOPE_CLOSED: {
            ClosedEnvelope *env = (ClosedEnvelope *)envelope;
            Subscription *subscription = go_hash_map_get_int(r->subscriptions, sub_id_to_serial(env->subscription_id));
            if (subscription) {
                subscription_dispatch_closed(subscription, env->reason); // Dispatch closed event
            }
            break;
        }
        case ENVELOPE_COUNT: {
            CountEnvelope *env = (CountEnvelope *)envelope;
            Subscription *subscription = go_hash_map_get_int(r->subscriptions, sub_id_to_serial(env->subscription_id));
            if (subscription) {
                go_channel_send(subscription->priv->count_result, &env->count); // Send count result
            }
            break;
        }
        case ENVELOPE_OK: {
            OKEnvelope *env = (OKEnvelope *)envelope;
            ok_callback cb = go_hash_map_get_string(r->priv->ok_callbacks, env->event_id);
            if (cb) {
                cb(env->ok, env->reason); // Handle OK callback
            }
            break;
        }
        default:
            break;
        }
        nsync_mu_unlock(&r->priv->mutex); // Unlock after processing the envelope
    }

    free(buf); // Free the buffer when done
    return NULL;
}

bool relay_connect(Relay *relay, Error **err) {
    if (!relay) {
        *err = new_error(1, "relay must be initialized with a call to new_relay()");
        return false;
    }

    Connection *conn = new_connection(relay->url);
    if (!conn) {
        *err = new_error(1, "error opening websocket to '%s'\n", relay->url);
        return false;
    }
    relay->connection = conn;

    // Ticker *ticker = create_ticker(29 * GO_TIME_SECOND);
    // void *cleanup[] = {relay, ticker};
    //  go(cleanup_routine, cleanup);
    
    go(write_operations, relay);
    go(message_loop, relay);

    return true;
}

void *write_error(void *arg) {
    go_channel_send((GoChannel *)arg, new_error(0, "connection closed"));
    return NULL;
}

int nsync_go_context_is_canceled(const void *ctx) {
    return go_context_is_canceled((GoContext *)ctx);
}

GoChannel *relay_write(Relay *r, char *msg) {
    GoChannel *chan = go_channel_create(1);

    // Create a write request
    write_request req = {
        .msg = msg,
        .answer = chan
    };

    nsync_mu_lock(&r->priv->mutex);  // Lock the mutex to ensure thread safety

    // Use a loop to handle the condition (waiting until the message is sent or context is canceled)
    while (true) {
        // Check if the context is canceled (done)
        if (go_context_is_canceled(r->priv->connection_context)) {
            go(write_error, chan);  // If canceled, write the error to the channel
            nsync_mu_unlock(&r->priv->mutex);
            return chan;  // Return immediately after cancellation
        }

        // Add the write request to the write queue
        if (go_channel_send(r->priv->write_queue, &req) == 0) {
            // Successfully added to the write queue, return the channel
            nsync_mu_unlock(&r->priv->mutex);
            return chan;
        }

        // Wait until either the write queue has space or the context is canceled
        nsync_mu_wait_with_deadline(
            &r->priv->mutex, 
            &nsync_go_context_is_canceled, 
            r->priv->connection_context,
            NULL,
            nsync_time_no_deadline,  // No timeout deadline
            NULL  // No specific note for cancellation
        );
    }
}

void relay_publish(Relay *relay, NostrEvent *event) {
    char *event_json = nostr_event_serialize(event);
    if (!event_json)
        return;

    GoChannel *chan = relay_write(relay, event_json);
    free(event_json);
}

void relay_auth(Relay *relay, void (*sign)(NostrEvent *, Error **), Error **err) {

    NostrEvent auth_event = {
        .created_at = time(NULL),
        .kind = KIND_CLIENT_AUTHENTICATION,
        .tags = create_tags(2,
                            create_tag("relay", relay->url),
                            create_tag("challenge", relay->priv->challenge)),
        .content = "",
    };

    Error **sig_err = NULL;
    sign(&auth_event, sig_err);
    if (sig_err && *sig_err) {
        *err = *sig_err;
        return;
    }
    relay_publish(relay, &auth_event);
    free_tags(auth_event.tags);
}

bool relay_subscribe(Relay *relay, GoContext *ctx, Filters *filters, Error **err) {
    // Ensure the relay connection exists
    if (relay->connection == NULL) {
        *err = new_error(1, "not connected to %s", relay->url);
        return false;
    }

    // Prepare the subscription
    Subscription *subscription = relay_prepare_subscription(relay, ctx, filters);
    if (!subscription) {
        *err = new_error(1, "failed to prepare subscription");
        return false;
    }

    // Send the subscription request (Fire the subscription)
    if (!subscription_fire(subscription, err)) {
        return false;
    }

    // Successfully subscribed
    return true;
}

Subscription *relay_prepare_subscription(Relay *relay, GoContext *ctx, Filters *filters) {
    if (!relay || !filters || !ctx) {
        return NULL;
    }

    // Generate a unique subscription ID
    static int64_t subscription_counter = 1;
    int64_t subscription_id = subscription_counter++;

    Subscription *subscription = create_subscription(relay, filters);
    // Initialize the subscription fields
    subscription->priv->counter = subscription_id;
    subscription->priv->match = filters_match; // Function for matching filters with events

    // Store subscription in relay subscriptions map
    go_hash_map_insert_int(relay->subscriptions, subscription->priv->counter, subscription);

    return subscription;
}

GoChannel *relay_query_events(Relay *relay, GoContext *ctx, Filter *filter, Error **err) {
    if (!relay->connection) {
        *err = new_error(1, "not connected to relay");
        return NULL;
    }

    Filters filters = {
        .filters = filter,
    };

    // Prepare the subscription
    Subscription *subscription = relay_prepare_subscription(relay, ctx, &filters);
    if (!subscription) {
        *err = new_error(ERR_RELAY_SUBSCRIBE_FAILED, "failed to prepare subscription");
        return NULL;
    }

    // Fire the subscription (send REQ)
    if (!subscription_fire(subscription, err)) {
        *err = new_error(ERR_RELAY_SUBSCRIBE_FAILED, "couldn't subscribe to filter at relay");
        return NULL;
    }

    // Return the channel where events will be received
    return subscription->events;
}

NostrEvent **relay_query_sync(Relay *relay, GoContext *ctx, Filter *filter, int *event_count, Error **err) {
    if (!relay->connection) {
        *err = new_error(1, "not connected to relay");
        return NULL;
    }

    // Create an array to store the events
    size_t max_events = (filter->limit > 0) ? filter->limit : 250; // Default to 250 if no limit is specified
    NostrEvent **events = (NostrEvent **)malloc(max_events * sizeof(NostrEvent *));
    if (!events) {
        *err = new_error(1, "failed to allocate memory for events");
        return NULL;
    }

    Filters filters = {
        .filters = filter};

    // Prepare the subscription
    Subscription *subscription = relay_prepare_subscription(relay, ctx, &filters);
    if (!subscription) {
        *err = new_error(1, "failed to prepare subscription");
        free(events);
        return NULL;
    }

    // Fire the subscription (send REQ)
    if (!subscription_fire(subscription, err)) {
        free(events);
        return NULL;
    }

    // Wait for events or until the subscription closes
    int received_count = 0;
    GoSelectCase cases[] = {
        {GO_SELECT_RECEIVE, subscription->events, NULL},
        {GO_SELECT_RECEIVE, subscription->end_of_stored_events, NULL},
        {GO_SELECT_RECEIVE, relay->priv->connection_context->done, NULL}};

    while (true) {
        // Select which event happens (receiving an event or end of stored events)
        int result = go_select(cases, 3);
        switch (result) {
        case 0: { // New event received
            if (received_count >= max_events) {
                max_events *= 2; // Expand the events array if needed
                events = (NostrEvent **)realloc(events, max_events * sizeof(NostrEvent *));
                if (!events) {
                    *err = new_error(1, "failed to expand event array");
                    return NULL;
                }
            }

            NostrEvent *event;
            go_channel_receive(subscription->events, (void **)event);
            events[received_count++] = event; // Store the event
            break;
        }
        case 1: {                             // End of stored events (EOSE)
            subscription_unsub(subscription); // Unsubscribe from the relay
            *event_count = received_count;    // Set the event count for the caller
            return events;                    // Return the array of events
        }
        case 2: { // Connection context is canceled (relay is closing)
            *err = new_error(1, "relay connection closed while querying events");
            free(events);
            return NULL;
        }
        default:
            break;
        }
    }
}

int64_t relay_count(Relay *relay, GoContext *ctx, Filter *filter, Error **err) {
    if (!relay->connection) {
        *err = new_error(1, "not connected to relay");
        return 0;
    }

    Filters filters = {
        .filters = filter};

    // Prepare the subscription (but don't fire it yet)
    Subscription *subscription = relay_prepare_subscription(relay, ctx, &filters);
    if (!subscription) {
        *err = new_error(1, "failed to prepare subscription");
        return 0;
    }

    subscription->priv->count_result = go_channel_create(1);

    // Fire the subscription (send REQ)
    int result = subscription_fire(subscription, err);
    if (result != 0) {
        *err = new_error(1, "failed to send subscription request");
        return -1;
    }

    // Wait for count result
    int64_t *count;
    go_channel_receive(subscription->priv->count_result, (void **)count);

    // Return the count result
    return *count;
}

bool relay_close(Relay *r, Error **err) {

    if (!r->connection) {
        *err = new_error(ERR_RELAY_CLOSE_FAILED, "relay not connected");
        return false;
    }

    nsync_mu_lock(&r->priv->mutex);
    connection_close(r->connection);
    r->priv->connection_context_cancel(r->priv->connection_context);
    nsync_mu_unlock(&r->priv->mutex);
    return true;
}
