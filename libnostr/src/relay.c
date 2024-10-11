#include "relay.h"
#include "envelope.h"
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

    Relay *relay = (Relay *)malloc(sizeof(Relay));
    RelayPrivate *priv = (RelayPrivate *)malloc(sizeof(RelayPrivate));
    if (!relay || !priv)
        *err = new_error(1, "failed to allocate memory for Relay struct");
    return NULL;

    relay->url = strdup(url);
    relay->subscriptions = concurrent_hash_map_create(16);
    relay->assume_valid = false;

    relay->priv = priv;
    nsync_mu_init(relay->priv->close_mutex);
    relay->priv->connection_context = context;
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
    return 0;
}

bool sub_foreach_unsub(const char *key, void *sub) {
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

    write_request *write_req;

    GoSelectCase cases[] = {
        {GO_SELECT_RECEIVE, t->c, NULL},
        {GO_SELECT_RECEIVE, r->priv->write_queue, write_req},
        {GO_SELECT_RECEIVE, r->priv->connection_context->done, NULL}};

    while (true) {
        int result = go_select(cases, 3);
        switch (result) {
        case 0:
            // ping!
            break;
        case 1: {
            Error **err = NULL;
            connection_write_message(r->connection, r->priv->connection_context, write_req->msg, err);
            if (err)
                go_channel_send(write_req->answer, err);
            go_channel_free(write_req->answer);
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
            relay_close(r);
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
            } else {
                // log it
            }
            break;
        }
        case ENVELOPE_AUTH: {
            AuthEnvelope *env = (AuthEnvelope *)envelope;
            if (!env->challenge)
                continue;
            r->priv->challenge = env->challenge;
            break;
        }
        case ENVELOPE_EVENT: {
            EventEnvelope *env = (EventEnvelope *)envelope;
            if (!env->subscription_id) {
                continue;
            }
            // implement subIdToSerial
            Subscription *subscription = concurrent_hash_map_get(r->subscriptions, env->subscription_id);
            if (!subscription) {
                continue;
            } else {
                if (!subscription->priv->match(env->event)) {
                    continue;
                }
                if (!r->assume_valid) {
                    if (!event_check_signature(env->event)) {
                        continue;
                    }
                }
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
            Subscription *subscription = concurrent_hash_map_get(r->subscriptions, env->subscription_id);
            if (subscription) {
                subscription_dispatch_closed(subscription, env->reason);
            }
            break;
        }
        case ENVELOPE_COUNT: {
            CountEnvelope *env = (CountEnvelope *)envelope;
            Subscription *subscription = concurrent_hash_map_get(r->subscriptions, env->subscription_id);
            if (subscription) {
                go_channel_send(subscription->priv->count_result, &env->count);
            }
            break;
        }
        case ENVELOPE_OK: {
            OKEnvelope *env = (OKEnvelope *)envelope;
            ok_callback cb = concurrent_hash_map_get(r->priv->ok_callbacks, env->event_id);
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

int relay_connect(Relay *relay, Error **err) {
    if (relay == NULL) {
        *err = new_error(1, "relay must be initialized with a call to new_relay()");
        return -1;
    }

    Connection *conn = new_connection(relay->url);
    if (conn == NULL) {
        *err = new_error(1, "error opening websocket to '%s'\n", relay->url);
        return -1;
    }
    relay->connection = conn;

    Ticker *ticker = create_ticker(29 * GO_TIME_SECOND);

    void *cleanup[] = {relay, ticker};
    go(cleanup_routine, cleanup);
    go(write_operations, cleanup[0]);
    go(message_loop, relay);

    return 0;
}

void relay_write(char *msg) {
}

void relay_publish(Relay *relay, NostrEvent *event) {
}

void relay_auth(Relay *relay, void (*sign)(NostrEvent *)) {

    NostrEvent auth_event = {
        .created_at = time(NULL),
        .kind = KIND_CLIENT_AUTHENTICATION,
        .tags = create_tags(2,
                            create_tag("relay", relay->url),
                            create_tag("challenge", relay->priv->challenge)),
        .content = "",
    };

    relay_publish(relay, &auth_event);
    free_tags(auth_event.tags);
}

int relay_subscribe(Relay *relay, Filters *filters) {

    return 0;
}

int relay_prepare_subscription(Relay *relay, Filters *filters) {

    return 0;
}

int relay_query_events(Relay *relay, Filter *filter) {
    return 0;
}

int relay_query_sync(Relay *relay, Filter *filter) {
    return 0;
}

int relay_count(Relay *relay, Filter *filter) {
    return 0;
}

int relay_close(Relay *relay) {
    return 0;
}

void relay_unsubscribe(Relay *relay, int subscription_id) {
}

void relay_disconnect(Relay *relay) {
}
