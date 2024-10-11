#ifndef NOSTR_CONNECTION_H
#define NOSTR_CONNECTION_H

#include "go.h"

typedef struct _ConnectionPrivate ConnectionPrivate;

typedef struct _Connection {
    ConnectionPrivate *priv;
    GoChannel *send_channel;
    GoChannel *recv_channel;
} Connection;

Connection *new_connection(const char *url);

void connection_close(Connection *conn);

void connection_write_message(Connection *conn, GoContext *ctx, char *buffer, Error **err);

void connection_read_message(Connection *conn, GoContext *ctx, char *message, Error **err);

#endif // NOSTR_CONNECTION_H