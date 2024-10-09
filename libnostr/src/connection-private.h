#ifndef CONNECTION_PRIVATE_H
#define CONNECTION_PRIVATE_H

#include <libwebsockets.h>

struct _ConnectionPrivate {
    struct lws *wsi;
    int enable_compression;
    struct lws_context *context;
};

// Struct to hold WebSocket message
typedef struct WebSocketMessage {
    char *data;
    size_t length;
} WebSocketMessage;

#endif // CONNECTION_PRIVATE_H