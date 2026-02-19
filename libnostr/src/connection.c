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
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/ssl.h>

/* nostrc-8zpc: Increased from 1024 to 128KB. With 1024, lws fragments any
 * Nostr message >1KB into multiple LWS_CALLBACK_CLIENT_RECEIVE calls.
 * Most Nostr events (kind:0 profiles, kind:1 notes with signatures) are
 * 1-4KB, causing systematic fragmentation that the old code didn't handle. */
#define MAX_PAYLOAD_SIZE (128 * 1024)
#define MAX_HEADER_SIZE 1024

static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len) {
    (void)user; // not used; use opaque user data API
    NostrConnection *conn = (NostrConnection *)lws_get_opaque_user_data(wsi);

    /* nostrc-uaf: CRITICAL - Check conn validity BEFORE switch to prevent
     * heap-use-after-free. During connection close, nostr_connection_close()
     * calls lws_set_opaque_user_data(wsi, NULL) to detach, but callbacks
     * can still fire in the race window before LWS processes the detachment.
     * Without this check, we dereference freed memory at line 139. */
    if (!conn) {
        return 0; /* Connection detached, ignore callback */
    }

    switch (reason) {
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        /* nostrc-priv-race: CRITICAL - Capture conn->priv pointer ONCE at the start.
         * Another thread may call nostr_connection_close() which frees conn->priv
         * while this callback is still running. By capturing the pointer locally,
         * we ensure consistent access throughout the callback. If priv becomes
         * invalid mid-callback, we're accessing freed memory, but at least we
         * won't crash from a NULL deref after the initial check. */
        NostrConnectionPrivate *priv = conn->priv;
        if (!priv) break;
        // Enforce hard frame cap (check total accumulated size for fragmented messages)
        size_t total_len = priv->rx_reassembly_len + len;
        if (total_len > (size_t)nostr_limit_max_frame_len()) {
            nostr_rl_log(NLOG_WARN, "ws", "drop: frame too large (%zu > %lld)", total_len, (long long)nostr_limit_max_frame_len());
            // Discard partial reassembly
            priv->rx_reassembly_len = 0;
            lws_close_reason(wsi, LWS_CLOSE_STATUS_MESSAGE_TOO_LARGE, NULL, 0);
            return -1;
        }
        // Token-bucket admission: frames/sec and bytes/sec
        // DISABLED for clients - rate limiting is more useful for relay servers protecting
        // against malicious clients. Clients receiving from relays don't need this protection.
        // Can be re-enabled via NOSTR_CLIENT_RATE_LIMIT=1 environment variable.
        static int rate_limit_enabled = -1;
        if (rate_limit_enabled < 0) {
            const char *env = getenv("NOSTR_CLIENT_RATE_LIMIT");
            rate_limit_enabled = (env && env[0] == '1') ? 1 : 0;
        }
        if (rate_limit_enabled) {
            if (!tb_allow(&priv->tb_frames, 1.0) || !tb_allow(&priv->tb_bytes, (double)len)) {
                nostr_rl_log(NLOG_DEBUG, "ws", "drop: rate limit exceeded (len=%zu)", len);
                return 0;
            }
        }
        // Update RX timing/progress
        uint64_t now_us = (uint64_t)lws_now_usecs();
        priv->last_rx_ns = now_us; /* store usec */
        if (priv->rx_window_start_ns == 0) {
            priv->rx_window_start_ns = now_us;
            priv->rx_window_bytes = 0;
        }
        priv->rx_window_bytes += (uint64_t)len;

        /* nostrc-8zpc: Proper WebSocket frame reassembly.
         * LWS delivers data in chunks up to rx_buffer_size. When a WebSocket
         * message exceeds this, we get multiple callbacks. We must accumulate
         * fragments and only queue the complete message to recv_channel. */
        int is_final = lws_is_final_fragment(wsi);
        size_t remaining = lws_remaining_packet_payload(wsi);

        if (is_final && priv->rx_reassembly_len == 0) {
            /* Fast path: complete message in a single callback (common case
             * now that rx_buffer_size is 128KB). No reassembly needed. */
            goto queue_message;
        }

        /* Accumulate fragment into reassembly buffer */
        size_t needed = priv->rx_reassembly_len + len + 1;
        if (needed > priv->rx_reassembly_alloc) {
            size_t new_alloc = priv->rx_reassembly_alloc;
            if (new_alloc == 0) new_alloc = 4096;
            while (new_alloc < needed) new_alloc *= 2;
            char *new_buf = (char *)realloc(priv->rx_reassembly_buf, new_alloc);
            if (!new_buf) {
                nostr_rl_log(NLOG_WARN, "ws", "drop: reassembly OOM (%zu bytes)", new_alloc);
                priv->rx_reassembly_len = 0;
                return 0;
            }
            priv->rx_reassembly_buf = new_buf;
            priv->rx_reassembly_alloc = new_alloc;
        }
        memcpy(priv->rx_reassembly_buf + priv->rx_reassembly_len, in, len);
        priv->rx_reassembly_len += len;

        if (!is_final || remaining > 0) {
            /* More fragments coming - wait for complete message */
            return 0;
        }

        /* Reassembly complete - use the reassembled buffer as the message */
        priv->rx_reassembly_buf[priv->rx_reassembly_len] = '\0';
        in = priv->rx_reassembly_buf;
        len = priv->rx_reassembly_len;

queue_message:
        // Check if channel is still valid (it may have been freed during shutdown)
        // Validate both pointer and magic number to detect use-after-free
        {
            GoChannel *recv_chan = conn->recv_channel;
            if (!recv_chan || recv_chan->magic != GO_CHANNEL_MAGIC) {
                priv->rx_reassembly_len = 0;
                return 0;
            }
        }
        // Allocate a copy buffer and queue as a WebSocketMessage
        WebSocketMessage *msg = (WebSocketMessage*)malloc(sizeof(WebSocketMessage));
        if (!msg) { priv->rx_reassembly_len = 0; return -1; }
        msg->length = len;
        msg->data = (char*)malloc(len + 1);
        if (!msg->data) { free(msg); priv->rx_reassembly_len = 0; return -1; }
        memcpy(msg->data, in, len);
        msg->data[len] = '\0';
        // Reset reassembly state
        priv->rx_reassembly_len = 0;
        // Capture channel pointer once for thread-safe access
        GoChannel *recv_chan = conn->recv_channel;
        // Non-blocking send: the lws service thread is shared across ALL
        // connections. A blocking send here would freeze the entire app if
        // the consumer (message_loop) falls behind. (nostrc-j6h1)
        if (go_channel_try_send(recv_chan, msg) != 0) {
            // Channel full — retry a few times with brief yields before dropping.
            // With proper reassembly, channel pressure is much lower since we
            // queue 1 complete message instead of N fragments. (nostrc-8zpc)
            int retries = 10;
            while (retries-- > 0) {
                sched_yield();
                /* nostrc-uaf: Do NOT re-read conn->recv_channel here - conn may have
                 * been freed during the retry loop. Just validate the channel pointer
                 * we already captured at line 136. If the channel was closed, the
                 * magic check will fail and we'll bail out safely. */
                if (!recv_chan || recv_chan->magic != GO_CHANNEL_MAGIC) {
                    free(msg->data);
                    free(msg);
                    return 0;
                }
                if (go_channel_try_send(recv_chan, msg) == 0) {
                    goto send_ok;
                }
            }
            nostr_metric_counter_add("ws_rx_drop_full", 1);
            nostr_rl_log(NLOG_WARN, "ws", "drop: recv_channel full after retries (len=%zu)", len);
            free(msg->data);
            free(msg);
            break;
        }
send_ok:
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
            nsync_mu_lock(&conn->priv->mutex);
            conn->priv->established = 1;  /* Mark handshake complete */
            uint64_t now_us = (uint64_t)lws_now_usecs();
            conn->priv->last_rx_ns = now_us;
            conn->priv->rx_window_start_ns = now_us;
            conn->priv->rx_window_bytes = 0;
            nsync_mu_unlock(&conn->priv->mutex);
        }
        // Arm a 1s timer for periodic timeout/progress checks
        lws_set_timer_usecs(wsi, 1000000);
        lws_callback_on_writable(wsi); // Ensure the socket becomes writable
        break;
    
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
        if (conn && conn->priv) {
            nsync_mu_lock(&conn->priv->mutex);
            if (conn->priv->wsi == wsi && conn->priv->writable_pending) {
                conn->priv->writable_pending = 0;
                nsync_mu_unlock(&conn->priv->mutex);
                lws_callback_on_writable(wsi);
            } else {
                nsync_mu_unlock(&conn->priv->mutex);
            }
        }
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        // Pop a message from the send_channel and send it over the WebSocket (non-blocking)
        if (!conn || !conn->priv || !conn->priv->wsi || !conn->send_channel) break;
        
        // Double-check connection is still valid after channel access
        nsync_mu_lock(&conn->priv->mutex);
        struct lws *wsi_check = conn->priv->wsi;
        nsync_mu_unlock(&conn->priv->mutex);
        if (!wsi_check || wsi_check != wsi) {
            // Connection has been closed or changed
            break;
        }
        WebSocketMessage *msg = NULL;
        if (go_channel_try_receive(conn->send_channel, (void **)&msg) == 0 && msg) {
            // Validate message structure before use
            if (!msg->data || msg->length == 0 || msg->length > 1024*1024) {
                fprintf(stderr, "Invalid message: data=%p length=%zu\n", (void*)msg->data, msg->length);
                if (msg->data) free(msg->data);
                free(msg);
                return -1;
            }
            
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
        if (conn && conn->priv) {
            lws_set_timer_usecs(wsi, 0);
            nsync_mu_lock(&conn->priv->mutex);
            conn->priv->wsi = NULL;
            conn->priv->writable_pending = 0;
            conn->priv->established = 0;  /* Mark handshake as incomplete */
            /* Reset reassembly state to prevent stale partial data from
             * being prepended to the first message on reconnect. */
            conn->priv->rx_reassembly_len = 0;
            nsync_mu_unlock(&conn->priv->mutex);
        }
        break;
    case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
#if defined(LWS_CALLBACK_OPENSSL_CTX_LOAD_EXTRA_CLIENT_VERIFY_CERTS) && \
    (LWS_CALLBACK_OPENSSL_CTX_LOAD_EXTRA_CLIENT_VERIFY_CERTS != LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS)
    case LWS_CALLBACK_OPENSSL_CTX_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
#endif
    {
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
        int should_process = 0;
        nsync_mu_lock(&conn->priv->mutex);
        if (conn->priv->wsi == wsi) should_process = 1;
        nsync_mu_unlock(&conn->priv->mutex);
        if (!should_process) break;
        uint64_t now_us = (uint64_t)lws_now_usecs();
        int64_t read_to_s = nostr_limit_ws_read_timeout_seconds();
        if (read_to_s > 0) {
            uint64_t last_us = conn->priv->last_rx_ns ? conn->priv->last_rx_ns : now_us;
            if (now_us - last_us > (uint64_t)read_to_s * 1000000ULL) {
                nostr_rl_log(NLOG_WARN, "ws", "read timeout: no data for %llds", (long long)read_to_s);
                nostr_metric_counter_add("ws_timeout_read", 1);
                // Avoid closing from timer callback; just log and continue.
                // The upper layers can decide to reconnect.
                return 0;
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
                    // Avoid closing from timer callback; just log and continue.
                    // The upper layers can decide to reconnect.
                    return 0;
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

/* Deferred cleanup queue for conn->priv structs that can't be freed immediately
 * because the service thread is still running. Queue is processed when the
 * service thread stops (g_lws_refcount hits 0). */
typedef struct DeferredPriv {
    NostrConnectionPrivate *priv;
    struct DeferredPriv *next;
} DeferredPriv;
static DeferredPriv *g_deferred_cleanup_head = NULL;

/* hq-5ejm4: Connection request queue — moves lws_client_connect_via_info()
 * off caller threads and onto the LWS service thread.  Callers enqueue a
 * request and block on a per-request result channel.  The service loop drains
 * the queue between lws_service() iterations. */
typedef struct {
    char host[256];
    char path[512];
    int port;
    int use_ssl;
    NostrConnection *conn;   /* pre-allocated by caller */
    GoChannel *result;        /* capacity-1 channel: carries (intptr_t)1 ok, 0 fail */
} ConnectionRequest;

static GoChannel *g_conn_request_queue = NULL; /* capacity 32, created under g_lws_mutex */

/* Must hold g_lws_mutex when calling */
static void deferred_cleanup_add(NostrConnectionPrivate *priv) {
    DeferredPriv *node = malloc(sizeof(DeferredPriv));
    if (!node) return; /* leak on OOM, but better than crash */
    node->priv = priv;
    node->next = g_deferred_cleanup_head;
    g_deferred_cleanup_head = node;
}

/* Must hold g_lws_mutex when calling */
static void deferred_cleanup_process(void) {
    DeferredPriv *node = g_deferred_cleanup_head;
    while (node) {
        DeferredPriv *next = node->next;
        if (node->priv) {
            free(node->priv->rx_reassembly_buf);
            free(node->priv);
        }
        free(node);
        node = next;
    }
    g_deferred_cleanup_head = NULL;
}

/* hq-5ejm4: Process a connection request on the LWS service thread.
 * Called WITHOUT g_lws_mutex held — safe to do synchronous DNS inside
 * lws_client_connect_via_info() without blocking other mutex users. */
static void service_loop_process_connect_request(ConnectionRequest *req,
                                                  struct lws_context *ctx) {
    if (!req || !req->conn || !req->conn->priv) {
        if (req && req->result) {
            go_channel_send(req->result, (void *)(intptr_t)0);
        }
        return;
    }

    /* Persist connect parameters on the connection private state so pointers
     * passed into libwebsockets outlive the transient ConnectionRequest. */
    snprintf(req->conn->priv->connect_host, sizeof(req->conn->priv->connect_host), "%s", req->host);
    snprintf(req->conn->priv->connect_path, sizeof(req->conn->priv->connect_path), "%s", req->path);
    req->conn->priv->connect_port = req->port;
    req->conn->priv->connect_use_ssl = req->use_ssl;

    struct lws_client_connect_info ci;
    memset(&ci, 0, sizeof(ci));
    ci.context = ctx;
    ci.address = req->conn->priv->connect_host;
    ci.port = req->conn->priv->connect_port;
    ci.path = req->conn->priv->connect_path;
    ci.host = req->conn->priv->connect_host;
    ci.origin = req->conn->priv->connect_host;
    ci.ssl_connection = req->conn->priv->connect_use_ssl ? LCCSCF_USE_SSL : 0;
    ci.protocol = "wss";
    ci.pwsi = &req->conn->priv->wsi;
    ci.userdata = req->conn;

    struct lws *wsi = lws_client_connect_via_info(&ci);

    intptr_t ok = (wsi != NULL) ? 1 : 0;
    if (wsi) {
        lws_set_opaque_user_data(wsi, req->conn);
        tb_init(&req->conn->priv->tb_bytes,
                (double)nostr_limit_max_bytes_per_sec(),
                (double)nostr_limit_max_bytes_per_sec());
        tb_init(&req->conn->priv->tb_frames,
                (double)nostr_limit_max_frames_per_sec(),
                (double)nostr_limit_max_frames_per_sec());
    }

    /* Signal the blocked caller */
    go_channel_send(req->result, (void *)ok);
    /* Caller owns req and will free it + the result channel */
}

static void *lws_service_loop(void *arg) {
    (void)arg;
    for (;;) {
        // Read context and running flag under mutex, but DO NOT hold the mutex
        // while calling into libwebsockets service loop.
        pthread_mutex_lock(&g_lws_mutex);
        int running = g_lws_running;
        struct lws_context *ctx = g_lws_context;
        GoChannel *queue = g_conn_request_queue;
        pthread_mutex_unlock(&g_lws_mutex);

        if (!running || !ctx) {
            break;
        }

        /* nostrc-snap: Process AT MOST ONE connection request per iteration.
         *
         * lws_client_connect_via_info() does SYNCHRONOUS DNS resolution.
         * The old code drained ALL pending requests in a tight while loop,
         * blocking for seconds per request.  With query_thread_func creating
         * temp relays for disconnected relays, 5-10 connection requests
         * easily queue up, blocking the service loop for 10-50 seconds.
         *
         * During that time, lws_service() is NEVER called:
         *   - No LWS_CALLBACK_CLIENT_RECEIVE → no incoming events
         *   - No LWS_CALLBACK_CLIENT_WRITEABLE → send_channel fills up →
         *     write_operations blocks → write_queue fills up →
         *     nostr_relay_write() blocks anything writing from main thread
         *     → UI freezes
         *
         * Fix: process one request, then call lws_service() to keep
         * existing connections alive.  Pending requests are processed
         * in subsequent iterations (~50ms apart). */
        if (queue) {
            ConnectionRequest *req = NULL;
            if (go_channel_try_receive(queue, (void **)&req) == 0 && req) {
                service_loop_process_connect_request(req, ctx);
            }
        }

        // Service events without holding our mutex to avoid deadlocks with
        // threads calling lws_cancel_service() or other lws APIs.
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

    lws_set_log_level(LLL_USER | LLL_ERR | LLL_HEADER | LLL_CLIENT , NULL);

    /* Phase 1: Alloc + parse (no lock) */
    NostrConnection *conn = calloc(1, sizeof(NostrConnection));
    NostrConnectionPrivate *priv = calloc(1, sizeof(NostrConnectionPrivate));
    if (!conn || !priv) {
        free(conn);
        free(priv);
        return NULL;
    }
    conn->priv = priv;
    conn->priv->enable_compression = 0;
    nsync_mu_init(&conn->priv->mutex);

    // Check for test mode: bypass real network and event loop
    const char *test_env = getenv("NOSTR_TEST_MODE");
    int test_mode = (test_env && *test_env && strcmp(test_env, "0") != 0) ? 1 : 0;

    if (test_mode) {
        fprintf(stderr, "[nostr] NOSTR_TEST_MODE=1: offline mode enabled (no network I/O)\n");
        conn->priv->test_mode = 1;
        conn->recv_channel = go_channel_create(256);
        conn->send_channel = go_channel_create(16);
        // No context / thread in test mode
        return conn;
    }

    /* Create channels BEFORE enqueue — websocket_callback may fire
     * immediately after lws_client_connect_via_info returns on the
     * service thread, so recv_channel must exist. */
    /* nostrc-kw9r: Bump recv_channel from 256 → 2048.
     * At startup relays burst hundreds of stored events.  The LWS service
     * thread is a singleton shared by ALL connections — when even one
     * recv_channel is full the retry/drop loop blocks every connection.
     * 2048 pointers ≈ 16 KB, trivial memory for 8× headroom. */
    conn->recv_channel = go_channel_create(2048);
    conn->send_channel = go_channel_create(16);

    /* Phase 2: Ensure shared context (fast mutex — microseconds, not seconds) */
    lws_retry_bo_t retry_bo = {
        .retry_ms_table = retry_table,
        .retry_ms_table_count = LWS_ARRAY_SIZE(retry_table),
        .conceal_count = 3,
        .secs_since_valid_ping = 29,
        .secs_since_valid_hangup = 60,
        .jitter_percent = 5
    };

    struct lws_context_creation_info context_info;
    memset(&context_info, 0, sizeof(context_info));
    context_info.retry_and_idle_policy = &retry_bo;
    context_info.port = CONTEXT_PORT_NO_LISTEN;
    context_info.protocols = protocols;
    context_info.gid = -1;
    context_info.uid = -1;
    context_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
                           LWS_SERVER_OPTION_VALIDATE_UTF8;
    context_info.fd_limit_per_thread = 4096;
    context_info.pt_serv_buf_size = 32 * 1024;

    pthread_mutex_lock(&g_lws_mutex);
    if (!g_lws_context) {
        g_lws_context = lws_create_context(&context_info);
        if (!g_lws_context) {
            pthread_mutex_unlock(&g_lws_mutex);
            go_channel_close(conn->recv_channel);
            go_channel_free(conn->recv_channel);
            go_channel_close(conn->send_channel);
            go_channel_free(conn->send_channel);
            free(priv);
            free(conn);
            return NULL;
        }
        g_lws_running = 1;
        g_lws_refcount = 0; /* will increment below */
        pthread_create(&g_lws_service_thread, NULL, lws_service_loop, NULL);
    }
    if (!g_conn_request_queue) {
        g_conn_request_queue = go_channel_create(32);
    }
    g_lws_refcount++;
    pthread_mutex_unlock(&g_lws_mutex);

    /* Phase 3: Enqueue connection request + block on result.
     * The service thread calls lws_client_connect_via_info() — which may do
     * synchronous DNS — without holding g_lws_mutex, so other connections
     * and the service loop remain unblocked. */
    ConnectionRequest *req = calloc(1, sizeof(ConnectionRequest));
    if (!req) goto fail_decref;
    parse_ws_url(url, &req->use_ssl, req->host, sizeof req->host,
                 &req->port, req->path, sizeof req->path);
    req->conn = conn;
    req->result = go_channel_create(1);
    if (!req->result) { free(req); goto fail_decref; }

    if (go_channel_send(g_conn_request_queue, req) != 0) {
        /* Queue closed (shutting down) */
        go_channel_close(req->result);
        go_channel_free(req->result);
        free(req);
        goto fail_decref;
    }

    /* Wake the service thread so it drains the queue promptly */
    pthread_mutex_lock(&g_lws_mutex);
    if (g_lws_context) lws_cancel_service(g_lws_context);
    pthread_mutex_unlock(&g_lws_mutex);

    /* Block until the LWS service thread completes the handshake.
     * The underlying causes of service-thread stalls (recv_channel overflow,
     * connection duplication) are now fixed:
     *   - recv_channel 2048 (nostrc-kw9r, 822c4a8f)
     *   - shared relay registry eliminates duplicate connections (4569e9f1)
     * so this should never hang in practice. */
    void *result_val = NULL;
    go_channel_receive(req->result, &result_val);
    intptr_t ok = (intptr_t)result_val;

    go_channel_close(req->result);
    go_channel_free(req->result);
    free(req);

    if (!ok) goto fail_decref;

    return conn;

fail_decref:
    /* Decrement refcount; destroy context if last.
     * Same combined critical section as nostr_connection_close(). */
    {
        struct lws_context *ctx_to_destroy = NULL;
        GoChannel *queue_to_drain = NULL;
        pthread_mutex_lock(&g_lws_mutex);
        if (g_lws_refcount > 0) g_lws_refcount--;
        if (g_lws_refcount == 0 && g_lws_context) {
            ctx_to_destroy = g_lws_context;
            g_lws_context = NULL;
            g_lws_running = 0;
            if (g_conn_request_queue) {
                go_channel_close(g_conn_request_queue);
                queue_to_drain = g_conn_request_queue;
                g_conn_request_queue = NULL;
            }
        }
        pthread_mutex_unlock(&g_lws_mutex);
        if (ctx_to_destroy) {
            lws_cancel_service(ctx_to_destroy);
            pthread_join(g_lws_service_thread, NULL);
            if (queue_to_drain) {
                ConnectionRequest *pend = NULL;
                while (go_channel_try_receive(queue_to_drain, (void **)&pend) == 0 && pend) {
                    go_channel_send(pend->result, (void *)(intptr_t)0);
                    pend = NULL;
                }
                go_channel_free(queue_to_drain);
            }
            lws_context_destroy(ctx_to_destroy);
        }
    }
    if (conn->recv_channel) { go_channel_close(conn->recv_channel); go_channel_free(conn->recv_channel); }
    if (conn->send_channel) { go_channel_close(conn->send_channel); go_channel_free(conn->send_channel); }
    free(priv);
    free(conn);
    return NULL;
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
        free(conn->priv->rx_reassembly_buf);
        free(conn->priv);
    } else if (conn->priv) {
        /* Detach the connection from WSI to prevent callbacks from accessing freed memory.
         * We cannot safely call lws_close_reason() from this thread - that must be done
         * from the service thread. Instead, we just detach and let the WSI close naturally. */
        pthread_mutex_lock(&g_lws_mutex);
        if (conn->priv->wsi) {
            lws_set_opaque_user_data(conn->priv->wsi, NULL);
            /* Wake up service thread to process the detachment */
            if (g_lws_context) {
                lws_cancel_service(g_lws_context);
            }
            conn->priv->wsi = NULL;
        }
        pthread_mutex_unlock(&g_lws_mutex);
        
        /* Drop our ref to the shared context; stop/destroy if last.
         * hq-5ejm4 fix (boris review): Queue close/drain and context
         * teardown MUST happen in the SAME critical section to prevent:
         *  - TOCTOU: another thread recreating context+queue between unlock/relock
         *  - UAF: service loop using stale queue pointer after free */
        struct lws_context *ctx_to_destroy = NULL;
        GoChannel *queue_to_drain = NULL;
        int should_free_priv = 0;
        pthread_mutex_lock(&g_lws_mutex);
        if (g_lws_refcount > 0) g_lws_refcount--;
        if (g_lws_refcount == 0 && g_lws_context) {
            ctx_to_destroy = g_lws_context;
            g_lws_context = NULL;
            g_lws_running = 0;  /* Service loop will exit on next iteration */
            should_free_priv = 1;
            /* Close the queue to unblock any callers stuck in go_channel_send,
             * then take ownership for post-join drain+free. */
            if (g_conn_request_queue) {
                go_channel_close(g_conn_request_queue);
                queue_to_drain = g_conn_request_queue;
                g_conn_request_queue = NULL;
            }
        }
        pthread_mutex_unlock(&g_lws_mutex);
        if (ctx_to_destroy) {
            lws_cancel_service(ctx_to_destroy);
            pthread_join(g_lws_service_thread, NULL);
            /* Service thread is stopped — safe to drain + free the queue.
             * No other thread can access queue_to_drain since we NULLed
             * g_conn_request_queue under the mutex. */
            if (queue_to_drain) {
                ConnectionRequest *pend = NULL;
                while (go_channel_try_receive(queue_to_drain, (void **)&pend) == 0 && pend) {
                    go_channel_send(pend->result, (void *)(intptr_t)0);
                    pend = NULL;
                }
                go_channel_free(queue_to_drain);
            }
            lws_context_destroy(ctx_to_destroy);
            /* Process deferred cleanup queue now that service thread is stopped */
            pthread_mutex_lock(&g_lws_mutex);
            deferred_cleanup_process();
            pthread_mutex_unlock(&g_lws_mutex);
        }
        /* Free conn->priv: either immediately if service stopped, or defer until later */
        if (should_free_priv) {
            free(conn->priv->rx_reassembly_buf);
            free(conn->priv);
        } else if (conn->priv) {
            /* Can't free now - service thread may still reference via WSI callbacks.
             * Add to deferred cleanup queue, will be processed when service stops. */
            pthread_mutex_lock(&g_lws_mutex);
            deferred_cleanup_add(conn->priv);
            pthread_mutex_unlock(&g_lws_mutex);
        }
    }

    // Do not free channels here; the owner (relay) will free them after worker threads exit.
    free(conn);
}

void nostr_connection_write_message(NostrConnection *conn, GoContext *ctx, char *message, Error **err) {
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

    if (!conn->priv) {
        if (err) {
            *err = new_error(1, "connection state unavailable");
        }
        return;
    }

    if (!conn->send_channel) {
        if (err) {
            *err = new_error(1, "send channel unavailable");
        }
        return;
    }

    nsync_mu_lock(&conn->priv->mutex);
    struct lws *wsi = conn->priv->wsi;
    nsync_mu_unlock(&conn->priv->mutex);
    if (!wsi) {
        if (err) {
            *err = new_error(1, "connection not established or already closed");
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

    /* nostrc-ws1: Use context-aware send so that relay shutdown (context
     * cancellation) can unblock a worker stuck waiting for space in
     * send_channel.  Previously ctx was ignored ((void)ctx), so
     * write_operations could block forever in go_channel_send() while
     * relay_free_impl waited for the worker → main-thread deadlock. */
    if (go_channel_send_with_context(conn->send_channel, msg, ctx) != 0) {
        free(msg->data);
        free(msg);
        if (err) {
            *err = new_error(1, "failed to enqueue message (channel closed or context canceled)");
        }
        return;
    }
    // Metrics: count enqueued bytes/messages for TX
    nostr_metric_counter_add("ws_tx_enqueued_bytes", (uint64_t)message_length);
    nostr_metric_counter_add("ws_tx_enqueued", 1);

    nsync_mu_lock(&conn->priv->mutex);
    struct lws *wsi_again = conn->priv->wsi;
    if (wsi_again) {
        conn->priv->writable_pending = 1;
    }
    nsync_mu_unlock(&conn->priv->mutex);
    if (!wsi_again) {
        *err = new_error(1, "connection closed before write could schedule");
        return;
    }

    pthread_mutex_lock(&g_lws_mutex);
    struct lws_context *lws_ctx = g_lws_context;
    if (lws_ctx) lws_cancel_service(lws_ctx);
    pthread_mutex_unlock(&g_lws_mutex);

    if (err) *err = NULL;
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

    // Validate recv_channel before use to prevent use-after-free during shutdown/reconnect
    GoChannel *recv_chan = conn->recv_channel;
    if (!recv_chan || recv_chan->magic != GO_CHANNEL_MAGIC) {
        if (err) *err = new_error(1, "recv channel invalid or closed");
        return;
    }

    // Also validate ctx->done channel if context is provided
    GoChannel *done_chan = ctx ? ctx->done : NULL;
    if (ctx && (!done_chan || done_chan->magic != GO_CHANNEL_MAGIC)) {
        if (err) *err = new_error(1, "context done channel invalid");
        return;
    }

    // Wait on either an incoming message or context cancellation
    // nostrc-b0h-revert: Use polling instead of go_select due to race condition
    // where websocket messages arrive before select waiters are registered.
    WebSocketMessage *msg = NULL;
    if (ctx) {
        // Poll both channels with 1ms backoff
        for (;;) {
            // Check if context is canceled
            if (go_context_is_canceled(ctx)) {
                if (err) *err = new_error(1, "Context canceled");
                return;
            }
            // Try to receive a message
            if (go_channel_try_receive(recv_chan, (void **)&msg) == 0) {
                break; // Got a message
            }
            // Check if channel is closed
            if (go_channel_is_closed(recv_chan)) {
                if (err) *err = new_error(1, "Receive channel closed");
                return;
            }
            // Brief sleep to avoid busy-waiting
            usleep(1000); // 1ms
        }
        if (msg == NULL) {
            if (err) *err = new_error(1, "Receive failed or channel closed");
            return;
        }
    } else {
        if (go_channel_receive(recv_chan, (void **)&msg) != 0 || !msg) {
            if (err) *err = new_error(1, "Failed to receive message or channel closed");
            return;
        }
    }

    if (msg) {
        // Validate message structure before use
        if (!msg->data && msg->length > 0) {
            // Corrupted message: length > 0 but data is NULL
            if (err) *err = new_error(1, "Corrupted message: data is NULL");
            free(msg);
            return;
        }

        if (msg->length > 0 && msg->data) {
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
                if (msg->data) free(msg->data);
                free(msg);
                return;
            }
        }

        // Metrics: count dequeued RX bytes/messages delivered to caller
        nostr_metric_counter_add("ws_rx_dequeued_bytes", (uint64_t)msg->length);
        nostr_metric_counter_add("ws_rx_dequeued_messages", 1);

        // Free message memory
        if (msg->data) free(msg->data);
        free(msg);
    }
}
