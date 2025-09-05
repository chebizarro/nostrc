#include "nostr-connection.h"
#include "connection-private.h"
#include "go.h"
#include "nostr/metrics.h"
#include "nostr-init.h"
#include "../include/security_limits_runtime.h"
#include "../include/rate_limiter.h"
#include "../include/nostr_log.h"
#include <libwebsockets.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/ssl.h>

#define MAX_PAYLOAD_SIZE 1024
#define MAX_HEADER_SIZE 1024

static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len) {
    (void)user; // not used; use opaque user data API
    NostrConnection *conn = (NostrConnection *)lws_get_opaque_user_data(wsi);

    switch (reason) {
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        if (!conn || !conn->priv) break;
        // Enforce hard frame cap
        if (len > (size_t)nostr_limit_max_frame_len()) {
            nostr_rl_log(NLOG_WARN, "ws", "drop: frame too large (%zu > %lld)", len, (long long)nostr_limit_max_frame_len());
            lws_close_reason(wsi, LWS_CLOSE_STATUS_MESSAGE_TOO_LARGE, NULL, 0);
            return -1;
        }
        // Token-bucket admission: frames/sec and bytes/sec
        if (!tb_allow(&conn->priv->tb_frames, 1.0) || !tb_allow(&conn->priv->tb_bytes, (double)len)) {
            nostr_rl_log(NLOG_WARN, "ws", "drop: rate limit exceeded (len=%zu)", len);
            lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION, NULL, 0);
            return -1;
        }
        // Update RX timing/progress
        uint64_t now_us = (uint64_t)lws_now_usecs();
        conn->priv->last_rx_ns = now_us; /* store usec */
        if (conn->priv->rx_window_start_ns == 0) {
            conn->priv->rx_window_start_ns = now_us;
            conn->priv->rx_window_bytes = 0;
        }
        conn->priv->rx_window_bytes += (uint64_t)len;
        // Normal path: deliver to recv_channel as before
        // Allocate a copy buffer and queue as a WebSocketMessage
        WebSocketMessage *msg = (WebSocketMessage*)malloc(sizeof(WebSocketMessage));
        if (!msg) return -1;
        msg->length = len;
        msg->data = (char*)malloc(len + 1);
        if (!msg->data) { free(msg); return -1; }
        memcpy(msg->data, in, len);
        msg->data[len] = '\0';
        go_channel_send(conn->recv_channel, msg);
        // Metrics
        nostr_metric_counter_add("ws_rx_enqueued_bytes", (uint64_t)len);
        nostr_metric_counter_add("ws_rx_enqueued_messages", 1);
        break;
    }
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
        // Initialize timers/progress trackers and arm periodic checks
        if (conn && conn->priv) {
            uint64_t now_us = (uint64_t)lws_now_usecs();
            conn->priv->last_rx_ns = now_us;
            conn->priv->rx_window_start_ns = now_us;
            conn->priv->rx_window_bytes = 0;
        }
        // Arm a 1s timer for periodic timeout/progress checks
        lws_set_timer_usecs(wsi, 1000000);
        lws_callback_on_writable(wsi); // Ensure the socket becomes writable
        break;
    
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
            // Metrics: time socket write and count TX at socket boundary
            nostr_metric_timer t_sock = {0};
            nostr_metric_timer_start(&t_sock);
            int n = lws_write(wsi, p, msg->length, LWS_WRITE_TEXT);
            static nostr_metric_histogram *h_ws_socket_write_ns;
            if (!h_ws_socket_write_ns) h_ws_socket_write_ns = nostr_metric_histogram_get("ws_socket_write_ns");
            nostr_metric_timer_stop(&t_sock, h_ws_socket_write_ns);
            nostr_metric_counter_add("ws_socket_tx_bytes", (uint64_t)msg->length);
            nostr_metric_counter_add("ws_socket_tx_messages", 1);
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
    case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
    case LWS_CALLBACK_OPENSSL_CTX_LOAD_EXTRA_CLIENT_VERIFY_CERTS: {
        // Configure OpenSSL SSL_CTX ciphers / versions / groups for client side
        (void)user; (void)len;
        SSL_CTX *ctx = (SSL_CTX *)in;
        if (!ctx) break;
        // Min protocol version: TLS 1.2 (prefer 1.3 during negotiation)
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        // Disable compression and renegotiation
        SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
        // TLS 1.3 cipher suites (server order irrelevant in TLS1.3, but set list)
        (void)SSL_CTX_set_ciphersuites(ctx,
          "TLS_AES_128_GCM_SHA256:"
          "TLS_CHACHA20_POLY1305_SHA256:"
          "TLS_AES_256_GCM_SHA384");
        // TLS 1.2 AEAD-only cipher list
        (void)SSL_CTX_set_cipher_list(ctx,
          "ECDHE-ECDSA-AES128-GCM-SHA256:"
          "ECDHE-RSA-AES128-GCM-SHA256:"
          "ECDHE-ECDSA-CHACHA20-POLY1305:"
          "ECDHE-RSA-CHACHA20-POLY1305:"
          "ECDHE-ECDSA-AES256-GCM-SHA384:"
          "ECDHE-RSA-AES256-GCM-SHA384");
        // Groups preference (key exchange)
        (void)SSL_CTX_set1_groups_list(ctx, "X25519:P-256");
        // Disable 0-RTT by simply not enabling early data API calls here
        // Session tickets (rotation advisable by external ticket key mgmt)
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER);
        SSL_CTX_set_num_tickets(ctx, 2);
        break;
    }
    case LWS_CALLBACK_TIMER: {
        if (!conn || !conn->priv) break;
        uint64_t now_us = (uint64_t)lws_now_usecs();
        int64_t read_to_s = nostr_limit_ws_read_timeout_seconds();
        if (read_to_s > 0) {
            uint64_t last_us = conn->priv->last_rx_ns ? conn->priv->last_rx_ns : now_us;
            if (now_us - last_us > (uint64_t)read_to_s * 1000000ULL) {
                nostr_rl_log(NLOG_WARN, "ws", "read timeout: no data for %llds", (long long)read_to_s);
                nostr_metric_counter_add("ws_timeout_read", 1);
                lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION, NULL, 0);
                return -1;
            }
        }
        int64_t win_ms = nostr_limit_ws_progress_window_ms();
        int64_t min_bytes = nostr_limit_ws_min_bytes_per_window();
        if (win_ms > 0 && min_bytes > 0) {
            uint64_t win_start = conn->priv->rx_window_start_ns ? conn->priv->rx_window_start_ns : now_us;
            if (now_us - win_start >= (uint64_t)win_ms * 1000ULL) {
                uint64_t bytes = conn->priv->rx_window_bytes;
                if (bytes < (uint64_t)min_bytes) {
                    nostr_rl_log(NLOG_WARN, "ws", "progress violation: %lluB < %lldB in %lldms", (unsigned long long)bytes, (long long)min_bytes, (long long)win_ms);
                    nostr_metric_counter_add("ws_progress_violation", 1);
                    lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION, NULL, 0);
                    return -1;
                }
                // reset window
                conn->priv->rx_window_start_ns = now_us;
                conn->priv->rx_window_bytes = 0;
            }
        }
        // Re-arm timer for continuous checks
        lws_set_timer_usecs(wsi, 1000000);
        break;
    }
    default:
        break;
    }
    return 0;
}

GoChannel *nostr_connection_get_send_channel(const NostrConnection *conn) {
    if (!conn) return NULL;
    return conn->send_channel;
}

GoChannel *nostr_connection_get_recv_channel(const NostrConnection *conn) {
    if (!conn) return NULL;
    return conn->recv_channel;
}

bool nostr_connection_is_running(const NostrConnection *conn) {
    if (!conn || !conn->priv) return false;
    return conn->priv->running != 0;
}

static const struct lws_protocols protocols[] = {
    {
        .name = "wss",
        .callback = websocket_callback,
        .per_session_data_size = 0,
        .rx_buffer_size = MAX_PAYLOAD_SIZE,
        .id = 0,
        .user = NULL,
        .tx_packet_size = 0,
    },
    LWS_PROTOCOL_LIST_TERM
};

static const uint32_t retry_table[] = {1000, 2000, 3000}; // Retry intervals in ms

/* Shared libwebsockets context & service thread */
static struct lws_context *g_lws_context = NULL;
static pthread_t g_lws_service_thread;
static int g_lws_running = 0;
static int g_lws_refcount = 0;
static pthread_mutex_t g_lws_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *lws_service_loop(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lws_mutex);
        int running = g_lws_running;
        struct lws_context *ctx = g_lws_context;
        pthread_mutex_unlock(&g_lws_mutex);
        if (!running || !ctx) break;
        lws_service(ctx, 50);
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

NostrConnection *nostr_connection_new(const char *url) {
    // Ensure global initialization runs (pulls in init.o and enables metrics auto-init)
    nostr_global_init();
    struct lws_context_creation_info context_info;
    struct lws_client_connect_info connect_info;

    lws_set_log_level(LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_DEBUG | LLL_HEADER | LLL_CLIENT , NULL);

    lws_retry_bo_t retry_bo = {
        .retry_ms_table = retry_table,
        .retry_ms_table_count = LWS_ARRAY_SIZE(retry_table),
        .conceal_count = 3,            // Number of retries before giving up
        .secs_since_valid_ping = 29,   // Issue a PING after 30 seconds of idle time
        .secs_since_valid_hangup = 60, // Hang up the connection if no response after 60 seconds
        .jitter_percent = 5            // Add 5% random jitter to avoid synchronized retries
    };

    NostrConnection *conn = calloc(1, sizeof(NostrConnection));
    NostrConnectionPrivate *priv = calloc(1, sizeof(NostrConnectionPrivate));
    if (!conn || !priv)
        return NULL;
    conn->priv = priv;
    conn->priv->enable_compression = 0;
    nsync_mu_init(&conn->priv->mutex);

    // Check for test mode: bypass real network and event loop
    const char *test_env = getenv("NOSTR_TEST_MODE");
    int test_mode = (test_env && *test_env && strcmp(test_env, "0") != 0) ? 1 : 0;

    if (test_mode) {
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
    // Generous fd limit per thread since we share one context across connections
    context_info.fd_limit_per_thread = 4096;
    context_info.pt_serv_buf_size = 32 * 1024;

    /* Acquire or create shared context */
    pthread_mutex_lock(&g_lws_mutex);
    if (!g_lws_context) {
        g_lws_context = lws_create_context(&context_info);
        if (!g_lws_context) {
            pthread_mutex_unlock(&g_lws_mutex);
            free(conn);
            free(priv);
            return NULL;
        }
        g_lws_running = 1;
        g_lws_refcount = 0; /* will increment below */
        pthread_create(&g_lws_service_thread, NULL, lws_service_loop, NULL);
    }
    g_lws_refcount++;
    pthread_mutex_unlock(&g_lws_mutex);

    // Initialize client connect info
    memset(&connect_info, 0, sizeof(connect_info));
    connect_info.context = g_lws_context;
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
        /* Release shared context ref and possibly shut it down */
        pthread_mutex_lock(&g_lws_mutex);
        if (g_lws_refcount > 0) g_lws_refcount--;
        int should_stop = (g_lws_refcount == 0);
        pthread_mutex_unlock(&g_lws_mutex);
        if (should_stop) {
            pthread_mutex_lock(&g_lws_mutex);
            g_lws_running = 0;
            if (g_lws_context) lws_cancel_service(g_lws_context);
            pthread_mutex_unlock(&g_lws_mutex);
            pthread_join(g_lws_service_thread, NULL);
            pthread_mutex_lock(&g_lws_mutex);
            if (g_lws_context) { lws_context_destroy(g_lws_context); g_lws_context = NULL; }
            pthread_mutex_unlock(&g_lws_mutex);
        }
        free(priv);
        free(conn);
        return NULL;
    }
    // Attach our Connection* so callbacks can retrieve it safely
    lws_set_opaque_user_data(conn->priv->wsi, conn);
    conn->recv_channel = go_channel_create(16);
    conn->send_channel = go_channel_create(16);
    // Initialize token buckets for ingress limits (runtime configurable)
    tb_init(&conn->priv->tb_bytes, (double)nostr_limit_max_bytes_per_sec(), (double)nostr_limit_max_bytes_per_sec());
    tb_init(&conn->priv->tb_frames, (double)nostr_limit_max_frames_per_sec(), (double)nostr_limit_max_frames_per_sec());
    return conn;
}

// Coroutine for processing incoming WebSocket messages
void *websocket_receive_coroutine(void *arg) {
    NostrConnection *conn = (NostrConnection *)arg;
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
    NostrConnection *conn = (NostrConnection *)arg;
    while (1) {
        WebSocketMessage *msg;
        if (go_channel_receive(conn->send_channel, (void **)&msg) == 0) {
            lws_callback_on_writable(conn->priv->wsi); // Signal that there is data to send
        }
    }
}

void nostr_connection_close(NostrConnection *conn) {
    if (!conn) return;
    // Ownership note:
    // - connection_close() closes channels to wake waiters but MUST NOT free them.
    // - nostr_relay_close() is responsible for freeing conn->recv_channel/send_channel
    //   after all workers have exited (see relay.c) to prevent use-after-free.
    // Close channels to unblock any waiters; do not free here to avoid UAF.
    if (conn->recv_channel) go_channel_close(conn->recv_channel);
    if (conn->send_channel) go_channel_close(conn->send_channel);

    if (conn->priv && conn->priv->test_mode) {
        // No libwebsockets context or thread in test mode
        free(conn->priv);
    } else if (conn->priv) {
        /* Drop our ref to the shared context; stop/destroy if last */
        pthread_mutex_lock(&g_lws_mutex);
        if (g_lws_refcount > 0) g_lws_refcount--;
        int should_stop = (g_lws_refcount == 0);
        pthread_mutex_unlock(&g_lws_mutex);
        if (should_stop) {
            pthread_mutex_lock(&g_lws_mutex);
            g_lws_running = 0;
            if (g_lws_context) lws_cancel_service(g_lws_context);
            pthread_mutex_unlock(&g_lws_mutex);
            pthread_join(g_lws_service_thread, NULL);
            pthread_mutex_lock(&g_lws_mutex);
            if (g_lws_context) { lws_context_destroy(g_lws_context); g_lws_context = NULL; }
            pthread_mutex_unlock(&g_lws_mutex);
        }
        free(conn->priv);
    }

    // Do not free channels here; the owner (relay) will free them after worker threads exit.
    free(conn);
}

void nostr_connection_write_message(NostrConnection *conn, GoContext *ctx, char *message, Error **err) {
    (void)ctx; // currently unused here; selection handled at higher level
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
    // Metrics: count enqueued bytes/messages for TX
    nostr_metric_counter_add("ws_tx_enqueued_bytes", (uint64_t)message_length);
    nostr_metric_counter_add("ws_tx_enqueued", 1);

    // Ensure that the socket becomes writable
    lws_callback_on_writable(conn->priv->wsi);
}

void nostr_connection_read_message(NostrConnection *conn, GoContext *ctx, char *buffer, size_t buffer_size, Error **err) {
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
            (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = conn->recv_channel, .value = NULL, .recv_buf = (void **)&msg },
            (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = ctx->done, .value = NULL, .recv_buf = NULL },
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

        // Metrics: count dequeued RX bytes/messages delivered to caller
        nostr_metric_counter_add("ws_rx_dequeued_bytes", (uint64_t)msg->length);
        nostr_metric_counter_add("ws_rx_dequeued_messages", 1);

        // Free message memory
        free(msg->data);
        free(msg);
    }
}
