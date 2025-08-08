#ifndef CONNECTION_PRIVATE_H
#define CONNECTION_PRIVATE_H

#include <libwebsockets.h>
#include <pthread.h>
#include "nsync.h"

struct _ConnectionPrivate {
    struct lws *wsi;
    int enable_compression;
    struct lws_context *context;
    nsync_mu mutex;
    pthread_t service_thread;
    int running;
    int test_mode; // when set, no real network; helpers short-circuit
};

// Struct to hold WebSocket message
typedef struct WebSocketMessage {
    char *data;
    size_t length;
} WebSocketMessage;

#endif // CONNECTION_PRIVATE_H
