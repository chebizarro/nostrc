#include "nostr.h"
#include "relay-private.h"
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <libwebsockets.h>


static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("WebSocket connection established\n");
            break;
        case LWS_CALLBACK_CLIENT_RECEIVE:
            printf("Received data: %.*s\n", (int)len, (char *)in);
            break;
        case LWS_CALLBACK_CLIENT_WRITEABLE:
            // This is where you would write data to the WebSocket
            break;
        case LWS_CALLBACK_CLIENT_CLOSED:
            printf("WebSocket connection closed\n");
            break;
        default:
            break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    { "wss", websocket_callback, 0, 128 },
    { NULL, NULL, 0, 0 }
};


Relay *create_relay(const char *url) {
    // Allocate memory for the Relay and RelayPrivate structs
    Relay *relay = (Relay *)malloc(sizeof(Relay));
    RelayPrivate *priv = (RelayPrivate *)malloc(sizeof(RelayPrivate));
    if (!relay || !priv) return NULL;

    // Initialize the relay URL
    relay->url = strdup(url);
    relay->priv = priv;
    relay->priv->port = 443;  // Default WebSocket over SSL port
 
    // Initialize the WebSocket context and instance
    relay->priv->ws_context = NULL;  // WebSocket context will be created during connection
    relay->priv->wsi = NULL;         // No WebSocket instance yet

    // Initialize the subscriptions (assuming create_filters is a valid function)
    //relay->subscriptions = create_filters(0);

    // Initialize the private data (mutex and handlers)
    pthread_mutex_init(&relay->priv->mutex, NULL);
    relay->priv->assume_valid = false;
    relay->priv->notice_handler = NULL;
    relay->priv->signature_checker = NULL;

    return relay;
}

void free_relay(Relay *relay) {
    if (relay) {
        // Free the URL
        free(relay->url);
        // Clean up WebSocket context and instance
        if (relay->priv->wsi) {
            // Close the WebSocket connection
            lws_set_timeout(relay->priv->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
            lws_service(relay->priv->ws_context, 0);  // Process the WebSocket closure
            relay->priv->wsi = NULL;
        }
        
        // Destroy the WebSocket context
        if (relay->priv->ws_context) {
            lws_context_destroy(relay->priv->ws_context);
            relay->priv->ws_context = NULL;
        }

        //free_filters(relay->subscriptions);

        // Destroy the mutex
        pthread_mutex_destroy(&relay->priv->mutex);

        // Free the private data
        free(relay->priv);

        // Finally, free the relay itself
        free(relay);
    }
}
int relay_connect(Relay *relay) {
    struct lws_client_connect_info connect_info;
    memset(&connect_info, 0, sizeof(connect_info));

	struct lws_context_creation_info context_info;
	memset(&context_info, 0, sizeof(context_info));

    // Set up the WebSocket context if it's not already created
    if (!relay->priv->ws_context) {
        context_info.port = CONTEXT_PORT_NO_LISTEN;  // No listening on a port
        context_info.protocols = protocols;               // We'll define our protocols below
        context_info.gid = -1;
        context_info.uid = -1;

		// Enable detailed logging
		lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO | LLL_DEBUG, NULL);

        // Create the WebSocket context and store it in the relay
        relay->priv->ws_context = lws_create_context(&context_info);
        if (!relay->priv->ws_context) {
            fprintf(stderr, "Failed to create WebSocket context\n");
            return -1;
        }
    }

    // Set up the connection information
    connect_info.context = relay->priv->ws_context;
    connect_info.address = relay->url;           // Relay URL
    connect_info.port = relay->priv->port;             // WebSocket port (typically 443 for SSL)
    connect_info.path = "/";                     // WebSocket path
    connect_info.host = lws_canonical_hostname(relay->priv->ws_context);
    connect_info.origin = connect_info.host;
    connect_info.ssl_connection = relay->priv->ssl_connection; // Use SSL if set in the relay
    connect_info.protocol = "wss";                // WebSocket protocol
    connect_info.pwsi = &relay->priv->wsi;             // Store the WebSocket instance in the relay

    // Connect to the WebSocket server
    if (!lws_client_connect_via_info(&connect_info)) {
        fprintf(stderr, "Failed to connect to relay WebSocket\n");
        return -1;
    }

    // Process WebSocket events
    while (lws_service(relay->priv->ws_context, 0) >= 0) {
		fprintf(stdout, "Events incomming\n");
    }

    return 0;
}

void relay_disconnect(Relay *relay) {
    pthread_mutex_lock(&relay->priv->mutex);

    if (relay->priv->wsi) {
        // Close the WebSocket connection
        lws_set_timeout(relay->priv->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
        
        // Call service to actually process the closure
        lws_service(relay->priv->ws_context, 0);
        
        // Clear the WebSocket instance after closure
        relay->priv->wsi = NULL;
    }

    // Destroy the WebSocket context if it's still available
    if (relay->priv->ws_context) {
        lws_context_destroy(relay->priv->ws_context);
        relay->priv->ws_context = NULL;
    }

    pthread_mutex_unlock(&relay->priv->mutex);
}

int relay_subscribe(Relay *relay, Filters *filters) {
    pthread_mutex_lock(&relay->priv->mutex);
    //relay->subscriptions = filters;
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

    // Check if the WebSocket instance exists and if it's still connected
    bool connected = (relay->priv->wsi != NULL && lws_get_context(relay->priv->wsi) != NULL);

    pthread_mutex_unlock(&relay->priv->mutex);
    return connected;
}
