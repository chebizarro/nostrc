#include "nostr.h"
#include "relay-private.h"
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>

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
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    if (getaddrinfo(relay->url, "443", &hints, &res) != 0) {
        return -1;
    }

    relay->priv->socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (relay->priv->socket == -1) {
        freeaddrinfo(res);
        return -1;
    }

    if (connect(relay->priv->socket, res->ai_addr, res->ai_addrlen) == -1) {
        close(relay->priv->socket);
        relay->priv->socket = -1;
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);

    relay->priv->ssl = SSL_new(relay->priv->ssl_ctx);
    SSL_set_fd(relay->priv->ssl, relay->priv->socket);

    if (SSL_connect(relay->priv->ssl) <= 0) {
        SSL_free(relay->priv->ssl);
        relay->priv->ssl = NULL;
        close(relay->priv->socket);
        relay->priv->socket = -1;
        return -1;
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