#ifndef NOSTR_RELAY_H
#define NOSTR_RELAY_H

#include "connection.h"
#include "event.h"
#include "filter.h"

typedef struct _RelayPrivate RelayPrivate;

typedef struct Relay {
    RelayPrivate *priv;
    char *url;
    // request_header;
    Connection *connection;
    Error **connection_error;
    ConcurrentHashMap *subscriptions;
    bool assume_valid;
} Relay;

Relay *new_relay(GoContext *context, const char *url, Error **err);
void free_relay(Relay *relay);
int relay_connect(Relay *relay, Error **err);
void relay_disconnect(Relay *relay);
int relay_subscribe(Relay *relay, Filters *filters);
int relay_close(Relay *relay);
void relay_unsubscribe(Relay *relay, int subscription_id);
void relay_publish(Relay *relay, NostrEvent *event);
void relay_auth(Relay *relay, void (*sign)(NostrEvent *));
bool relay_is_connected(Relay *relay);

#endif // NOSTR_RELAY_H