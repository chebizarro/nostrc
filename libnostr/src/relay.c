#include "relay.h"
#include "envelope.h"
#include "error_codes.h"
#include "json.h"
#include "kinds.h"
#include "relay-private.h"
#include "subscription-private.h"
#include "subscription.h"
#include "utils.h"
#include "go.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

// Forward declaration for worker used before its definition
static void *write_error(void *arg);
static void *write_operations(void *arg);
static void *message_loop(void *arg);
static void relay_debug_emit(Relay *r, const char *s) {
    if (!r || !r->priv || !r->priv->debug_raw || !s) return;
    char *copy = strdup(s);
    if (!copy) return;
    // non-blocking: if channel is full, drop
    (void)go_channel_try_send(r->priv->debug_raw, copy);
}

void relay_enable_debug_raw(Relay *relay, int enable) {
    if (!relay || !relay->priv) return;
    nsync_mu_lock(&relay->priv->mutex);
    if (enable) {
        if (!relay->priv->debug_raw) {
            relay->priv->debug_raw = go_channel_create(128);
        }
    } else {
        if (relay->priv->debug_raw) {
            go_channel_close(relay->priv->debug_raw);
            go_channel_free(relay->priv->debug_raw);
            relay->priv->debug_raw = NULL;
        }
    }
    nsync_mu_unlock(&relay->priv->mutex);
}

GoChannel *relay_get_debug_raw_channel(Relay *relay) {
    if (!relay || !relay->priv) return NULL;
    return relay->priv->debug_raw;
}

bool relay_is_connected(Relay *relay) {
    if (!relay) return false;
    nsync_mu_lock(&relay->priv->mutex);
    bool connected = (relay->connection != NULL);
    nsync_mu_unlock(&relay->priv->mutex);
    return connected;
}

Relay *new_relay(GoContext *context, const char *url, Error **err) {
    if (url == NULL) {
        if (err) *err = new_error(1, "invalid relay URL");
        return NULL;
    }
    CancelContextResult cancellabe = go_context_with_cancel(context);

    Relay *relay = (Relay *)calloc(1, sizeof(Relay));
    RelayPrivate *priv = (RelayPrivate *)calloc(1, sizeof(RelayPrivate));
    if (!relay || !priv) {
        if (err) *err = new_error(1, "failed to allocate memory for Relay struct");
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
    relay->priv->debug_raw = NULL;
    go_wait_group_init(&relay->priv->workers);
    // request_header

    relay->priv->notice_handler = NULL;
    relay->priv->custom_handler = NULL;

    return relay;
}

void free_relay(Relay *relay) {
    if (!relay) return;
    // Signal background loops to stop
    if (relay->priv && relay->priv->connection_context_cancel) {
        relay->priv->connection_context_cancel(relay->priv->connection_context);
    }
    // Close queues to unblock workers
    if (relay->priv) {
        if (relay->priv->write_queue) go_channel_close(relay->priv->write_queue);
        if (relay->priv->subscription_channel_close_queue) go_channel_close(relay->priv->subscription_channel_close_queue);
        if (relay->priv->debug_raw) go_channel_close(relay->priv->debug_raw);
    }
    // Close network connection last
    if (relay->connection) {
        connection_close(relay->connection);
        relay->connection = NULL;
    }
    // Wait for worker goroutines to finish
    if (relay->priv) {
        go_wait_group_wait(&relay->priv->workers);
        go_wait_group_destroy(&relay->priv->workers);
    }
}

bool relay_connect(Relay *relay, Error **err) {
    if (!relay) {
        if (err) *err = new_error(1, "relay must be initialized with a call to new_relay()");
        return false;
    }

    Connection *conn = new_connection(relay->url);
    if (!conn) {
        if (err) *err = new_error(1, "error opening websocket to '%s'\n", relay->url);
        return false;
    }
    relay->connection = conn;

    go_wait_group_add(&relay->priv->workers, 2);
    go(write_operations, relay);
    go(message_loop, relay);

    return true;
}

static void *write_error(void *arg) {
    go_channel_send((GoChannel *)arg, new_error(0, "connection closed"));
    return NULL;
}

// Worker: processes relay->priv->write_queue and writes frames to the connection.
static void *write_operations(void *arg) {
    Relay *r = (Relay *)arg;
    if (!r || !r->priv) return NULL;

    for (;;) {
        write_request *req = NULL;
        GoSelectCase cases[] = {
            (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = r->priv->write_queue, .value = NULL, .recv_buf = (void **)&req },
            (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = r->priv->connection_context->done, .value = NULL, .recv_buf = NULL },
        };
        int idx = go_select(cases, 2);
        if (idx == 1) {
            // context canceled
            break;
        }
        if (idx != 0) {
            // unexpected; continue
            continue;
        }
        if (!req) {
            // queue closed or spurious
            break;
        }

        Error *werr = NULL;
        nsync_mu_lock(&r->priv->mutex);
        Connection *conn = r->connection;
        nsync_mu_unlock(&r->priv->mutex);
        if (!conn) {
            werr = new_error(1, "no connection");
        } else {
            connection_write_message(conn, r->priv->connection_context, req->msg, &werr);
        }
        // The writer owns req->msg copy
        if (req->msg) free(req->msg);
        // Send result back to caller
        go_channel_send(req->answer, werr);
        free(req);
    }

    go_wait_group_done(&((Relay *)arg)->priv->workers);
    return NULL;
}

// Worker: reads messages from the connection, parses envelopes, dispatches,
// and emits concise debug summaries on the optional debug_raw channel.
static void *message_loop(void *arg) {
    Relay *r = (Relay *)arg;
    if (!r || !r->priv) return NULL;

    char buf[4096];
    Error *err = NULL;
    for (;;) {
        memset(buf, 0, sizeof(buf));
        nsync_mu_lock(&r->priv->mutex);
        Connection *conn = r->connection;
        nsync_mu_unlock(&r->priv->mutex);
        if (!conn) break;

        connection_read_message(conn, r->priv->connection_context, buf, sizeof(buf), &err);
        if (err) {
            free_error(err);
            err = NULL;
            break;
        }
        if (buf[0] == '\0') continue;

        Envelope *envelope = parse_message(buf);
        if (!envelope) {
            if (r->priv->custom_handler) r->priv->custom_handler(buf);
            continue;
        }

        switch (envelope->type) {
        case ENVELOPE_NOTICE: {
            NoticeEnvelope *ne = (NoticeEnvelope *)envelope;
            if (r->priv->notice_handler) r->priv->notice_handler(ne->message);
            char tmp[256]; snprintf(tmp, sizeof(tmp), "NOTICE: %s", ne->message ? ne->message : "");
            relay_debug_emit(r, tmp);
            break; }
        case ENVELOPE_EOSE: {
            EOSEEnvelope *env = (EOSEEnvelope *)envelope;
            if (env->message) {
                Subscription *subscription = go_hash_map_get_int(r->subscriptions, sub_id_to_serial(env->message));
                if (subscription) subscription_dispatch_eose(subscription);
            }
            char tmp[128]; snprintf(tmp, sizeof(tmp), "EOSE sid=%s", env->message ? env->message : "");
            relay_debug_emit(r, tmp);
            break; }
        case ENVELOPE_AUTH: {
            r->priv->challenge = ((AuthEnvelope *)envelope)->challenge;
            char tmp[256]; snprintf(tmp, sizeof(tmp), "AUTH challenge=%s", r->priv->challenge ? r->priv->challenge : "");
            relay_debug_emit(r, tmp);
            break; }
        case ENVELOPE_EVENT: {
            EventEnvelope *env = (EventEnvelope *)envelope;
            // Emit summary BEFORE handing event to subscription
            if (env->event) {
                char tmp[256];
                const char *id = env->event->id ? env->event->id : "";
                const char *pk = env->event->pubkey ? env->event->pubkey : "";
                snprintf(tmp, sizeof(tmp), "EVENT kind=%d pubkey=%.8s id=%.8s", env->event->kind, pk, id);
                relay_debug_emit(r, tmp);
            }
            Subscription *subscription = go_hash_map_get_int(r->subscriptions, sub_id_to_serial(env->subscription_id));
            if (subscription && env->event) {
                // Optionally verify signature if available
                if (!r->assume_valid && !event_check_signature(env->event)) {
                    // drop invalid event
                } else {
                    subscription_dispatch_event(subscription, env->event);
                    // ownership passed to subscription; avoid double free
                    env->event = NULL;
                }
            }
            break; }
        case ENVELOPE_CLOSED: {
            ClosedEnvelope *env = (ClosedEnvelope *)envelope;
            Subscription *subscription = go_hash_map_get_int(r->subscriptions, sub_id_to_serial(env->subscription_id));
            if (subscription) subscription_dispatch_closed(subscription, env->reason);
            char tmp[256]; snprintf(tmp, sizeof(tmp), "CLOSED sid=%s reason=%s",
                                   env->subscription_id ? env->subscription_id : "",
                                   env->reason ? env->reason : "");
            relay_debug_emit(r, tmp);
            break; }
        case ENVELOPE_OK: {
            OKEnvelope *oe = (OKEnvelope *)envelope;
            char tmp[256]; snprintf(tmp, sizeof(tmp), "OK id=%s ok=%s reason=%s",
                                   oe->event_id ? oe->event_id : "",
                                   oe->ok ? "true" : "false",
                                   oe->reason ? oe->reason : "");
            relay_debug_emit(r, tmp);
            break; }
        case ENVELOPE_COUNT: {
            CountEnvelope *ce = (CountEnvelope *)envelope;
            Subscription *subscription = go_hash_map_get_int(r->subscriptions, sub_id_to_serial(ce->subscription_id));
            if (subscription && subscription->priv->count_result) {
                int64_t *val = (int64_t *)malloc(sizeof(int64_t));
                if (val) { *val = (int64_t)ce->count; go_channel_send(subscription->priv->count_result, val); }
            }
            char tmp[64]; snprintf(tmp, sizeof(tmp), "COUNT=%d", ce->count);
            relay_debug_emit(r, tmp);
            break; }
        default:
            break;
        }

        free_envelope(envelope);
    }

    go_wait_group_done(&((Relay *)arg)->priv->workers);
    return NULL;
}

int nsync_go_context_is_canceled(const void *ctx) {
    return go_context_is_canceled((GoContext *)ctx);
}

GoChannel *relay_write(Relay *r, char *msg) {
    if (!r) return NULL;
    GoChannel *chan = go_channel_create(1);

    // Copy message so caller can free its own buffer safely
    char *msg_copy = strdup(msg ? msg : "");
    write_request *req = (write_request *)malloc(sizeof(write_request));
    if (!req || !msg_copy) {
        if (req) free(req);
        if (msg_copy) free(msg_copy);
        go(write_error, chan);
        return chan;
    }
    req->msg = msg_copy;
    req->answer = chan;

    // Enqueue request (non-blocking); if fails and context canceled, return error
    if (go_channel_send(r->priv->write_queue, req) != 0) {
        // Fallback: if cannot enqueue, surface error
        go(write_error, chan);
        free(req->msg);
        free(req);
        return chan;
    }
    return chan;
}

void relay_publish(Relay *relay, NostrEvent *event) {
    char *event_json = nostr_event_serialize(event);
    if (!event_json)
        return;

    (void)relay_write(relay, event_json);
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

    Error *sig_err = NULL;
    sign(&auth_event, &sig_err);
    if (sig_err) {
        if (err) *err = sig_err;
        return;
    }
    relay_publish(relay, &auth_event);
    free_tags(auth_event.tags);
}

bool relay_subscribe(Relay *relay, GoContext *ctx, Filters *filters, Error **err) {
    // Ensure the relay connection exists
    if (!relay->connection) {
        if (err) *err = new_error(1, "not connected to %s", relay->url);
        return false;
    }

    // Prepare the subscription
    Subscription *subscription = relay_prepare_subscription(relay, ctx, filters);
    if (!subscription) {
        if (err && *err == NULL) *err = new_error(1, "failed to prepare subscription");
        return false;
    }

    // Send the subscription request (Fire the subscription)
    if (!subscription_fire(subscription, err)) {
        if (err && *err == NULL) *err = new_error(ERR_RELAY_SUBSCRIBE_FAILED, "couldn't subscribe to filter at relay");
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
    if (!subscription) return NULL;
    // Initialize the subscription fields
    subscription->priv->counter = subscription_id;
    // Generate id string from counter
    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%lld", (long long)subscription_id);
    subscription->priv->id = strdup(idbuf);
    // Context with cancel for this subscription
    CancelContextResult subctx = go_context_with_cancel(ctx);
    subscription->context = subctx.context;
    subscription->priv->cancel = subctx.cancel;
    subscription->priv->match = filters_match; // Function for matching filters with events
    // Start lifecycle watcher
    go(subscription_start, subscription);

    // Store subscription in relay subscriptions map
    go_hash_map_insert_int(relay->subscriptions, subscription->priv->counter, subscription);

    return subscription;
}

GoChannel *relay_query_events(Relay *relay, GoContext *ctx, Filter *filter, Error **err) {
    if (!relay->connection) {
        if (err) *err = new_error(1, "not connected to relay");
        return NULL;
    }

    Filters filters = {
        .filters = filter,
    };

    // Prepare the subscription
    Subscription *subscription = relay_prepare_subscription(relay, ctx, &filters);
    if (!subscription) {
        if (err) *err = new_error(ERR_RELAY_SUBSCRIBE_FAILED, "failed to prepare subscription");
        return NULL;
    }

    // Fire the subscription (send REQ)
    if (!subscription_fire(subscription, err)) {
        if (err) *err = new_error(ERR_RELAY_SUBSCRIBE_FAILED, "couldn't subscribe to filter at relay");
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

    // Wait for events or until the subscription closes or timeout
    size_t received_count = 0;
    // Create a timeout ticker (e.g., 3s) to bound waits
    Ticker *timeout = create_ticker(3000);
    GoSelectCase cases[] = {
        (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = subscription->events, .value = NULL, .recv_buf = NULL },
        (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = subscription->end_of_stored_events, .value = NULL, .recv_buf = NULL },
        (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = relay->priv->connection_context->done, .value = NULL, .recv_buf = NULL },
        (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = timeout->c, .value = NULL, .recv_buf = NULL },
    };

    while (true) {
        // Select which event happens (receiving an event or end of stored events)
        int result = go_select(cases, 4);
        switch (result) {
        case 0: { // New event received
            if (received_count >= max_events) {
                max_events *= 2; // Expand the events array if needed
                events = (NostrEvent **)realloc(events, max_events * sizeof(NostrEvent *));
                if (!events) {
                    if (err) *err = new_error(1, "failed to expand event array");
                    return NULL;
                }
            }

            NostrEvent *event = NULL;
            go_channel_receive(subscription->events, (void **)&event);
            events[received_count++] = event; // Store the event
            break;
        }
        case 1: {                             // End of stored events (EOSE)
            subscription_unsub(subscription); // Unsubscribe from the relay
            *event_count = (int)received_count;    // Set the event count for the caller
            stop_ticker(timeout);
            return events;                    // Return the array of events
        }
        case 2: { // Connection context is canceled (relay is closing)
            if (err) *err = new_error(1, "relay connection closed while querying events");
            stop_ticker(timeout);
            free(events);
            return NULL;
        }
        case 3: { // Timeout waiting for events
            if (err) *err = new_error(1, "relay_query_sync timed out");
            subscription_unsub(subscription);
            *event_count = (int)received_count;
            stop_ticker(timeout);
            return events;
        }
        default:
            break;
        }
    }
}

int64_t relay_count(Relay *relay, GoContext *ctx, Filter *filter, Error **err) {
    if (!relay->connection) {
        if (err) *err = new_error(1, "not connected to relay");
        return 0;
    }

    Filters filters = {
        .filters = filter};

    // Prepare the subscription (but don't fire it yet)
    Subscription *subscription = relay_prepare_subscription(relay, ctx, &filters);
    if (!subscription) {
        if (err) *err = new_error(1, "failed to prepare subscription");
        return 0;
    }

    subscription->priv->count_result = go_channel_create(1);

    // Fire the subscription (send REQ)
    int fire_result = subscription_fire(subscription, err);
    if (fire_result != 0) {
        if (err) *err = new_error(1, "failed to send subscription request");
        return -1;
    }

    // Wait for count result
    int64_t *count = NULL;
    if (go_channel_receive(subscription->priv->count_result, (void **)&count) != 0 || !count) {
        if (err) *err = new_error(1, "failed to receive count result");
        return -1;
    }
    int64_t result = *count;
    free(count);
    return result;
}

bool relay_close(Relay *r, Error **err) {
    if (!r) {
        if (err) *err = new_error(ERR_RELAY_CLOSE_FAILED, "invalid relay");
        return false;
    }

    // Snapshot connection under lock and cancel context to wake workers
    nsync_mu_lock(&r->priv->mutex);
    Connection *conn = r->connection;
    r->priv->connection_context_cancel(r->priv->connection_context);
    r->connection = NULL;
    nsync_mu_unlock(&r->priv->mutex);

    if (!conn) {
        if (err) *err = new_error(ERR_RELAY_CLOSE_FAILED, "relay not connected");
        return false;
    }

    // Close the network connection outside the relay mutex
    connection_close(conn);
    return true;
}

void relay_disconnect(Relay *relay) {
    if (!relay) return;
    Error *err = NULL;
    (void)relay_close(relay, &err);
    if (err) free_error(err);
}

void relay_unsubscribe(Relay *relay, const char *subscription_id) {
    if (!relay) return;
    nsync_mu_lock(&relay->priv->mutex);
    Subscription *sub = go_hash_map_get_int(relay->subscriptions, sub_id_to_serial(subscription_id));
    if (sub) {
        subscription_unsub(sub);
        go_hash_map_remove_int(relay->subscriptions, sub_id_to_serial(subscription_id));
    }
    nsync_mu_unlock(&relay->priv->mutex);
}
