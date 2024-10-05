#include "nostr.h"
#include "relay-private.h"
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <libwebsockets.h>

// Relay-related functions
Relay *create_relay(const char *url) {
    Relay *relay = (Relay *)malloc(sizeof(Relay));
    RelayPrivate *priv = (RelayPrivate *)malloc(sizeof(RelayPrivate));
    if (!relay || !priv) return NULL;

    relay->url = strdup(url);
    relay->priv = priv;

    relay->priv->ssl_ctx = SSL_CTX_new(TLS_method());
    relay->priv->ssl = NULL;
    relay->priv->socket = -1;
    relay->subscriptions = create_filters(0);
    pthread_mutex_init(&relay->priv->mutex, NULL);
    relay->priv->assume_valid = false;
    relay->priv->notice_handler = NULL;
    relay->priv->signature_checker = NULL;

    return relay;
}

void free_relay(Relay *relay) {
    if (relay) {
        free(relay->url);
        if (relay->priv->ssl) SSL_free(relay->priv->ssl);
        if (relay->priv->ssl_ctx) SSL_CTX_free(relay->priv->ssl_ctx);
        if (relay->priv->socket != -1) close(relay->priv->socket);
        free_filters(relay->subscriptions);
        pthread_mutex_destroy(&relay->priv->mutex);
        free(relay);
    }
}

int relay_connect(Relay *relay) {
    struct lws_client_connect_info connect_info;
    memset(&connect_info, 0, sizeof(connect_info));

    // Set up the WebSocket context if it's not already created
    if (!relay->ws_context) {
        struct lws_context_creation_info context_info;
        memset(&context_info, 0, sizeof(context_info));
        context_info.port = CONTEXT_PORT_NO_LISTEN;  // No listening on a port
        context_info.protocols = NULL;               // We'll define our protocols below
        context_info.gid = -1;
        context_info.uid = -1;

        // Create the WebSocket context and store it in the relay
        relay->ws_context = lws_create_context(&context_info);
        if (!relay->ws_context) {
            fprintf(stderr, "Failed to create WebSocket context\n");
            return -1;
        }
    }

    // Set up the connection information
    connect_info.context = relay->ws_context;
    connect_info.address = relay->url;           // Relay URL
    connect_info.port = relay->port;             // WebSocket port (typically 443 for SSL)
    connect_info.path = "/";                     // WebSocket path
    connect_info.host = lws_canonical_hostname(relay->ws_context);
    connect_info.origin = connect_info.host;
    connect_info.ssl_connection = relay->ssl_connection; // Use SSL if set in the relay
    connect_info.protocol = "ws";                // WebSocket protocol
    connect_info.pwsi = &relay->wsi;             // Store the WebSocket instance in the relay

    // Connect to the WebSocket server
    if (!lws_client_connect_via_info(&connect_info)) {
        fprintf(stderr, "Failed to connect to relay WebSocket\n");
        return -1;
    }

    // Process WebSocket events
    while (lws_service(relay->ws_context, 0) >= 0) {
        //...
    }

    return 0;
}

void relay_disconnect(Relay *relay) {
    pthread_mutex_lock(&relay->priv->mutex);
    if (relay->priv->ssl) {
        SSL_shutdown(relay->priv->ssl);
        SSL_free(relay->priv->ssl);
        relay->priv->ssl = NULL;
    }
    if (relay->priv->socket != -1) {
        close(relay->priv->socket);
        relay->priv->socket = -1;
    }
    pthread_mutex_unlock(&relay->priv->mutex);
}

int relay_subscribe(Relay *relay, Filters *filters) {
    pthread_mutex_lock(&relay->priv->mutex);
    relay->subscriptions = filters;
    // Add implementation to send subscription message to the relay
    pthread_mutex_unlock(&relay->priv->mutex);
    return 0;
}

void relay_unsubscribe(Relay *relay, int subscription_id) {
    pthread_mutex_lock(&relay->priv->mutex);
    // Add implementation to send unsubscription message to the relay
    pthread_mutex_unlock(&relay->priv->mutex);
}

void relay_publish(Relay *relay, NostrEvent *event) {
    pthread_mutex_lock(&relay->priv->mutex);
    // Add implementation to send event to the relay
    pthread_mutex_unlock(&relay->priv->mutex);
}

void relay_auth(Relay *relay, void (*sign)(NostrEvent *)) {
    pthread_mutex_lock(&relay->priv->mutex);
    NostrEvent auth_event = {
        .created_at = time(NULL),
        .kind = 22242,
        .tags = create_tags(2,
          create_tag("relay", relay->url),
          create_tag("challenge", relay->priv->challenge)
        ),
        .content = "",
    };

    relay_publish(relay, &auth_event);
    free_tags(auth_event.tags);
    pthread_mutex_unlock(&relay->priv->mutex);
}

bool relay_is_connected(Relay *relay) {
    pthread_mutex_lock(&relay->priv->mutex);
    bool connected = relay->priv->ssl != NULL;
    pthread_mutex_unlock(&relay->priv->mutex);
    return connected;
}