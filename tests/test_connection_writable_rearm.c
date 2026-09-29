#define _DEFAULT_SOURCE /* usleep under -std=c11 on glibc */
/* Regression test for nostrc-75rv: a frame enqueued while the lws service
 * thread is concluding "send queue drained" must still be written.
 *
 * nostr_connection_write_message() pushes the frame to send_channel and only
 * then sets writable_pending (under priv->mutex) and wakes the service loop.
 * CLIENT_WRITEABLE, on an empty try_receive, cleared writable_pending.  When
 * the writer's set landed between that empty try_receive and the clear (a
 * no-op set, the flag still being 1), the clear won: the service loop's
 * writable sweep is gated on the flag, EVENT_WAIT_CANCELLED carries no
 * connection, and the frame sat in send_channel until some later write.  In
 * test_nostr_gobject_subscription_eose_order that stranded a REQ, so its
 * round never got EVENT/EOSE (seen under concurrent ctest load).
 *
 * The same window opens deterministically when the head of send_channel is
 * claimed by a writer that has not finished publishing it: try_receive
 * reports "nothing yet".  This test builds exactly that state (a claimed,
 * unpublished send_channel ticket, with a real frame queued behind it),
 * lets the service thread run its CLIENT_WRITEABLE callbacks, then publishes
 * the claimed frame and requires both frames to reach a local server, in
 * order, with no further write to wake anything.  Before the fix nothing was
 * ever sent.  White-box: needs libgo's MPMC channels; skipped otherwise.
 */

#include <libwebsockets.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "go.h"
#include "nostr-connection.h"
#include "connection-private.h" /* WebSocketMessage: send_channel element */
#include "error.h"

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s (%s) at %s:%d\n", msg, #cond, \
                    __FILE__, __LINE__);                                     \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static atomic_ulong g_srv_rx_msgs;
static atomic_int g_srv_stop;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_log[256]; /* received payloads, '|'-separated */

static int server_cb(struct lws *wsi, enum lws_callback_reasons reason,
                     void *user, void *in, size_t len) {
    (void)user;
    if (reason == LWS_CALLBACK_RECEIVE) {
        pthread_mutex_lock(&g_log_lock);
        size_t used = strlen(g_log);
        if (used + len + 2 < sizeof g_log) {
            memcpy(g_log + used, in, len);
            g_log[used + len] = '\0';
            if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0)
                strcat(g_log, "|");
        }
        pthread_mutex_unlock(&g_log_lock);
        if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0)
            atomic_fetch_add(&g_srv_rx_msgs, 1UL);
    }
    return 0;
}

/* The libnostr client requests subprotocol "wss"; the server must offer it. */
static struct lws_protocols g_srv_protocols[] = {
    { "wss", server_cb, 0, 4096, 0, NULL, 0 },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

static struct lws_context *g_srv_ctx;

static void *server_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&g_srv_stop))
        lws_service(g_srv_ctx, 50);
    return NULL;
}

static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static int wait_srv_msgs(unsigned long msgs, double timeout_s) {
    double deadline = now_seconds() + timeout_s;
    while (now_seconds() < deadline) {
        if (atomic_load(&g_srv_rx_msgs) >= msgs) return 1;
        usleep(10000);
    }
    return atomic_load(&g_srv_rx_msgs) >= msgs;
}

static WebSocketMessage *frame(const char *text) {
    WebSocketMessage *m = malloc(sizeof *m);
    CHECK(m, "oom");
    m->data = strdup(text);
    CHECK(m->data, "oom");
    m->length = strlen(text);
    return m;
}

int main(void) {
    unsetenv("NOSTR_TEST_MODE"); /* real network path */
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

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

    char url[64];
    snprintf(url, sizeof url, "ws://127.0.0.1:%d", port);
    NostrConnection *conn = nostr_connection_new(url);
    CHECK(conn, "nostr_connection_new failed");

    /* Establish and prove the TX path works. */
    char hello[] = "hello";
    Error *err = NULL;
    for (double deadline = now_seconds() + 10.0;;) {
        err = NULL;
        nostr_connection_write_message(conn, NULL, hello, &err);
        if (!err) break;
        free_error(err);
        CHECK(now_seconds() < deadline, "connection never established");
        usleep(20000);
    }
    CHECK(wait_srv_msgs(1, 5.0), "first frame never reached the server");

    GoChannel *sc = conn->send_channel;
    if (!sc->slot_seq) {
        printf("test_connection_writable_rearm: skipped (libgo built without MPMC slots)\n");
        nostr_connection_close(conn);
        atomic_store(&g_srv_stop, 1);
        lws_cancel_service(g_srv_ctx);
        pthread_join(srv, NULL);
        lws_context_destroy(g_srv_ctx);
        return 0;
    }
    usleep(200000); /* let the TX path go idle (writable_pending cleared) */

    /* A writer that has claimed the next send_channel slot but not yet
     * published its frame ... */
    size_t t = atomic_fetch_add_explicit(&sc->in, 1, memory_order_acq_rel);
    /* ... and a real frame queued behind it: sets writable_pending and wakes
     * the service thread, whose CLIENT_WRITEABLE finds the head unreadable. */
    char second[] = "second";
    nostr_connection_write_message(conn, NULL, second, &err);
    CHECK(err == NULL, "enqueue behind the claimed slot");
    usleep(300000);
    CHECK(atomic_load(&g_srv_rx_msgs) == 1, "nothing may be sent before the head is published");

    /* The claimed frame is published; nothing else will write or wake. */
    size_t idx = t & sc->mask;
    atomic_store_explicit(&sc->buffer[idx], (void *)frame("first"), memory_order_release);
    atomic_store_explicit(&sc->slot_seq[idx], t + 1, memory_order_release);

    int ok = wait_srv_msgs(3, 5.0);
    pthread_mutex_lock(&g_log_lock);
    printf("server received %lu frames: %s\n", atomic_load(&g_srv_rx_msgs), g_log);
    int in_order = strcmp(g_log, "hello|first|second|") == 0;
    pthread_mutex_unlock(&g_log_lock);
    CHECK(ok, "queued frames stranded: CLIENT_WRITEABLE cleared writable_pending with frames queued");
    CHECK(in_order, "frames out of order");

    nostr_connection_close(conn);
    atomic_store(&g_srv_stop, 1);
    lws_cancel_service(g_srv_ctx);
    pthread_join(srv, NULL);
    lws_context_destroy(g_srv_ctx);
    printf("test_connection_writable_rearm: OK\n");
    return 0;
}
