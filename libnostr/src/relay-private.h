#ifndef NOSTR_RELAY_PRIVATE_H
#define NOSTR_RELAY_PRIVATE_H

#include "event.h"
#include <stdbool.h>

struct _RelayPrivate {
    int port;                       // Relay port (usually 443 for WebSocket over SSL)
    struct lws_context *ws_context; // WebSocket context (reused across connections)
    struct lws *wsi;                // WebSocket connection instance (reused)
    int ssl_connection;             // SSL connection flag (if SSL is used)

    char *challenge;

    pthread_mutex_t mutex;
    bool assume_valid;
    void (*notice_handler)(const char *);
    bool (*signature_checker)(NostrEvent);
};

#endif // NOSTR_RELAY_PRIVATE_H