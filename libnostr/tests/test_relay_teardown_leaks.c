/* Teardown-order regressions for what a NostrRelay leaves behind (nostrc-xfjg,
 * nostrc-vpha, nostrc-xbso, nostrc-jw23). Each case forces one teardown order
 * against an in-process loopback server, with no sleeps deciding the outcome:
 *
 *  reconnect   The server drops a live connection; the relay's own loop
 *              reconnects. The replaced connection must be released, channels
 *              and all (nostrc-xfjg: relay_attempt_reconnect() only closed it,
 *              and its recv/send channels leaked once the service thread freed
 *              the connection struct; groundhog-relay-wire hit it as
 *              nostrc-vpha when libnostr's reconnect beat Groundhog's retry).
 *              Checked by nostr_connection_unreleased_count(), then under
 *              LeakSanitizer after the service thread's 2 s graveyard.
 *  queued      A frame the writer queued on a connection whose handshake
 *              never completes is still in its send channel when the relay is
 *              freed; the release frees it (it leaked with the channel).
 *
 * Leaks themselves are visible to LeakSanitizer only (the Linux ASAN build:
 * groundhog-ci.yml's groundhog-sanitizers job runs this test); without it the
 * counters and white-box checks still pin the order and the release. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <libwebsockets.h>
#ifndef LWS_PROTOCOL_LIST_TERM
#define LWS_PROTOCOL_LIST_TERM { .name = NULL, .callback = NULL }
#endif

#include "channel.h"
#include "error.h"
#include "nostr-relay.h"
#include "select.h"
#include "../src/connection-private.h"
#include "../src/relay-private.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    abort(); \
} } while (0)

static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* Polls @cond (a condition on test state, not a timing guess) for up to 10 s. */
#define WAIT_FOR(cond) do { \
    double _deadline = now_s() + 10.0; \
    while (!(cond) && now_s() < _deadline) usleep(5000); \
    CHECK(cond); \
} while (0)

/* ---- a WebSocket server that can drop its live connection ------------------------ */

static struct {
    struct lws_context *ctx;
    pthread_t thread;
    atomic_bool running;
    atomic_int connections;   /* ESTABLISHED so far */
    atomic_int open;          /* currently open */
    atomic_bool drop;         /* close every open connection */
    int port;
} srv;

static int server_cb(struct lws *wsi, enum lws_callback_reasons reason, void *user,
                     void *in, size_t len) {
    (void)user;
    (void)in;
    (void)len;
    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        atomic_fetch_add(&srv.connections, 1);
        atomic_fetch_add(&srv.open, 1);
        break;
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
        if (atomic_load(&srv.drop))
            lws_callback_on_writable_all_protocol(lws_get_context(wsi),
                                                  lws_get_protocol(wsi));
        break;
    case LWS_CALLBACK_SERVER_WRITEABLE:
        if (atomic_exchange(&srv.drop, false)) return -1; /* close it */
        break;
    case LWS_CALLBACK_CLOSED:
        atomic_fetch_sub(&srv.open, 1);
        break;
    default:
        break;
    }
    return 0;
}

static const struct lws_protocols srv_protocols[] = {
    { "wss", server_cb, 0, 4096, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

static void *server_thread(void *arg) {
    (void)arg;
    while (atomic_load(&srv.running)) lws_service(srv.ctx, 50);
    return NULL;
}

static void server_start(void) {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = 0; /* kernel-chosen: safe under parallel ctest */
    info.iface = "127.0.0.1";
    info.protocols = srv_protocols;
    info.gid = (gid_t)-1;
    info.uid = (uid_t)-1;
    srv.ctx = lws_create_context(&info);
    CHECK(srv.ctx);
    struct lws_vhost *vh = lws_get_vhost_by_name(srv.ctx, "default");
    CHECK(vh);
    srv.port = lws_get_vhost_listen_port(vh);
    CHECK(srv.port > 0);
    atomic_store(&srv.running, true);
    CHECK(pthread_create(&srv.thread, NULL, server_thread, NULL) == 0);
}

static void server_drop(void) {
    atomic_store(&srv.drop, true);
    lws_cancel_service(srv.ctx);
}

static void server_stop(void) {
    atomic_store(&srv.running, false);
    lws_cancel_service(srv.ctx);
    pthread_join(srv.thread, NULL);
    lws_context_destroy(srv.ctx);
}

/* ---- a TCP listener that never answers the WebSocket upgrade ------------------- */

/* Nothing accepts: the kernel's listen backlog completes the TCP handshake
 * and the client's upgrade request waits there, unanswered. */
static struct {
    int fd;
    int port;
} mute;

static void mute_start(void) {
    mute.fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(mute.fd >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(mute.fd, (struct sockaddr *)&addr, sizeof addr) == 0);
    CHECK(listen(mute.fd, 4) == 0);
    socklen_t len = sizeof addr;
    CHECK(getsockname(mute.fd, (struct sockaddr *)&addr, &len) == 0);
    mute.port = ntohs(addr.sin_port);
}

static void mute_stop(void) {
    close(mute.fd);
}

/* ---- helpers ---------------------------------------------------------------------- */

static NostrRelay *relay_for_port(int port) {
    char url[64];
    snprintf(url, sizeof url, "ws://127.0.0.1:%d", port);
    Error *err = NULL;
    /* No parent context: nothing outlives the relay. */
    NostrRelay *relay = nostr_relay_new(NULL, url, &err);
    CHECK(relay && !err);
    return relay;
}

static NostrRelayConnectionState state_of(NostrRelay *relay) {
    return nostr_relay_get_connection_state(relay);
}

/* ---- cases ------------------------------------------------------------------------ */

/* nostrc-xfjg / nostrc-vpha (1). */
static void test_reconnect_releases_replaced_connection(void) {
    server_start();
    int base = nostr_connection_unreleased_count();

    NostrRelay *relay = relay_for_port(srv.port);
    Error *err = NULL;
    CHECK(nostr_relay_connect(relay, &err));
    CHECK(nostr_relay_wait_established(relay, 10000, &err));
    CHECK(!err);
    WAIT_FOR(atomic_load(&srv.connections) == 1);
    CHECK(nostr_connection_unreleased_count() == base + 1);

    /* The server drops the connection; the relay's loop notices, backs off,
     * and is woken to reconnect now rather than after its jittered backoff. */
    server_drop();
    WAIT_FOR(atomic_load(&srv.open) == 0);
    WAIT_FOR(state_of(relay) == NOSTR_RELAY_STATE_BACKOFF);
    nostr_relay_reconnect_now(relay);
    WAIT_FOR(atomic_load(&srv.connections) == 2 && nostr_relay_is_established(relay));

    /* The replaced connection went through nostr_connection_release(): the
     * only one left unreleased is the live one. (It stayed at base + 2.) */
    CHECK(nostr_connection_unreleased_count() == base + 1);

    /* The service thread frees a closed connection's struct 2 s after its
     * close. Outlive that, so LeakSanitizer would see channels nobody freed
     * (before the fix the struct was their last reference). */
    usleep(2500 * 1000);

    nostr_relay_free(relay);
    CHECK(nostr_connection_unreleased_count() == base);
    server_stop();
    printf("  [ok] reconnect releases the replaced connection\n");
}

/* nostrc-xfjg / nostrc-jw23: a frame queued in a connection's send channel
 * when the relay goes is freed with it. */
static void test_release_frees_frames_never_written(void) {
    mute_start();
    int base = nostr_connection_unreleased_count();
    NostrRelay *relay = relay_for_port(mute.port);
    Error *err = NULL;
    CHECK(nostr_relay_connect(relay, &err)); /* the dial starts; no handshake ever */
    CHECK(!err);

    /* The writer queues the frame on the connection (the answer NULL says so)
     * but the WSI never becomes writable: the handshake is still pending. */
    char frame[] = "[\"REQ\",\"q\",{}]";
    GoChannel *answer = nostr_relay_write(relay, frame);
    CHECK(answer);
    Error *write_err = (Error *)1;
    GoSelectCase c = { .op = GO_SELECT_RECEIVE, .chan = answer, .recv_buf = (void **)&write_err };
    GoSelectResult r = go_select_timeout(&c, 1, 10000);
    CHECK(r.selected_case == 0 && r.ok && write_err == NULL);
    go_channel_close(answer);
    go_channel_unref(answer);

    nsync_mu_lock(&relay->priv->mutex);
    NostrConnection *conn = relay->connection;
    CHECK(conn && conn->send_channel);
    CHECK(go_channel_get_depth(conn->send_channel) == 1);
    nsync_mu_unlock(&relay->priv->mutex);

    nostr_relay_free(relay); /* releases the connection with the frame queued */
    CHECK(nostr_connection_unreleased_count() == base);
    mute_stop();
    printf("  [ok] a released connection frees the frames it never wrote\n");
}

int main(void) {
    unsetenv("NOSTR_TEST_MODE"); /* the real network path */
    lws_set_log_level(LLL_ERR, NULL);

    test_reconnect_releases_replaced_connection();
    test_release_frees_frames_never_written();

    puts("test_relay_teardown_leaks: OK");
    return 0;
}
