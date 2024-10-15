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

    Relay *relay = (Relay *)malloc(sizeof(Relay));
    RelayPrivate *priv = (RelayPrivate *)malloc(sizeof(RelayPrivate));
    if (!relay || !priv) {
        *err = new_error(1, "failed to allocate memory for Relay struct");
        return NULL;
    }

    relay->url = strdup(url);
    relay->subscriptions = concurrent_hash_map_create(16);
    relay->assume_valid = false;

    relay->priv = priv;
    //nsync_mu_init(relay->priv->close_mutex);
    relay->priv->connection_context = cancellabe.context;
    relay->priv->connection_context_cancel = cancellabe.cancel;
    relay->priv->ok_callbacks = concurrent_hash_map_create(16);
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
        concurrent_hash_map_destroy(relay->subscriptions);
        go_context_free(relay->priv->connection_context);
        concurrent_hash_map_destroy(relay->priv->ok_callbacks);
        go_channel_free(relay->priv->write_queue);
        go_channel_free(relay->priv->subscription_channel_close_queue);
        free(relay->priv);
        free(relay);
    }
}

bool relay_is_connected(Relay *relay) {
    return !go_context_is_canceled(relay->priv->connection_context);
}

bool sub_foreach_unsub(HashKey *key, void *sub) {
    subscription_unsub((Subscription *)sub);
    return true;
}

void *cleanup_routine(void *arg) {
    Relay *r = (Relay *)arg;
    Ticker *t = (Ticker *)++arg;
    // Wait for connection context to be done
    while (!go_context_is_canceled(r->priv->connection_context)) {
        sleep(1);
    }
    stop_ticker(t);
    connection_close(r->connection);
    r->connection = NULL;
    concurrent_hash_map_for_each(r->subscriptions, sub_foreach_unsub);
    return NULL;
}

void *write_operations(void *arg) {
    Relay *r = ((Relay **)arg)[0];
    Ticker *t = ((Ticker **)arg)[1];

    write_request write_req = {
        .answer = go_channel_create(1),
        .msg = (char*)malloc(4 * sizeof(char))
    };

    GoSelectCase cases[] = {
        {GO_SELECT_RECEIVE, t->c, NULL},
        {GO_SELECT_RECEIVE, r->priv->write_queue, &write_req},
        {GO_SELECT_RECEIVE, r->priv->connection_context->done, NULL}};

    while (true) {
        int result = go_select(cases, 3);
        switch (result) {
        case 0:
            // connection_ping(r->connection);
            break;
        case 1: {
            Error **err = NULL;
            connection_write_message(r->connection, r->priv->connection_context, write_req.msg, err);
            if (err) {
                go_channel_send(write_req.answer, err);
            }
            go_channel_free(write_req.answer);
            break;
        }
        case 2:
            if (go_context_is_canceled(r->priv->connection_context))
                return NULL;
        default:
            break;
        }
    }
    return NULL;
}

void *message_loop(void *arg) {
    Relay *r = (Relay *)arg;
    char **buf;
    Error **err;

    while (true) {
        buf = NULL;
        connection_read_message(r->connection, r->priv->connection_context, *buf, err);
        if (err) {
            r->connection_error = err;
            Error **cerr;
            relay_close(r, cerr);

            break;
        }
        const char *message = *buf;
        Envelope *envelope = parse_message(message);
        if (!envelope) {
            if (r->priv->custom_handler) {
                r->priv->custom_handler(message);
            }
            continue;
        }
        switch (envelope->type) {
        case ENVELOPE_NOTICE: {
            if (r->priv->notice_handler) {
                r->priv->notice_handler((char *)envelope);
            }
            break;
        }
        case ENVELOPE_AUTH: {
            r->priv->challenge = ((AuthEnvelope *)envelope)->challenge;
            break;
        }
        case ENVELOPE_EVENT: {
            EventEnvelope *env = (EventEnvelope *)envelope;
            Subscription *subscription = concurrent_hash_map_get_int(r->subscriptions, sub_id_to_serial(env->subscription_id));
            if (subscription && event_check_signature(env->event)) {
                subscription_dispatch_event(subscription, env->event);
            }
            break;
        }
        case ENVELOPE_EOSE: {
            EOSEEnvelope *env = (EOSEEnvelope *)envelope;
            /*
            Subscription *subscription = concurrent_hash_map_get(r->subscriptions, sub_id_to_serial(env->message));
            if (subscription) {
                subscription_dispatch_eose(subscription);
            }*/
            break;
        }
        case ENVELOPE_CLOSED: {
            ClosedEnvelope *env = (ClosedEnvelope *)envelope;
            Subscription *subscription = concurrent_hash_map_get_int(r->subscriptions, sub_id_to_serial(env->subscription_id));
            if (subscription) {
                subscription_dispatch_closed(subscription, env->reason);
            }
            break;
        }
        case ENVELOPE_COUNT: {
            CountEnvelope *env = (CountEnvelope *)envelope;
            Subscription *subscription = concurrent_hash_map_get_int(r->subscriptions, sub_id_to_serial(env->subscription_id));
            if (subscription) {
                go_channel_send(subscription->priv->count_result, &env->count);
            }
            break;
        }
        case ENVELOPE_OK: {
            OKEnvelope *env = (OKEnvelope *)envelope;
            ok_callback cb = concurrent_hash_map_get_string(r->priv->ok_callbacks, env->event_id);
            if (cb) {
                cb(env->ok, env->reason);
            }
            break;
        default:
            break;
        }
        }
    }
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

    Ticker *ticker = create_ticker(29 * GO_TIME_SECOND);

    void *cleanup[] = {relay, ticker};
    go(cleanup_routine, cleanup);
    go(write_operations, cleanup[0]);
    go(message_loop, relay);

    return true;
}

void *write_error(void *arg) {
    go_channel_send((GoChannel *)arg, new_error(0, "connection closed"));
    return NULL;
}

GoChannel *relay_write(Relay *r, char *msg) {
    GoChannel *chan = go_channel_create(1);

    write_request req = {
        .msg = msg,
        .answer = chan};

    GoSelectCase cases[] = {
        {GO_SELECT_SEND, r->priv->write_queue, &req},
        {GO_SELECT_RECEIVE, r->priv->connection_context->done, NULL}};

    while (true) {
        switch (go_select(cases, 2)) {
        case 0:
            break;
        case 1:
            go(write_error, chan);
        default:
            break;
        }
    }
    return chan;
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
    concurrent_hash_map_insert_int(relay->subscriptions, subscription->priv->counter, subscription);

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

    connection_close(r->connection);
    r->connection = NULL;
    r->priv->connection_context_cancel(r->priv->connection_context);
    return true;
}
