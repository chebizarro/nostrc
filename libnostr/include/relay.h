#ifndef NOSTR_RELAY_H
#define NOSTR_RELAY_H

#include "filter.h"
#include "event.h"

typedef struct _RelayPrivate RelayPrivate;

typedef struct Relay {
    RelayPrivate *priv;
    char *url;
    //Subscription *subscriptions;
} Relay;

Relay *create_relay(const char *url);
void free_relay(Relay *relay);
int relay_connect(Relay *relay);
void relay_disconnect(Relay *relay);
int relay_subscribe(Relay *relay, Filters *filters);
void relay_unsubscribe(Relay *relay, int subscription_id);
void relay_publish(Relay *relay, NostrEvent *event);
void relay_auth(Relay *relay, void (*sign)(NostrEvent *));
bool relay_is_connected(Relay *relay);


#endif // NOSTR_RELAY_H