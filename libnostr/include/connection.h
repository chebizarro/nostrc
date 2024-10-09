#ifndef NOSTR_CONNECTION_H
#define NOSTR_CONNECTION_H

#include "go.h"

typedef struct _ConnectionPrivate ConnectionPrivate;

typedef struct _Connection {
    ConnectionPrivate *priv;
    GoChannel *send_channel;
    GoChannel *recv_channel;
} Connection;

Connection *new_connection(const char *url, int port);
void connection_close(Connection *conn);

#endif // NOSTR_CONNECTION_H