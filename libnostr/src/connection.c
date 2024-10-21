#include "connection.h"
#include "connection-private.h"
#include "go.h"
#include <libwebsockets.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PAYLOAD_SIZE 1024
#define MAX_HEADER_SIZE 1024

static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len) {
    Connection *conn = (Connection *)user;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
        // Add custom headers (e.g., User-Agent) before the WebSocket handshake
        unsigned char **p = (unsigned char **)in;
        unsigned char *end = (*p) + len;

        const char *user_agent = "User-Agent: nostrc/1.0\r\n";
        if (lws_add_http_header_by_name(wsi, (unsigned char *)"User-Agent:",
                                        (unsigned char *)user_agent,
                                        strlen(user_agent), p, end)) {
            return 1;
        }
        break;
    }
    case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        printf("WebSocket connection established\n");
        lws_callback_on_writable(wsi); // Ensure the socket becomes writable
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        // Receive a message from the WebSocket and push it into the recv_channel
        WebSocketMessage *msg = (WebSocketMessage *)malloc(sizeof(WebSocketMessage));
        msg->data = (char *)malloc(len + 1);
        memcpy(msg->data, in, len);
        msg->data[len] = '\0';
        msg->length = len;

        go_channel_send(conn->recv_channel, msg); // Send to receive channel

        break;
    }
    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        // Pop a message from the send_channel and send it over the WebSocket
        WebSocketMessage *msg;
        if (go_channel_receive(conn->send_channel, (void **)&msg) == 0) {
            unsigned char buf[LWS_PRE + MAX_PAYLOAD_SIZE];
            unsigned char *p = &buf[LWS_PRE];

            // Copy the message data and send it over the WebSocket
            memcpy(p, msg->data, msg->length);
            int n = lws_write(wsi, p, msg->length, LWS_WRITE_TEXT);
            if (n < 0) {
                fprintf(stderr, "Error writing WebSocket message\n");
                free(msg->data);
                free(msg);
                return -1;
            }

            free(msg->data);
            free(msg);

            // Check if there are more messages to send
            if (go_channel_receive(conn->send_channel, (void **)&msg) == 0) {
                lws_callback_on_writable(wsi); // Request another writable callback
            }
        }

        break;
    }
    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        //connection_close(conn);
        break;
    default:
        break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    {"wss", websocket_callback, 0, MAX_PAYLOAD_SIZE},
    LWS_PROTOCOL_LIST_TERM};

static const uint32_t retry_table[] = {1000, 2000, 3000}; // Retry intervals in ms

Connection *new_connection(const char *url) {
    struct lws_context_creation_info context_info;
    struct lws_client_connect_info connect_info;
    struct lws_context *context;

    lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_DEBUG | LLL_HEADER | LLL_LATENCY | LLL_CLIENT | LLL_PARSER | LLL_EXT | LLL_INFO, NULL);

    lws_retry_bo_t retry_bo = {
        .retry_ms_table = retry_table,
        .retry_ms_table_count = LWS_ARRAY_SIZE(retry_table),
        .conceal_count = 3,            // Number of retries before giving up
        .secs_since_valid_ping = 29,   // Issue a PING after 30 seconds of idle time
        .secs_since_valid_hangup = 60, // Hang up the connection if no response after 60 seconds
        .jitter_percent = 5            // Add 5% random jitter to avoid synchronized retries
    };

    Connection *conn = malloc(sizeof(Connection));
    ConnectionPrivate *priv = malloc(sizeof(ConnectionPrivate));
    if (!conn || !priv)
        return NULL;
    conn->priv = priv;
    conn->priv->enable_compression = 0;

    // Initialize context creation info
    memset(&context_info, 0, sizeof(context_info));
    context_info.retry_and_idle_policy = &retry_bo;
    context_info.port = CONTEXT_PORT_NO_LISTEN;
    context_info.protocols = protocols;
    context_info.gid = -1;
    context_info.uid = -1;
    context_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
                           LWS_SERVER_OPTION_VALIDATE_UTF8;
    context_info.fd_limit_per_thread = 1 + 1 + 1;
    context_info.pt_serv_buf_size = 32 * 1024;

    context = lws_create_context(&context_info);
    if (!context) {
        free(conn);
        return NULL;
    }

    // Initialize client connect info
    memset(&connect_info, 0, sizeof(connect_info));
    connect_info.context = context;
    connect_info.address = url;
    connect_info.port = 443;
    connect_info.path = "/";
    connect_info.host = lws_canonical_hostname(context);
    connect_info.origin = connect_info.host;
    connect_info.ssl_connection = LCCSCF_USE_SSL;
    connect_info.protocol = "wss";
    connect_info.pwsi = &conn->priv->wsi;
    connect_info.userdata = conn;

    conn->priv->wsi = lws_client_connect_via_info(&connect_info);
    if (!conn->priv->wsi) {
        lws_context_destroy(context);
        free(conn);
        return NULL;
    }
    conn->recv_channel = go_channel_create(16);
    conn->send_channel = go_channel_create(16);
    return conn;
}

// Coroutine for processing incoming WebSocket messages
void *websocket_receive_coroutine(void *arg) {
    Connection *conn = (Connection *)arg;
    while (1) {
        WebSocketMessage *msg;
        if (go_channel_receive(conn->recv_channel, (void **)&msg) == 0) {
            printf("Received message: %s\n", msg->data); // Process the message
            free(msg->data);
            free(msg);
        }
    }
}

// Coroutine for processing outgoing WebSocket messages
void *websocket_send_coroutine(void *arg) {
    Connection *conn = (Connection *)arg;
    while (1) {
        WebSocketMessage *msg;
        if (go_channel_receive(conn->send_channel, (void **)&msg) == 0) {
            lws_callback_on_writable(conn->priv->wsi); // Signal that there is data to send
        }
    }
}

void connection_close(Connection *conn) {
    lws_context_destroy(conn->priv->context);
    free(conn);
}

void connection_write_message(Connection *conn, GoContext *ctx, char *message, Error **err) {
    if (!conn || !message) {
        if (err) {
            *err = new_error(1, "Invalid connection or message");
        }
        return;
    }

    // Prepare the message to send
    size_t message_length = strlen(message);
    WebSocketMessage *msg = malloc(sizeof(WebSocketMessage));
    if (!msg) {
        if (err) {
            *err = new_error(1, "Memory allocation failure for WebSocketMessage");
        }
        return;
    }

    msg->data = malloc(message_length + 1);
    if (!msg->data) {
        free(msg);
        if (err) {
            *err = new_error(1, "Memory allocation failure for message data");
        }
        return;
    }

    strcpy(msg->data, message);
    msg->length = message_length;

    // Add the message to the send channel
    go_channel_send(conn->send_channel, msg);

    // Ensure that the socket becomes writable
    lws_callback_on_writable(conn->priv->wsi);
}

void connection_read_message(Connection *conn, GoContext *ctx, char *buffer, Error **err) {
    if (!conn || !buffer) {
        if (err) {
            *err = new_error(1, "Invalid connection or buffer");
        }
        return;
    }

    // Wait for a message from the recv_channel
    WebSocketMessage *msg;
    if (go_channel_receive(conn->recv_channel, (void **)&msg) == 0) {
        // Copy the received message to the buffer
        if (msg->length > 0) {
            strncpy(buffer, msg->data, msg->length);
            buffer[msg->length] = '\0'; // Ensure null termination
        }

        free(msg->data);
        free(msg);
    } else {
        if (err) {
            *err = new_error(1, "Failed to receive message");
        }
    }
}
