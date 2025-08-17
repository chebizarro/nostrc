#include "nostr-relay.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-filter.h"
#include "nostr-envelope.h"
#include "error_codes.h"
#include "json.h"
#include "nostr-kinds.h"
#include "relay-private.h"
#include "subscription-private.h"
#include "nostr-subscription.h"
#include "nostr-utils.h"
#include "nostr/metrics.h"
#include "go.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static int shutdown_dbg_enabled(void) {
    static int inited = 0;
    static int enabled = 0;
    if (!inited) {
        const char *e = getenv("NOSTR_DEBUG_SHUTDOWN");
        enabled = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
        inited = 1;
    }
    return enabled;
}

// Forward declaration for worker used before its definition
static void *write_error(void *arg);
static void *write_operations(void *arg);
static void *message_loop(void *arg);
static void relay_debug_emit(NostrRelay *r, const char *s) {
    if (!r || !r->priv || !r->priv->debug_raw || !s) return;
    char *copy = strdup(s);
    if (!copy) return;
    // non-blocking: if channel is full, drop
    (void)go_channel_try_send(r->priv->debug_raw, copy);
}

void nostr_relay_enable_debug_raw(NostrRelay *relay, int enable) {
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

GoChannel *nostr_relay_get_debug_raw_channel(NostrRelay *relay) {
    if (!relay || !relay->priv) return NULL;
    return relay->priv->debug_raw;
}

bool nostr_relay_is_connected(NostrRelay *relay) {
    if (!relay) return false;
    nsync_mu_lock(&relay->priv->mutex);
    bool connected = (relay->connection != NULL);
    nsync_mu_unlock(&relay->priv->mutex);
    return connected;
}

/* GLib-style accessors (header: nostr-relay.h) */
const char *nostr_relay_get_url_const(const NostrRelay *relay) {
    if (!relay) return NULL;
    return relay->url;
}

GoContext *nostr_relay_get_context(const NostrRelay *relay) {
    if (!relay || !relay->priv) return NULL;
    return relay->priv->connection_context;
}

GoChannel *nostr_relay_get_write_channel(const NostrRelay *relay) {
    if (!relay || !relay->priv) return NULL;
    return relay->priv->write_queue;
}

NostrRelay *nostr_relay_new(GoContext *context, const char *url, Error **err) {
    if (url == NULL) {
        if (err) *err = new_error(1, "invalid relay URL");
        return NULL;
    }
    CancelContextResult cancellabe = go_context_with_cancel(context);

    NostrRelay *relay = (NostrRelay *)calloc(1, sizeof(NostrRelay));
    NostrRelayPrivate *priv = (NostrRelayPrivate *)calloc(1, sizeof(NostrRelayPrivate));
    if (!relay || !priv) {
        if (err) *err = new_error(1, "failed to allocate memory for NostrRelay struct");
        return NULL;
    }

    relay->url = strdup(url);
    relay->subscriptions = go_hash_map_create(16);
    relay->assume_valid = false;
    relay->refcount = 1;

    relay->priv = priv;
    nsync_mu_init(&relay->priv->mutex);
    relay->priv->connection_context = cancellabe.context;
    relay->priv->connection_context_cancel = cancellabe.cancel;
    relay->priv->ok_callbacks = go_hash_map_create(16);
    relay->priv->write_queue = go_channel_create(16);
    relay->priv->subscription_channel_close_queue = go_channel_create(16);
    relay->priv->debug_raw = NULL;
    go_wait_group_init(&relay->priv->workers);
    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] nostr_relay_new: initialized workers and queues for %s\n", relay->url);
    // request_header

    relay->priv->notice_handler = NULL;
    relay->priv->custom_handler = NULL;

    return (NostrRelay *)relay;
}

NostrRelay *nostr_relay_ref(NostrRelay *relay) {
    if (!relay) return NULL;
    nsync_mu_lock(&relay->priv->mutex);
    relay->refcount++;
    nsync_mu_unlock(&relay->priv->mutex);
    return relay;
}

static void relay_free_impl(NostrRelay *relay) {
    if (!relay) return;
    // Signal background loops to stop
    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] nostr_relay_free: cancel connection context\n");
    if (relay->priv && relay->priv->connection_context_cancel) {
        relay->priv->connection_context_cancel(relay->priv->connection_context);
    }
    // Close queues to unblock workers
    if (relay->priv) {
        if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] nostr_relay_free: closing queues\n");
        if (relay->priv->write_queue) go_channel_close(relay->priv->write_queue);
        if (relay->priv->subscription_channel_close_queue) go_channel_close(relay->priv->subscription_channel_close_queue);
        if (relay->priv->debug_raw) go_channel_close(relay->priv->debug_raw);
    }
    // Close network connection last
    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] nostr_relay_free: closing network connection\n");
    if (relay->connection) {
        nostr_connection_close(relay->connection);
        relay->connection = NULL;
    }
    // Wait for worker goroutines to finish
    if (relay->priv) {
        if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] nostr_relay_free: waiting for workers\n");
        go_wait_group_wait(&relay->priv->workers);
        if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] nostr_relay_free: workers joined\n");
        go_wait_group_destroy(&relay->priv->workers);
    }

    // Free resources
    if (relay->priv) {
        if (relay->priv->write_queue) { go_channel_free(relay->priv->write_queue); relay->priv->write_queue = NULL; }
        if (relay->priv->subscription_channel_close_queue) { go_channel_free(relay->priv->subscription_channel_close_queue); relay->priv->subscription_channel_close_queue = NULL; }
        if (relay->priv->debug_raw) { go_channel_free(relay->priv->debug_raw); relay->priv->debug_raw = NULL; }
        if (relay->priv->ok_callbacks) { go_hash_map_destroy(relay->priv->ok_callbacks); relay->priv->ok_callbacks = NULL; }
        if (relay->priv->challenge) { free(relay->priv->challenge); relay->priv->challenge = NULL; }
    }
    if (relay->subscriptions) { go_hash_map_destroy(relay->subscriptions); relay->subscriptions = NULL; }
    if (relay->url) { free(relay->url); relay->url = NULL; }
    if (relay->priv) { free(relay->priv); relay->priv = NULL; }
    free(relay);
}

void nostr_relay_unref(NostrRelay *relay) {
    if (!relay) return;
    int do_free = 0;
    nsync_mu_lock(&relay->priv->mutex);
    if (relay->refcount > 0) {
        relay->refcount--;
        if (relay->refcount == 0) do_free = 1;
    }
    nsync_mu_unlock(&relay->priv->mutex);
    if (do_free) relay_free_impl(relay);
}

void nostr_relay_free(NostrRelay *relay) {
    if (!relay) return;
    nostr_relay_unref(relay);
}

bool nostr_relay_connect(NostrRelay *relay, Error **err) {
    if (!relay) {
        if (err) *err = new_error(1, "relay must be initialized with a call to nostr_relay_new()");
        return false;
    }

    NostrConnection *conn = nostr_connection_new(relay->url);
    if (!conn) {
        if (err) *err = new_error(1, "error opening websocket to '%s'\n", relay->url);
        return false;
    }
    relay->connection = conn;

    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] relay_connect: starting workers\n");
    go_wait_group_add(&relay->priv->workers, 2);
    go(write_operations, relay);
    go(message_loop, relay);

    return true;
}

static void *write_error(void *arg) {
    GoChannel *chan = (GoChannel *)arg;
    go_channel_send(chan, new_error(0, "connection closed"));
    return NULL;
}

// Worker: processes relay->priv->write_queue and writes frames to the connection.
static void *write_operations(void *arg) {
    NostrRelay *r = (NostrRelay *)arg;
    if (!r || !r->priv) return NULL;
    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] write_operations: start\n");

    for (;;) {
        // Fast-path: if connection context is canceled, exit promptly
        if (r->priv->connection_context && go_context_is_canceled(r->priv->connection_context)) {
            break;
        }
        NostrRelayWriteRequest *req = NULL;
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
        NostrConnection *conn = r->connection;
        nsync_mu_unlock(&r->priv->mutex);
        if (!conn) {
            werr = new_error(1, "no connection");
        } else {
            // Metrics: time WS write and count bytes
            nostr_metric_timer t = {0};
            nostr_metric_timer_start(&t);
            nostr_connection_write_message(conn, r->priv->connection_context, req->msg, &werr);
            static nostr_metric_histogram *h_ws_write_ns;
            if (!h_ws_write_ns) h_ws_write_ns = nostr_metric_histogram_get("ws_write_ns");
            nostr_metric_timer_stop(&t, h_ws_write_ns);
            if (req->msg) {
                nostr_metric_counter_add("ws_tx_bytes", (uint64_t)strlen(req->msg));
                nostr_metric_counter_add("ws_tx_messages", 1);
            }
        }
        // The writer owns req->msg copy
        if (req->msg) free(req->msg);
        // Send result back to caller
        if (werr) {
            go_channel_send(req->answer, werr);
        } else {
            go_channel_send(req->answer, NULL);
        }
        free(req);
    }

    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] write_operations: exit\n");
    go_wait_group_done(&((NostrRelay *)arg)->priv->workers);
    return NULL;
}

// Worker: reads messages from the connection, parses envelopes, dispatches,
// and emits concise debug summaries on the optional debug_raw channel.
static void *message_loop(void *arg) {
    NostrRelay *r = (NostrRelay *)arg;
    if (!r || !r->priv) return NULL;
    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] message_loop: start\n");

    char buf[4096];
    Error *err = NULL;
    for (;;) {
        memset(buf, 0, sizeof(buf));
        nsync_mu_lock(&r->priv->mutex);
        NostrConnection *conn = r->connection;
        nsync_mu_unlock(&r->priv->mutex);
        if (!conn) break;

        nostr_metric_timer t_read = {0};
        nostr_metric_timer_start(&t_read);
        nostr_connection_read_message(conn, r->priv->connection_context, buf, sizeof(buf), &err);
        static nostr_metric_histogram *h_ws_read_ns;
        if (!h_ws_read_ns) h_ws_read_ns = nostr_metric_histogram_get("ws_read_ns");
        nostr_metric_timer_stop(&t_read, h_ws_read_ns);
        if (err) {
            free_error(err);
            err = NULL;
            break;
        }
        if (buf[0] == '\0') continue;

        size_t blen_for_metrics = strlen(buf);
        nostr_metric_counter_add("ws_rx_bytes", (uint64_t)blen_for_metrics);
        nostr_metric_counter_add("ws_rx_messages", 1);

        // Optional inbound raw debug: show what we received before parsing
        const char *dbg_in_env = getenv("NOSTR_DEBUG_INCOMING");
        if (dbg_in_env && *dbg_in_env && strcmp(dbg_in_env, "0") != 0) {
            // Limit to a sane number of bytes to avoid flooding
            size_t blen = strlen(buf);
            size_t show = blen < 512 ? blen : 512;
            fprintf(stderr, "[incoming] %.*s%s\n",
                    (int)show, buf,
                    (blen > show ? "..." : ""));
        }

        nostr_metric_timer t_parse = {0};
        nostr_metric_timer_start(&t_parse);
        NostrEnvelope *envelope = nostr_envelope_parse(buf);
        static nostr_metric_histogram *h_envelope_parse_ns;
        if (!h_envelope_parse_ns) h_envelope_parse_ns = nostr_metric_histogram_get("envelope_parse_ns");
        nostr_metric_timer_stop(&t_parse, h_envelope_parse_ns);
        if (!envelope) {
            if (dbg_in_env && *dbg_in_env && strcmp(dbg_in_env, "0") != 0) {
                size_t blen = strlen(buf);
                size_t show = blen < 256 ? blen : 256;
                fprintf(stderr, "[incoming][unparsed] %.*s%s\n",
                        (int)show, buf,
                        (blen > show ? "..." : ""));
            }
            if (r->priv->custom_handler) r->priv->custom_handler(buf);
            continue;
        }

        switch (envelope->type) {
        case NOSTR_ENVELOPE_NOTICE: {
            NostrNoticeEnvelope *ne = (NostrNoticeEnvelope *)envelope;
            if (r->priv->notice_handler) r->priv->notice_handler(ne->message);
            char tmp[256]; snprintf(tmp, sizeof(tmp), "NOTICE: %s", ne->message ? ne->message : "");
            relay_debug_emit(r, tmp);
            break; }
        case NOSTR_ENVELOPE_EOSE: {
            NostrEOSEEnvelope *env = (NostrEOSEEnvelope *)envelope;
            if (env->message) {
                NostrSubscription *subscription = go_hash_map_get_int(r->subscriptions, nostr_sub_id_to_serial(env->message));
                if (subscription) nostr_subscription_dispatch_eose(subscription);
            }
            char tmp[128]; snprintf(tmp, sizeof(tmp), "EOSE sid=%s", env->message ? env->message : "");
            relay_debug_emit(r, tmp);
            break; }
        case NOSTR_ENVELOPE_AUTH: {
            r->priv->challenge = ((NostrAuthEnvelope *)envelope)->challenge;
            char tmp[256]; snprintf(tmp, sizeof(tmp), "AUTH challenge=%s", r->priv->challenge ? r->priv->challenge : "");
            relay_debug_emit(r, tmp);
            break; }
        case NOSTR_ENVELOPE_EVENT: {
            NostrEventEnvelope *env = (NostrEventEnvelope *)envelope;
            // Emit summary BEFORE handing event to subscription
            if (env->event) {
                char tmp[256];
                const char *id = env->event->id ? env->event->id : "";
                const char *pk = env->event->pubkey ? env->event->pubkey : "";
                snprintf(tmp, sizeof(tmp), "EVENT kind=%d pubkey=%.8s id=%.8s", env->event->kind, pk, id);
                relay_debug_emit(r, tmp);
            }
            NostrSubscription *subscription = go_hash_map_get_int(r->subscriptions, nostr_sub_id_to_serial(env->subscription_id));
            if (subscription && env->event) {
                // Optionally verify signature if available
                bool verified = true;
                if (!r->assume_valid) {
                    nostr_metric_timer t_verify = {0};
                    nostr_metric_timer_start(&t_verify);
                    verified = nostr_event_check_signature(env->event);
                    static nostr_metric_histogram *h_event_verify_ns;
                    if (!h_event_verify_ns) h_event_verify_ns = nostr_metric_histogram_get("event_verify_ns");
                    nostr_metric_timer_stop(&t_verify, h_event_verify_ns);
                    nostr_metric_counter_add("event_verify_count", 1);
                    if (verified) nostr_metric_counter_add("event_verify_ok", 1);
                    else nostr_metric_counter_add("event_verify_fail", 1);
                }
                if (!verified) {
                    // drop invalid event
                    const char *dbg_in_env = getenv("NOSTR_DEBUG_INCOMING");
                    if (dbg_in_env && *dbg_in_env && strcmp(dbg_in_env, "0") != 0) {
                        char tmp[256];
                        const char *id = env->event->id ? env->event->id : "";
                        snprintf(tmp, sizeof(tmp), "DROP invalid signature id=%.8s", id);
                        relay_debug_emit(r, tmp);
                    }
                } else {
                    nostr_metric_timer t_dispatch = {0};
                    nostr_metric_timer_start(&t_dispatch);
                    nostr_subscription_dispatch_event(subscription, env->event);
                    static nostr_metric_histogram *h_event_dispatch_ns;
                    if (!h_event_dispatch_ns) h_event_dispatch_ns = nostr_metric_histogram_get("event_dispatch_ns");
                    nostr_metric_timer_stop(&t_dispatch, h_event_dispatch_ns);
                    nostr_metric_counter_add("event_dispatch_count", 1);
                    // ownership passed to subscription; avoid double free
                    env->event = NULL;
                }
            }
            break; }
        case NOSTR_ENVELOPE_CLOSED: {
            NostrClosedEnvelope *env = (NostrClosedEnvelope *)envelope;
            NostrSubscription *subscription = go_hash_map_get_int(r->subscriptions, nostr_sub_id_to_serial(env->subscription_id));
            if (subscription) nostr_subscription_dispatch_closed(subscription, env->reason);
            char tmp[256]; snprintf(tmp, sizeof(tmp), "CLOSED sid=%s reason=%s",
                                   env->subscription_id ? env->subscription_id : "",
                                   env->reason ? env->reason : "");
            relay_debug_emit(r, tmp);
            break; }
        case NOSTR_ENVELOPE_OK: {
            NostrOKEnvelope *oe = (NostrOKEnvelope *)envelope;
            char tmp[256]; snprintf(tmp, sizeof(tmp), "OK id=%s ok=%s reason=%s",
                                   oe->event_id ? oe->event_id : "",
                                   oe->ok ? "true" : "false",
                                   oe->reason ? oe->reason : "");
            relay_debug_emit(r, tmp);
            break; }
        case NOSTR_ENVELOPE_COUNT: {
            NostrCountEnvelope *ce = (NostrCountEnvelope *)envelope;
            NostrSubscription *subscription = go_hash_map_get_int(r->subscriptions, nostr_sub_id_to_serial(ce->subscription_id));
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

        nostr_envelope_free(envelope);
    }

    if (shutdown_dbg_enabled()) fprintf(stderr, "[shutdown] message_loop: exit\n");
    go_wait_group_done(&((NostrRelay *)arg)->priv->workers);
    return NULL;
}

int nsync_go_context_is_canceled(const void *ctx) {
    return go_context_is_canceled((GoContext *)ctx);
}

GoChannel *nostr_relay_write(NostrRelay *r, char *msg) {
    if (!r) return NULL;
    GoChannel *chan = go_channel_create(1);

    // Copy message so caller can free its own buffer safely
    char *msg_copy = strdup(msg ? msg : "");
    NostrRelayWriteRequest *req = (NostrRelayWriteRequest *)malloc(sizeof(NostrRelayWriteRequest));
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

void nostr_relay_publish(NostrRelay *relay, NostrEvent *event) {
    nostr_metric_timer t_ser = {0};
    nostr_metric_timer_start(&t_ser);
    char *event_json = nostr_event_serialize_compact(event);
    static nostr_metric_histogram *h_event_serialize_ns;
    if (!h_event_serialize_ns) h_event_serialize_ns = nostr_metric_histogram_get("event_serialize_ns");
    nostr_metric_timer_stop(&t_ser, h_event_serialize_ns);
    if (!event_json)
        return;

    nostr_metric_counter_add("events_published", 1);
    nostr_metric_counter_add("ws_tx_bytes", (uint64_t)strlen(event_json));
    (void)nostr_relay_write(relay, event_json);
    free(event_json);
}

void nostr_relay_auth(NostrRelay *relay, void (*sign)(NostrEvent *, Error **), Error **err) {

    NostrEvent auth_event = {
        .id = NULL,
        .pubkey = NULL,
        .created_at = 0,
        .kind = NOSTR_KIND_CLIENT_AUTHENTICATION,
        .tags = nostr_tags_new(2,
                nostr_tag_new("challenge", relay->priv->challenge, NULL),
                nostr_tag_new("relay", relay->url, NULL)),
        .content = strdup(""),
        .sig = NULL};
    Error *sig_err = NULL;
    sign(&auth_event, &sig_err);
    if (sig_err) {
        if (err) *err = sig_err;
        return;
    }
    nostr_relay_publish(relay, &auth_event);
    nostr_tags_free(auth_event.tags);
}

bool nostr_relay_subscribe(NostrRelay *relay, GoContext *ctx, NostrFilters *filters, Error **err) {
    // Ensure the relay connection exists
    if (!relay->connection) {
        if (err) *err = new_error(1, "not connected to %s", relay->url);
        return false;
    }

    // Prepare the subscription
    NostrSubscription *subscription = nostr_relay_prepare_subscription(relay, ctx, filters);
    if (!subscription) {
        if (err && *err == NULL) *err = new_error(1, "failed to prepare subscription");
        return false;
    }

    // Send the subscription request (Fire the subscription)
    if (!nostr_subscription_fire(subscription, err)) {
        if (err && *err == NULL) *err = new_error(ERR_RELAY_SUBSCRIBE_FAILED, "couldn't subscribe to filter at relay");
        return false;
    }

    // Successfully subscribed
    return true;
}

NostrSubscription *nostr_relay_prepare_subscription(NostrRelay *relay, GoContext *ctx, NostrFilters *filters) {
    if (!relay || !filters || !ctx) {
        return NULL;
    }

    // Generate a unique subscription ID
    static int64_t subscription_counter = 1;
    int64_t subscription_id = subscription_counter++;

    NostrSubscription *subscription = nostr_subscription_new(relay, filters);
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
    subscription->priv->match = nostr_filters_match; // Function for matching filters with events
    // Lifecycle watcher is already started in nostr_subscription_new()

    // Store subscription in relay subscriptions map
    go_hash_map_insert_int(relay->subscriptions, subscription->priv->counter, subscription);

    return subscription;
}

GoChannel *nostr_relay_query_events(NostrRelay *relay, GoContext *ctx, NostrFilter *filter, Error **err) {
    if (!relay->connection) {
        if (err) *err = new_error(1, "not connected to relay");
        return NULL;
    }

    NostrFilters filters = {
        .filters = filter,
        .count = 1,
        .capacity = 1,
    };

    // Prepare the subscription
    NostrSubscription *subscription = nostr_relay_prepare_subscription(relay, ctx, &filters);
    if (!subscription) {
        if (err) *err = new_error(ERR_RELAY_SUBSCRIBE_FAILED, "failed to prepare subscription");
        return NULL;
    }

    // Fire the subscription (send REQ)
    if (!nostr_subscription_fire(subscription, err)) {
        if (err) *err = new_error(ERR_RELAY_SUBSCRIBE_FAILED, "couldn't subscribe to filter at relay");
        return NULL;
    }

    // Return the channel where events will be received
    return subscription->events;
}

NostrEvent **nostr_relay_query_sync(NostrRelay *relay, GoContext *ctx, NostrFilter *filter, int *event_count, Error **err) {
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

    NostrFilters filters = {
        .filters = filter,
        .count = 1,
        .capacity = 1,
    };

    // Prepare the subscription
    NostrSubscription *subscription = nostr_relay_prepare_subscription(relay, ctx, &filters);
    if (!subscription) {
        *err = new_error(1, "failed to prepare subscription");
        free(events);
        return NULL;
    }

    // Fire the subscription (send REQ)
    if (!nostr_subscription_fire(subscription, err)) {
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
            nostr_subscription_unsubscribe(subscription); // Unsubscribe from the relay
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
            if (err) *err = new_error(1, "nostr_relay_query_sync timed out");
            nostr_subscription_unsubscribe(subscription);
            *event_count = (int)received_count;
            stop_ticker(timeout);
            return events;
        }
        default:
            break;
        }
    }
}

int64_t nostr_relay_count(NostrRelay *relay, GoContext *ctx, NostrFilter *filter, Error **err) {
    if (!relay->connection) {
        if (err) *err = new_error(1, "not connected to relay");
        return 0;
    }

    NostrFilters filters = {
        .filters = filter,
        .count = 1,
        .capacity = 1,
    };

    // Prepare the subscription (but don't fire it yet)
    NostrSubscription *subscription = nostr_relay_prepare_subscription(relay, ctx, &filters);
    if (!subscription) {
        if (err) *err = new_error(1, "failed to prepare subscription");
        return 0;
    }

    subscription->priv->count_result = go_channel_create(1);

    // Fire the subscription (send REQ)
    if (!nostr_subscription_fire(subscription, err)) {
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

bool nostr_relay_close(NostrRelay *r, Error **err) {
    if (!r) {
        if (err) *err = new_error(ERR_RELAY_CLOSE_FAILED, "invalid relay");
        return false;
    }

    // Cancel context to wake workers and stop I/O
    nsync_mu_lock(&r->priv->mutex);
    r->priv->connection_context_cancel(r->priv->connection_context);
    // Close queues to unblock writer/select before tearing down connection
    if (r->priv->write_queue) go_channel_close(r->priv->write_queue);
    if (r->priv->subscription_channel_close_queue) go_channel_close(r->priv->subscription_channel_close_queue);
    // Snapshot connection and clear relay pointer so workers see NULL
    NostrConnection *conn = r->connection;
    r->connection = NULL;
    nsync_mu_unlock(&r->priv->mutex);

    if (!conn) {
        if (err) *err = new_error(ERR_RELAY_CLOSE_FAILED, "relay not connected");
        return false;
    }

    // Ownership note:
    // - nostr_connection_close() only closes channels; it MUST NOT free them.
    // - nostr_relay_close() is responsible for freeing conn->recv_channel/send_channel
    //   but ONLY AFTER all workers have exited to avoid use-after-free in go_select.
    // Ensure workers exit before tearing down the connection to avoid UAF
    // Workers observe r->connection == NULL and/or context cancel and then call done
    go_wait_group_wait(&r->priv->workers);
    // Now that workers are done, it's safe to free connection-owned channels
    if (conn->recv_channel) { go_channel_free(conn->recv_channel); conn->recv_channel = NULL; }
    if (conn->send_channel) { go_channel_free(conn->send_channel); conn->send_channel = NULL; }
    // Close the network connection outside the relay mutex
    nostr_connection_close(conn);
    return true;
}

void nostr_relay_disconnect(NostrRelay *relay) {
    if (!relay) return;
    Error *err = NULL;
    (void)nostr_relay_close(relay, &err);
    if (err) free_error(err);
}

/* Legacy relay_unsubscribe removed. Use nostr_subscription_unsubscribe() via NostrSubscription* or
 * provide a thin wrapper in higher layers if needed. */
