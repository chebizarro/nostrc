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
 *  abandon     nostr_subscription_free_async()'s worker finishes before the
 *              caller abandons the handle (nostrc-xbso: the worker checked
 *              'abandoned' once, so the handle and its done channel leaked).
 *  late answer The writer's Error lands in the answer channel after the
 *              caller stopped waiting and before it lets go (nostrc-xbso R2:
 *              the channel's last unref frees no items).
 *  close       Writes queued behind a blocked writer are answered by
 *              nostr_relay_close(), not left until the relay is freed
 *              (nostrc-xbso R3).
 *  destroy     A CLOSED reason and a COUNT result nobody read are freed with
 *              the subscription (nostrc-jwj0's dispatch_closed allocation).
 *  contexts    A background context's last unref frees it, and a
 *              subscription releases its own context (nostrc-jw23: neither
 *              was ever freed; every subscription leaked one).
 *  filters     Filters handed over with nostr_subscription_set_filters() are
 *              freed with the subscription, after an async cleanup too
 *              (GNostrSubscription's finalize dropped them, nostrc-jw23).
 *  dials       Two nostr_relay_connect() calls race on one relay (a shared
 *              relay connected from two pools): both pass the "already
 *              connected?" check, a hook holds both until both have dialled,
 *              and exactly one connection may survive (nostrc-vpha: both were
 *              stored unlocked, the first lost with its channels and a second
 *              pair of workers).
 *  adopt       The loop's reconnect finds that a nostr_relay_connect() published
 *              a connection during its own dial, and adopts it: its own dial
 *              is released at once, although the writer holds a lease on the
 *              adopted connection, blocked on its full send channel (its
 *              handshake is held). It used to wait for that lease: a
 *              deadlock once the adopted handshake failed (nostrc-vpha B1).
 *  close idle  Writes queued on a relay that is not connected are answered
 *              by nostr_relay_close(), not only by the free (nostrc-vpha N6).
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
#include "nostr-filter.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "select.h"
#include "../src/connection-private.h"
#include "../src/relay-private.h"
#include "../src/subscription-private.h"

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
    atomic_bool paused;       /* service nothing: handshakes stay pending */
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
    while (atomic_load(&srv.running)) {
        if (atomic_load(&srv.paused)) usleep(1000);
        else lws_service(srv.ctx, 50);
    }
    return NULL;
}

/* Started once for the whole run: with Homebrew's libwebsockets plugins a
 * context teardown closes fd 0 (see nostrc-jc2o in connection.c), and the
 * next context then logs "ZERO RANDOM FD". */
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
    int base = nostr_connection_unreleased_count();
    int seen = atomic_load(&srv.connections);

    NostrRelay *relay = relay_for_port(srv.port);
    Error *err = NULL;
    CHECK(nostr_relay_connect(relay, &err));
    CHECK(nostr_relay_wait_established(relay, 10000, &err));
    CHECK(!err);
    WAIT_FOR(atomic_load(&srv.connections) == seen + 1);
    CHECK(nostr_connection_unreleased_count() == base + 1);

    /* The server drops the connection; the relay's loop notices, backs off,
     * and is woken to reconnect now rather than after its jittered backoff. */
    server_drop();
    WAIT_FOR(atomic_load(&srv.open) == 0);
    WAIT_FOR(state_of(relay) == NOSTR_RELAY_STATE_BACKOFF);
    nostr_relay_reconnect_now(relay);
    WAIT_FOR(atomic_load(&srv.connections) == seen + 2 && nostr_relay_is_established(relay));

    /* The replaced connection went through nostr_connection_release(): the
     * only one left unreleased is the live one. (It stayed at base + 2.) */
    CHECK(nostr_connection_unreleased_count() == base + 1);

    /* The service thread frees a closed connection's struct 2 s after its
     * close. Outlive that, so LeakSanitizer would see channels nobody freed
     * (before the fix the struct was their last reference). */
    usleep(2500 * 1000);

    nostr_relay_free(relay);
    CHECK(nostr_connection_unreleased_count() == base);
    WAIT_FOR(atomic_load(&srv.open) == 0);
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

static NostrFilters *any_filters(void) {
    NostrFilters *filters = nostr_filters_new();
    NostrFilter *filter = nostr_filter_new();
    CHECK(filters && filter && nostr_filters_add(filters, filter));
    nostr_filter_free(filter); /* contents moved into the vector */
    return filters;
}

/* nostrc-xbso: the worker is done before the caller abandons the handle. */
static void test_abandon_after_cleanup_completed(void) {
    NostrRelay *relay = relay_for_port(1); /* never connected */
    NostrFilters *filters = any_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    CHECK(sub);
    AsyncCleanupHandle *handle = nostr_subscription_free_async(sub, 1000);
    CHECK(handle);
    WAIT_FOR(nostr_subscription_cleanup_is_complete(handle));
    usleep(20000); /* past the worker's last touch of the handle */
    nostr_subscription_cleanup_abandon(handle); /* frees it now */
    nostr_filters_free(filters);
    nostr_relay_free(relay);
    printf("  [ok] a handle abandoned after its cleanup finished is freed\n");
}

/* nostrc-xbso R2: an Error the writer sent after the caller gave up. The
 * relay loses its connection and does not reconnect, so every write fails. */
static void test_late_answer_is_freed(void) {
    NostrRelay *relay = relay_for_port(srv.port);
    nostr_relay_set_auto_reconnect(relay, false);
    Error *err = NULL;
    CHECK(nostr_relay_connect(relay, &err));
    CHECK(nostr_relay_wait_established(relay, 10000, &err));
    server_drop();
    WAIT_FOR(atomic_load(&srv.open) == 0 && !nostr_relay_is_connected(relay));

    char frame[] = "[\"CLOSE\",\"x\"]";
    GoChannel *answer = nostr_relay_write(relay, frame);
    CHECK(answer);
    /* The caller stopped waiting (a zero-timeout check, a timed-out publish);
     * then the writer's Error arrives, before the caller lets go. */
    WAIT_FOR(go_channel_get_depth(answer) == 1);
    nostr_relay_write_answer_release(answer);

    nostr_relay_free(relay);
    printf("  [ok] an answer that arrives after the caller gave up is freed\n");
}

/* nostrc-xbso R3: writes still queued when the relay is closed are answered
 * by the close. The writer is blocked: the connection's handshake never
 * completes, so its send channel (16 frames) fills and stays full. */
static void test_close_answers_queued_writes(void) {
    enum { SEND_CAPACITY = 16, EXTRA = 8, N = SEND_CAPACITY + 1 + EXTRA };
    mute_start();
    NostrRelay *relay = relay_for_port(mute.port);
    Error *err = NULL;
    CHECK(nostr_relay_connect(relay, &err));
    GoChannel *answers[N];
    char frame[] = "[\"REQ\",\"q\",{}]";
    for (int i = 0; i < N; i++) {
        answers[i] = nostr_relay_write(relay, frame);
        CHECK(answers[i]);
    }
    /* 16 frames queued on the connection, the writer blocked on the 17th,
     * the rest waiting in the relay's write queue. */
    WAIT_FOR(go_channel_get_depth(relay->priv->write_queue) == EXTRA);

    nostr_relay_close(relay, NULL);
    int answered = 0;
    for (int i = 0; i < N; i++) {
        void *got = (void *)1;
        if (go_channel_try_receive(answers[i], &got) == 0) {
            answered++;
            if (got) free_error((Error *)got);
        }
        nostr_relay_write_answer_release(answers[i]);
    }
    CHECK(answered == N); /* the queued ones used to wait for the free */
    nostr_relay_free(relay);
    mute_stop();
    printf("  [ok] closing the relay answers the writes still queued\n");
}

/* A CLOSED reason and a COUNT result nobody received are freed with the
 * subscription. */
static void test_destroy_frees_unread_results(void) {
    NostrRelay *relay = relay_for_port(1); /* never connected */
    NostrFilters *filters = any_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    CHECK(sub);
    nostr_subscription_dispatch_closed(sub, "auth-required: nobody reads this");
    CHECK(go_channel_get_depth(sub->closed_reason) == 1);
    sub->priv->count_result = go_channel_create(1);
    int64_t *count = malloc(sizeof *count);
    CHECK(count);
    *count = 7;
    CHECK(go_channel_send(sub->priv->count_result, count) == 0);
    nostr_subscription_free(sub);
    nostr_filters_free(filters);
    nostr_relay_free(relay);
    printf("  [ok] a subscription frees the CLOSED reason and COUNT nobody read\n");
}

/* nostrc-jw23: contexts. A background context used to be kept forever (no
 * vtable, so go_context_free() did nothing), and a subscription never let
 * go of its own context. */
static void test_contexts_are_released(void) {
    GoContext *bg = go_context_background();
    CHECK(bg && go_context_done(bg) == NULL); /* vtable-less, as before */
    go_context_unref(bg);

    NostrRelay *relay = relay_for_port(1); /* never connected */
    NostrFilters *filters = any_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    CHECK(sub && sub->context);
    nostr_subscription_free(sub); /* destroys it and its context */
    nostr_filters_free(filters);
    nostr_relay_free(relay);
    printf("  [ok] background and subscription contexts are released\n");
}

/* nostrc-jw23: filters handed to the subscription go with it, also when an
 * abandoned async cleanup destroys it (GNostrSubscription's finalize). */
static void test_set_filters_owned_by_subscription(void) {
    NostrRelay *relay = relay_for_port(1); /* never connected */
    NostrFilters *filters = any_filters();
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    CHECK(sub);
    nostr_subscription_set_filters(sub, filters); /* same pointer: now owned */
    CHECK(nostr_subscription_get_filters(sub) == filters);
    AsyncCleanupHandle *handle = nostr_subscription_free_async(sub, 1000);
    CHECK(handle);
    nostr_subscription_cleanup_abandon(handle);
    /* No wait: libnostr finishes the cleanup at exit if it is still running. */
    nostr_relay_free(relay); /* the subscription keeps its own reference */
    printf("  [ok] set filters are freed with the subscription\n");
}

/* nostrc-vpha: racing dials publish one connection. */
static atomic_int g_dials;

static void hold_dial(NostrRelay *relay, void *data) {
    (void)relay;
    (void)data;
    atomic_fetch_add(&g_dials, 1);
    double deadline = now_s() + 10.0; /* a failure bound: both dial at once */
    while (atomic_load(&g_dials) < 2 && now_s() < deadline) usleep(1000);
}

static void *dial_thread(void *arg) {
    Error *err = NULL;
    bool ok = nostr_relay_connect((NostrRelay *)arg, &err);
    if (err) free_error(err);
    return ok ? arg : NULL;
}

static void test_racing_dials_publish_one_connection(void) {
    int base = nostr_connection_unreleased_count();
    NostrRelay *relay = relay_for_port(srv.port);
    atomic_store(&g_dials, 0);
    nostr_relay_test_set_dial_hook(hold_dial, NULL);
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, dial_thread, relay) == 0);
    CHECK(pthread_create(&b, NULL, dial_thread, relay) == 0);
    void *ra = NULL, *rb = NULL;
    pthread_join(a, &ra);
    pthread_join(b, &rb);
    nostr_relay_test_set_dial_hook(NULL, NULL);
    CHECK(atomic_load(&g_dials) == 2); /* both dialled: the race happened */
    CHECK(ra && rb);                   /* and the relay is connected for both */
    CHECK(nostr_connection_unreleased_count() == base + 1); /* was base + 2 */
    Error *err = NULL;
    CHECK(nostr_relay_wait_established(relay, 10000, &err));
    WAIT_FOR(atomic_load(&srv.open) == 1); /* the loser's socket is closed */
    nostr_relay_free(relay);
    CHECK(nostr_connection_unreleased_count() == base);
    WAIT_FOR(atomic_load(&srv.open) == 0);
    printf("  [ok] racing dials publish one connection\n");
}

/* nostrc-vpha B1: the loop's reconnect adopts a connection published during
 * its dial without waiting for the lease the writer holds on it. */
static atomic_bool g_loop_dialled, g_loop_go;

static void hold_reconnect(NostrRelay *relay, void *data) {
    (void)relay;
    (void)data;
    atomic_store(&g_loop_dialled, true);
    double deadline = now_s() + 20.0; /* a failure bound */
    while (!atomic_load(&g_loop_go) && now_s() < deadline) usleep(1000);
}

static void test_reconnect_adopts_without_lease_wait(void) {
    enum { SEND_CAPACITY = 16, N = SEND_CAPACITY + 8 };
    int base = nostr_connection_unreleased_count();
    NostrRelay *relay = relay_for_port(srv.port);
    Error *err = NULL;
    CHECK(nostr_relay_connect(relay, &err));
    CHECK(nostr_relay_wait_established(relay, 10000, &err));
    atomic_store(&g_loop_dialled, false);
    atomic_store(&g_loop_go, false);
    nostr_relay_test_set_reconnect_hook(hold_reconnect, NULL);

    /* The server drops the connection; the loop reconnects at once and is
     * held right after its own dial (C), with relay->connection NULL. */
    server_drop();
    WAIT_FOR(nostr_relay_get_connection_state(relay) == NOSTR_RELAY_STATE_BACKOFF);
    nostr_relay_reconnect_now(relay);
    WAIT_FOR(atomic_load(&g_loop_dialled));

    /* A connect publishes B, whose handshake the paused server holds; no
     * second loop. The writer fills B's send channel and blocks on the next
     * frame, holding a lease on B. */
    atomic_store(&srv.paused, true);
    CHECK(nostr_relay_connect(relay, &err));
    nsync_mu_lock(&relay->priv->mutex);
    NostrConnection *b = relay->connection;
    nsync_mu_unlock(&relay->priv->mutex);
    CHECK(b);
    GoChannel *answers[N];
    char frame[] = "[\"REQ\",\"q\",{}]";
    for (int i = 0; i < N; i++) CHECK((answers[i] = nostr_relay_write(relay, frame)));
    WAIT_FOR(relay->priv->conn_leases == 1 &&
             go_channel_get_depth(b->send_channel) == SEND_CAPACITY);

    /* The loop adopts B and releases C without waiting for B's writer. */
    int before = nostr_connection_unreleased_count();
    atomic_store(&g_loop_go, true);
    WAIT_FOR(nostr_connection_unreleased_count() == before - 1); /* hung here */
    nsync_mu_lock(&relay->priv->mutex);
    CHECK(relay->connection == b);
    nsync_mu_unlock(&relay->priv->mutex);

    /* B's handshake completes: the writer drains and every write is answered. */
    atomic_store(&srv.paused, false);
    CHECK(nostr_relay_wait_established(relay, 10000, &err));
    WAIT_FOR(relay->priv->conn_leases == 0 &&
             go_channel_get_depth(relay->priv->write_queue) == 0);
    for (int i = 0; i < N; i++) {
        void *got = (void *)1;
        WAIT_FOR(go_channel_get_depth(answers[i]) == 1 || go_channel_is_closed(answers[i]));
        CHECK(go_channel_try_receive(answers[i], &got) == 0 && got == NULL);
        nostr_relay_write_answer_release(answers[i]);
    }
    nostr_relay_test_set_reconnect_hook(NULL, NULL);
    nostr_relay_free(relay);
    CHECK(nostr_connection_unreleased_count() == base);
    WAIT_FOR(atomic_load(&srv.open) == 0);
    printf("  [ok] a reconnect adopts a published connection without a lease wait\n");
}

/* nostrc-vpha N6: writes queued on a relay that is not connected (never
 * here; also between a lost connection and its reconnect) are answered by
 * the close. It returned early, and they waited for the free. */
static void test_close_answers_writes_when_not_connected(void) {
    enum { N = 8 };
    NostrRelay *relay = relay_for_port(srv.port); /* never connected */
    GoChannel *answers[N];
    char frame[] = "[\"REQ\",\"q\",{}]";
    for (int i = 0; i < N; i++) CHECK((answers[i] = nostr_relay_write(relay, frame)));
    CHECK(go_channel_get_depth(relay->priv->write_queue) == N);
    Error *err = NULL;
    CHECK(!nostr_relay_close(relay, &err)); /* "relay not connected" */
    if (err) free_error(err);
    int answered = 0;
    for (int i = 0; i < N; i++) {
        void *got = NULL;
        if (go_channel_try_receive(answers[i], &got) == 0 && got) {
            answered++;
            free_error((Error *)got);
        }
        nostr_relay_write_answer_release(answers[i]);
    }
    CHECK(answered == N);
    nostr_relay_free(relay);
    printf("  [ok] closing a relay that is not connected answers its queued writes\n");
}

int main(void) {
    unsetenv("NOSTR_TEST_MODE"); /* the real network path */
    lws_set_log_level(LLL_ERR, NULL);
    server_start();

    test_reconnect_releases_replaced_connection();
    test_release_frees_frames_never_written();
    test_abandon_after_cleanup_completed();
    test_late_answer_is_freed();
    test_close_answers_queued_writes();
    test_destroy_frees_unread_results();
    test_contexts_are_released();
    test_set_filters_owned_by_subscription();
    test_racing_dials_publish_one_connection();
    test_reconnect_adopts_without_lease_wait();
    test_close_answers_writes_when_not_connected();

    server_stop();

    puts("test_relay_teardown_leaks: OK");
    return 0;
}
