#include "relay.h"
#include "relay-private.h"
#include "kinds.h"

Relay *new_relay(GoContext *context, char *url) {
    if (url == NULL) {
        fprintf(stderr, "invalid relay URL\n");
        return NULL;
    }

    Relay *relay = (Relay *)malloc(sizeof(Relay));
    RelayPrivate *priv = (RelayPrivate *)malloc(sizeof(RelayPrivate));
    if (!relay || !priv)
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
    relay->priv->signature_checker = NULL;

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

int relay_connect(Relay *relay) {
    if (relay == NULL) {
        fprintf(stderr, "relay must be initialized with a call to new_relay()\n");
        return -1;
    }

    Connection *conn = new_connection(relay->url);
    if (conn == NULL) {
        fprintf(stderr, "error opening websocket to '%s'\n", relay->url);
        return -1;
    }
    relay->connection = conn;



    return 0;
}

void relay_write(char *msg) {

}

void relay_publish(Relay *relay, NostrEvent *event) {

}

void relay_auth(Relay *relay, void (*sign)(NostrEvent *)) {
    pthread_mutex_lock(&relay->priv->mutex);

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

    pthread_mutex_unlock(&relay->priv->mutex);
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

int relay_close(Relay *relay, Filter *filter) {
    return 0;
}


void relay_unsubscribe(Relay *relay, int subscription_id) {



}


void relay_disconnect(Relay *relay) {
}



bool relay_is_connected(Relay *relay) {
    pthread_mutex_lock(&relay->priv->mutex);
    // Check if the WebSocket instance exists and if it's still connected
    bool connected = (relay->priv->wsi != NULL && lws_get_context(relay->priv->wsi) != NULL);
    pthread_mutex_unlock(&relay->priv->mutex);
    return connected;
}
