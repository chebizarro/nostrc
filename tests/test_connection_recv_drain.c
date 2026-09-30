/* Regression test for nostrc-lpvj: frames still queued in a connection's
 * recv_channel when its owner lets it go are freed, not leaked.
 *
 * websocket_callback copies every received frame into a malloc'd
 * WebSocketMessage and queues it on conn->recv_channel.  The relay releases
 * that channel on close (relay.c: close the channels, join the workers,
 * detach conn->recv_channel under conn->priv->mutex, nostr_connection_close,
 * release the channel).  Frames nobody had read were never freed: the
 * Groundhog sanitizer job suppressed them as leak:^websocket_callback$.
 * Releasing now goes through nostr_connection_recv_channel_free(), which
 * drains and frees them, and the callback queues only under priv->mutex to a
 * channel still attached, so none can arrive after the drain.
 *
 * The test releases a connection as relay.c does (nostr_connection_release,
 * nostrc-xfjg), around a local lws server:
 *  1. The server sends 64 frames; nothing reads them; the channel holds all
 *     64 when it is released.
 *  2. On a second connection the server floods frames without pause, and the
 *     channel is released while they keep arriving.
 * The leak itself is only visible to LeakSanitizer (Linux, ASAN with
 * detect_leaks=1: scripts/groundhog-linux-ci.sh or the groundhog-sanitizers
 * CI job); without it this checks the sequence runs cleanly.  Before the fix
 * LSan reported every queued frame, allocated in websocket_callback.
 */

#include <libwebsockets.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "go.h"
#include "nostr-connection.h"
#include "connection-private.h"

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s (%s) at %s:%d\n", msg, #cond, \
                    __FILE__, __LINE__);                                     \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define QUEUED_FRAMES 64

static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* ---- the server: sends frames up to a target ------------------------------------- */

static struct lws_context *g_srv_ctx;
static atomic_int g_srv_stop;
static struct lws *g_srv_wsi;     /* service thread only */
static atomic_long g_srv_target;  /* frames to send on the current connection */
static atomic_long g_srv_sent;
static atomic_int g_srv_connections;

static int server_cb(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in,
                     size_t len) {
    (void)user;
    (void)in;
    (void)len;
    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        g_srv_wsi = wsi;
        atomic_store(&g_srv_sent, 0);
        atomic_fetch_add(&g_srv_connections, 1);
        lws_callback_on_writable(wsi);
        break;
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
        if (g_srv_wsi) lws_callback_on_writable(g_srv_wsi);
        break;
    case LWS_CALLBACK_SERVER_WRITEABLE: {
        long sent = atomic_load(&g_srv_sent);
        if (wsi != g_srv_wsi || sent >= atomic_load(&g_srv_target)) break;
        unsigned char buf[LWS_PRE + 64];
        int n = snprintf((char *)buf + LWS_PRE, 64, "[\"NOTICE\",\"frame %ld\"]", sent);
        if (lws_write(wsi, buf + LWS_PRE, (size_t)n, LWS_WRITE_TEXT) < n) return -1;
        atomic_store(&g_srv_sent, sent + 1);
        lws_callback_on_writable(wsi);
        break;
    }
    case LWS_CALLBACK_CLOSED:
        if (wsi == g_srv_wsi) g_srv_wsi = NULL;
        break;
    default:
        break;
    }
    return 0;
}

/* The libnostr client requests subprotocol "wss"; the server must offer it. */
static struct lws_protocols g_srv_protocols[] = {
    { "wss", server_cb, 0, 4096, 0, NULL, 0 },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

static void *server_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&g_srv_stop)) lws_service(g_srv_ctx, 50);
    return NULL;
}

static void server_send(long target) {
    atomic_store(&g_srv_target, target);
    lws_cancel_service(g_srv_ctx);
}

/* ---- the client ------------------------------------------------------------------- */

static NostrConnection *open_connection(int port, int nth) {
    char url[64];
    snprintf(url, sizeof url, "ws://127.0.0.1:%d", port);
    NostrConnection *conn = nostr_connection_new(url);
    CHECK(conn, "nostr_connection_new failed");
    CHECK(nostr_connection_wait_handshake(conn, 10000) == 1, "handshake");
    double deadline = now_seconds() + 10.0;
    while (atomic_load(&g_srv_connections) < nth && now_seconds() < deadline) usleep(10000);
    CHECK(atomic_load(&g_srv_connections) == nth, "the server never saw the connection");
    return conn;
}

static size_t queued(NostrConnection *conn) {
    nsync_mu_lock(&conn->priv->mutex);
    size_t depth = conn->recv_channel ? go_channel_get_depth(conn->recv_channel) : 0;
    nsync_mu_unlock(&conn->priv->mutex);
    return depth;
}

static void wait_queued(NostrConnection *conn, size_t count) {
    double deadline = now_seconds() + 10.0;
    while (queued(conn) < count && now_seconds() < deadline) usleep(5000);
    CHECK(queued(conn) >= count, "frames never reached recv_channel");
}

/* How relay.c lets every connection go (close, free, a failed dial, a
 * reconnect: relay_retire_connection), with no reader left. */
static void release_connection(NostrConnection *conn) {
    int before = nostr_connection_unreleased_count();
    nostr_connection_release(conn);
    CHECK(nostr_connection_unreleased_count() == before - 1, "release not counted");
}

int main(void) {
    unsetenv("NOSTR_TEST_MODE"); /* the real network path */
    lws_set_log_level(LLL_ERR, NULL);

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = 0; /* kernel-chosen: safe under parallel ctest */
    info.iface = "127.0.0.1";
    info.protocols = g_srv_protocols;
    info.gid = (gid_t)-1;
    info.uid = (uid_t)-1;
    g_srv_ctx = lws_create_context(&info);
    CHECK(g_srv_ctx, "failed to create local ws server context");
    struct lws_vhost *vh = lws_get_vhost_by_name(g_srv_ctx, "default");
    CHECK(vh, "no default vhost");
    int port = lws_get_vhost_listen_port(vh);
    CHECK(port > 0, "no listen port");
    pthread_t srv;
    CHECK(pthread_create(&srv, NULL, server_thread, NULL) == 0, "server thread");

    /* 1. Frames queued, nobody reading: all of them are freed on release. */
    NostrConnection *conn = open_connection(port, 1);
    server_send(QUEUED_FRAMES);
    wait_queued(conn, QUEUED_FRAMES);
    usleep(100000);
    CHECK(queued(conn) == QUEUED_FRAMES, "more frames than the server sent");
    printf("released a connection with %d unread frames\n", QUEUED_FRAMES);
    release_connection(conn);

    /* 2. Frames still arriving while the channel is released. */
    conn = open_connection(port, 2);
    server_send(LONG_MAX);
    wait_queued(conn, 256);
    release_connection(conn);
    long before = atomic_load(&g_srv_sent);
    usleep(200000); /* frames keep coming to the released connection */
    printf("released a connection during a flood (%ld frames sent so far, %ld after)\n", before,
           atomic_load(&g_srv_sent));

    server_send(0);
    atomic_store(&g_srv_stop, 1);
    lws_cancel_service(g_srv_ctx);
    pthread_join(srv, NULL);
    lws_context_destroy(g_srv_ctx);
    printf("test_connection_recv_drain: OK\n");
    return 0;
}
