#ifndef CONNECTION_PRIVATE_H
#define CONNECTION_PRIVATE_H

#include "../include/rate_limiter.h"
#include "nsync.h"
#include <libwebsockets.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

struct _NostrConnectionPrivate {
    /* nostrc-priv-refcount: Atomic refcount for lifetime correctness.
     * Callbacks must acquire a ref before accessing priv fields.
     * When closing is set, priv_try_ref() fails and no new refs can be taken.
     * The priv struct is freed when refs drops to 0 AND closing is set. */
    atomic_int refs;
    atomic_int closing;
    struct lws *wsi;
    int enable_compression;
    struct lws_context *context;
    nsync_mu mutex;
    pthread_t service_thread;
    int running;
    int test_mode; // when set, no real network; helpers short-circuit
    /* Ingress rate limiting */
    nostr_token_bucket tb_bytes;
    nostr_token_bucket tb_frames;
    /* Timeout & progress tracking */
    uint64_t last_rx_ns;
    uint64_t rx_window_start_ns;
    uint64_t rx_window_bytes;
    int writable_pending;
    int established; /* Set when WebSocket handshake completes (LWS_CALLBACK_CLIENT_ESTABLISHED) */
    /* WebSocket message reassembly buffer for fragmented frames (nostrc-8zpc) */
    char *rx_reassembly_buf;	/* Dynamically allocated reassembly buffer */
    size_t rx_reassembly_len;	/* Current bytes accumulated */
    size_t rx_reassembly_alloc; /* Allocated size of reassembly buffer */

    /* Persisted connect parameters (nostrc-dns-lifetime): pointers passed to
     * lws_client_connect_via_info() must outlive the transient request object. */
    char connect_host[256];
    char connect_path[512];
    int connect_port;
    int connect_use_ssl;
};

// Struct to hold WebSocket message
typedef struct WebSocketMessage {
    char *data;
    size_t length;
} WebSocketMessage;

#endif // CONNECTION_PRIVATE_H
