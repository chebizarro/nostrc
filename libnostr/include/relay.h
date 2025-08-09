#ifndef NOSTR_RELAY_H
#define NOSTR_RELAY_H

#include "connection.h"
#include "event.h"
#include "filter.h"

typedef struct _Subscription Subscription;
typedef struct _RelayPrivate RelayPrivate;

typedef struct Relay {
    RelayPrivate *priv;
    char *url;
    // request_header;
    Connection *connection;
    Error **connection_error;
    GoHashMap *subscriptions;
    bool assume_valid;
    int refcount;
} Relay;

/* Legacy relay_* and free_relay/new_relay symbols have been removed. */

#endif // NOSTR_RELAY_H
