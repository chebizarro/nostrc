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
    (void)user; // not used; use opaque user data API
    Connection *conn = (Connection *)lws_get_opaque_user_data(wsi);

    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
        // Add custom headers (e.g., User-Agent) before the WebSocket handshake
        if (!in || !len) break;
        unsigned char **p = (unsigned char **)in;
        unsigned char *end = (*p) + len;

        const char *user_agent_val = "nostrc/1.0";
        if (lws_add_http_header_by_name(wsi, (unsigned char *)"User-Agent:",
                                        (unsigned char *)user_agent_val,
                                        (int)strlen(user_agent_val), p, end)) {
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
        if (!conn) break;
        WebSocketMessage *msg = (WebSocketMessage *)malloc(sizeof(WebSocketMessage));
        msg->data = (char *)malloc(len + 1);
        memcpy(msg->data, in, len);
        msg->data[len] = '\0';
        msg->length = len;

        go_channel_send(conn->recv_channel, msg); // Send to receive channel

        break;
    }
    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        // Pop a message from the send_channel and send it over the WebSocket (non-blocking)
        if (!conn) break;
        WebSocketMessage *msg = NULL;
        if (go_channel_try_receive(conn->send_channel, (void **)&msg) == 0 && msg) {
            size_t total = LWS_PRE + msg->length;
            unsigned char *buf = (unsigned char *)malloc(total);
            if (!buf) {
                fprintf(stderr, "OOM allocating write buffer\n");
                free(msg->data);
                free(msg);
                return -1;
            }
            unsigned char *p = buf + LWS_PRE;

            // Copy the message data and send it over the WebSocket
            memcpy(p, msg->data, msg->length);
            int n = lws_write(wsi, p, msg->length, LWS_WRITE_TEXT);
            if (n < 0) {
                fprintf(stderr, "Error writing WebSocket message\n");
                free(msg->data);
                free(msg);
                free(buf);
                return -1;
            }

            free(buf);
            free(msg->data);
            free(msg);

            // Ask for another writable callback in case there are more messages pending
            lws_callback_on_writable(wsi);
        }

        break;
    }
    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        // Nothing to do here; higher layers manage lifecycle.
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

static void *lws_service_loop(void *arg) {
    Connection *c = (Connection *)arg;
    while (c->priv->running) {
        lws_service(c->priv->context, 50);
    }
    return NULL;
}

// Minimal URL parser for ws/wss URLs: scheme://host[:port][/path]
static void parse_ws_url(const char *url, int *use_ssl, char *host, size_t host_sz,
                         int *port, char *path, size_t path_sz) {
    if (!url || !host || !path || !use_ssl || !port) return;
    *use_ssl = 0;
    *port = 0;
    host[0] = '\0';
    path[0] = '\0';

    const char *p = url;
    if (!strncmp(p, "wss://", 6)) {
        *use_ssl = 1;
        p += 6;
    } else if (!strncmp(p, "ws://", 5)) {
        *use_ssl = 0;
        p += 5;
    }

    // host[:port][/<path>]
    const char *host_start = p;
    const char *host_end = host_start;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;
    size_t host_len = (size_t)(host_end - host_start);
    if (host_len >= host_sz) host_len = host_sz - 1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    const char *after_host = host_end;
    if (*after_host == ':') {
        after_host++;
        char port_buf[8] = {0};
        size_t i = 0;
        while (*after_host && *after_host != '/' && i < sizeof port_buf - 1) {
            if (*after_host < '0' || *after_host > '9') break;
            port_buf[i++] = *after_host++;
        }
        *port = atoi(port_buf);
    }
    if (*after_host == '\0') {
        snprintf(path, path_sz, "/");
    } else if (*after_host == '/') {
        snprintf(path, path_sz, "%s", after_host);
    } else {
        snprintf(path, path_sz, "/");
    }
    if (*port == 0) *port = *use_ssl ? 443 : 80;
}

Connection *new_connection(const char *url) {
    struct lws_context_creation_info context_info;
    struct lws_client_connect_info connect_info;
    struct lws_context *context;

    lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_DEBUG | LLL_HEADER | LLL_CLIENT , NULL);

    lws_retry_bo_t retry_bo = {
        .retry_ms_table = retry_table,
        .retry_ms_table_count = LWS_ARRAY_SIZE(retry_table),
        .conceal_count = 3,            // Number of retries before giving up
        .secs_since_valid_ping = 29,   // Issue a PING after 30 seconds of idle time
        .secs_since_valid_hangup = 60, // Hang up the connection if no response after 60 seconds
        .jitter_percent = 5            // Add 5% random jitter to avoid synchronized retries
    };

    Connection *conn = calloc(1, sizeof(Connection));
    ConnectionPrivate *priv = calloc(1, sizeof(ConnectionPrivate));
    if (!conn || !priv)
        return NULL;
    conn->priv = priv;
    conn->priv->enable_compression = 0;
    nsync_mu_init(&conn->priv->mutex);

    // Check for test mode: bypass real network and event loop
    const char *test_env = getenv("NOSTR_TEST_MODE");
    int test_mode = (test_env && *test_env && strcmp(test_env, "0") != 0) ? 1 : 0;

    if (test_mode) {
        Connection *conn = calloc(1, sizeof(Connection));
        ConnectionPrivate *priv = calloc(1, sizeof(ConnectionPrivate));
        if (!conn || !priv) {
            if (conn) free(conn);
            if (priv) free(priv);
            return NULL;
        }
        conn->priv = priv;
        nsync_mu_init(&conn->priv->mutex);
        conn->priv->test_mode = 1;
        conn->recv_channel = go_channel_create(16);
        conn->send_channel = go_channel_create(16);
        // No context / thread in test mode
        return conn;
    }

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

    conn->priv->context = context;

    // Initialize client connect info
    memset(&connect_info, 0, sizeof(connect_info));
    connect_info.context = context;
    char host[256];
    char path[512];
    int port = 0;
    int use_ssl = 0;
    parse_ws_url(url, &use_ssl, host, sizeof host, &port, path, sizeof path);
    connect_info.address = host;
    connect_info.port = port;
    connect_info.path = path;
    connect_info.host = host;
    connect_info.origin = host;
    connect_info.ssl_connection = use_ssl ? LCCSCF_USE_SSL : 0;
    connect_info.protocol = NULL; // no subprotocol for nostr by default
    connect_info.pwsi = &conn->priv->wsi;
    connect_info.userdata = conn;

    conn->priv->wsi = lws_client_connect_via_info(&connect_info);
    if (!conn->priv->wsi) {
        lws_context_destroy(context);
        free(conn);
        return NULL;
    }
    // Attach our Connection* so callbacks can retrieve it safely
    lws_set_opaque_user_data(conn->priv->wsi, conn);
    conn->recv_channel = go_channel_create(16);
    conn->send_channel = go_channel_create(16);

    // Start a background service loop to pump libwebsockets
    conn->priv->running = 1;
    pthread_create(&conn->priv->service_thread, NULL, lws_service_loop, conn);
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
    if (!conn) return;
    // Close channels to unblock any waiters before freeing
    if (conn->recv_channel) go_channel_close(conn->recv_channel);
    if (conn->send_channel) go_channel_close(conn->send_channel);

    if (conn->priv && conn->priv->test_mode) {
        // No libwebsockets context or thread in test mode
        free(conn->priv);
    } else if (conn->priv) {
        // Signal thread to stop and join
        conn->priv->running = 0;
        // Wake service loop
        lws_cancel_service(conn->priv->context);
        pthread_join(conn->priv->service_thread, NULL);
        lws_context_destroy(conn->priv->context);
        free(conn->priv);
    }

    // Free channels after they have been closed and any waiters unblocked
    if (conn->recv_channel) go_channel_free(conn->recv_channel);
    if (conn->send_channel) go_channel_free(conn->send_channel);
    free(conn);
}

void connection_write_message(Connection *conn, GoContext *ctx, char *message, Error **err) {
    if (!conn || !message) {
        if (err) {
            *err = new_error(1, "Invalid connection or message");
        }
        return;
    }

    // In test mode, pretend the write succeeded without touching websockets
    if (conn->priv && conn->priv->test_mode) {
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

void connection_read_message(Connection *conn, GoContext *ctx, char *buffer, size_t buffer_size, Error **err) {
    if (!conn || !buffer) {
        if (err) {
            *err = new_error(1, "Invalid connection or buffer");
        }
        return;
    }

    // In test mode, simulate no data and request caller to stop waiting
    if (conn->priv && conn->priv->test_mode) {
        if (err) *err = new_error(1, "test mode: no data");
        return;
    }

    // Wait on either an incoming message or context cancellation
    WebSocketMessage *msg = NULL;
    if (ctx) {
        GoSelectCase cases[] = {
            {GO_SELECT_RECEIVE, conn->recv_channel, (void **)&msg},
            {GO_SELECT_RECEIVE, ctx->done, NULL},
        };
        int idx = go_select(cases, 2);
        if (idx == 1) {
            if (err) *err = new_error(1, "Context canceled");
            return;
        }
        // idx == 0 => message
        if (msg == NULL) {
            if (err) *err = new_error(1, "Receive failed or channel closed");
            return;
        }
    } else {
        if (go_channel_receive(conn->recv_channel, (void **)&msg) != 0 || !msg) {
            if (err) *err = new_error(1, "Failed to receive message or channel closed");
            return;
        }
    }

    if (msg) {
        if (msg->length > 0) {
            // Check if the buffer can accommodate the message and the null terminator
            if (msg->length < buffer_size) {
                strncpy(buffer, msg->data, msg->length);
                buffer[msg->length] = '\0'; // Ensure null termination
            } else {
                // Handle case where message exceeds buffer size
                if (err) {
                    *err = new_error(1, "Buffer too small to hold message");
                }
                // Free memory and return early to avoid issues
                free(msg->data);
                free(msg);
                return;
            }
        }

        // Free message memory
        free(msg->data);
        free(msg);
    }
}
