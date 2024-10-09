#ifndef NOSTR_CONNECTION_H
#define NOSTR_CONNECTION_H

typedef struct _ConnectionPrivate ConnectionPrivate;

typedef struct _Connection {
    ConnectionPrivate *priv;
} Connection;

Connection * new_connection(const char * url, int port);

#endif // NOSTR_CONNECTION_H