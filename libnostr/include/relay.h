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
} Relay;

Relay *new_relay(GoContext *context, const char *url, Error **err);
void free_relay(Relay *relay);
bool relay_connect(Relay *relay, Error **err);
void relay_disconnect(Relay *relay);
bool relay_subscribe(Relay *relay, GoContext *ctx, Filters *filters, Error **err);
Subscription *relay_prepare_subscription(Relay *relay, GoContext *ctx, Filters *filters);
GoChannel *relay_query_events(Relay *relay, GoContext *ctx, Filter *filter, Error **err);
NostrEvent **relay_query_sync(Relay *relay, GoContext *ctx, Filter *filter, int *event_count, Error **err);
int64_t relay_count(Relay *relay, GoContext *ctx, Filter *filter, Error **err);
bool relay_close(Relay *relay, Error **err);
void relay_unsubscribe(Relay *relay, const char *subscription_id);
GoChannel *relay_write(Relay *r, char *msg);
void relay_publish(Relay *relay, NostrEvent *event);
void relay_auth(Relay *relay, void (*sign)(NostrEvent *, Error **), Error **err);
bool relay_is_connected(Relay *relay);

#endif // NOSTR_RELAY_H
