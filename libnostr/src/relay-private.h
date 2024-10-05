#ifndef RELAY_H
#define RELAY_H

#include "nostr.h"
#include <openssl/types.h>
#include <bits/pthreadtypes.h>


struct _RelayPrivate {
    int port;                             // Relay port (usually 443 for WebSocket over SSL)
    struct lws_context *ws_context;       // WebSocket context (reused across connections)
    struct lws *wsi;                      // WebSocket connection instance (reused)
    int ssl_connection;                   // SSL connection flag (if SSL is used)

    char *challenge;

    pthread_mutex_t mutex;
    bool assume_valid;
    void (*notice_handler)(const char *);
    bool (*signature_checker)(NostrEvent);
};


#endif // RELAY_H