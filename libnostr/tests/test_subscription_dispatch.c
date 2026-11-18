/* Regression tests for nostrc-prqu.4:
 *
 *   1. nostr_relay_prepare_subscription() must accept ctx == NULL (the ctx
 *      argument is unused; gnostr's neg-client passes NULL and always failed).
 *   2. A subscription created with nostr_subscription_new() + fire must be
 *      registered in relay->subscriptions so message_loop routes the relay's
 *      EVENT/EOSE frames to it (notify_subs.c uses exactly this path and
 *      received nothing).
 *
 * Part A runs in NOSTR_TEST_MODE (offline) and checks the dispatch map
 * directly. Part B stands up a loopback fixture relay (a libwebsockets server
 * on 127.0.0.1) that answers every REQ with one signed EVENT + EOSE for that
 * subscription id, and asserts both arrive on the subscription's channels.
 * Pre-fix, part B's event never arrives (the frame is dropped as "unknown
 * subscription") and the test fails on the deadline.
 */
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libwebsockets.h>
/* libwebsockets < 4.3 (e.g. Debian bookworm) lacks this terminator macro. */
#ifndef LWS_PROTOCOL_LIST_TERM
#define LWS_PROTOCOL_LIST_TERM { .name = NULL, .callback = NULL }
#endif

#include "error.h"
#include "go.h"
#include "json.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-keys.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-simple-pool.h"
#include <errno.h>
#include "../src/relay-private.h"
#include "../src/subscription-private.h"

/* Release builds define NDEBUG; these checks must always run. */
#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #expr);                                                \
            abort();                                                       \
        }                                                                  \
    } while (0)

static NostrFilters *kind1_filters(void) {
    NostrFilters *fs = nostr_filters_new();
    NostrFilter *f = nostr_filter_new();
    int kinds[] = {1};
    nostr_filter_set_kinds(f, kinds, 1);
    CHECK(nostr_filters_add(fs, f));
    nostr_filter_free(f); /* contents moved into fs */
    return fs;
}

static bool in_dispatch_map(NostrRelay *relay, NostrSubscription *sub) {
    return go_hash_map_get_int(relay->subscriptions, sub->priv->counter) == sub;
}

static void clear_error(Error **err) {
    if (err && *err) {
        free_error(*err);
        *err = NULL;
    }
}

/* ---------------------------------------------------------------- Part A */

static void test_offline_registration(void) {
    CHECK(setenv("NOSTR_TEST_MODE", "1", 1) == 0);
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(go_context_background(), "wss://prqu4.invalid", &err);
    CHECK(relay && err == NULL);
    CHECK(nostr_relay_connect(relay, &err));
    clear_error(&err);

    NostrFilters *fs = kind1_filters();

    /* 1. NULL ctx is accepted and the subscription is registered at once. */
    NostrSubscription *prepared = nostr_relay_prepare_subscription(relay, NULL, fs);
    CHECK(prepared != NULL);
    CHECK(in_dispatch_map(relay, prepared));
    /* NULL relay / filters still rejected. */
    CHECK(nostr_relay_prepare_subscription(NULL, NULL, fs) == NULL);
    CHECK(nostr_relay_prepare_subscription(relay, NULL, NULL) == NULL);

    /* 2. nostr_subscription_new(): not dispatchable until fired, then it is. */
    NostrSubscription *direct = nostr_subscription_new(relay, fs);
    CHECK(direct != NULL);
    CHECK(!in_dispatch_map(relay, direct));
    CHECK(nostr_subscription_fire(direct, &err));
    clear_error(&err);
    CHECK(in_dispatch_map(relay, direct));
    /* Refire is idempotent. */
    CHECK(nostr_subscription_fire(direct, &err));
    clear_error(&err);
    CHECK(in_dispatch_map(relay, direct));

    /* 3. Teardown removes it and a later fire cannot resurrect the entry. */
    int direct_counter = direct->priv->counter;
    nostr_subscription_ref(direct);
    nostr_subscription_free(direct);
    CHECK(go_hash_map_get_int(relay->subscriptions, direct_counter) == NULL);
    (void)nostr_subscription_fire(direct, &err);
    clear_error(&err);
    CHECK(go_hash_map_get_int(relay->subscriptions, direct_counter) == NULL);
    nostr_subscription_unref(direct);

    nostr_subscription_free(prepared);
    nostr_filters_free(fs);
    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    unsetenv("NOSTR_TEST_MODE");
    printf("  [ok] offline: NULL ctx accepted; new()+fire registers; free unregisters\n");
}

/* ---------------------------------------------------------------- Part B */

typedef struct {
    struct lws_context *ctx;
    int port;
    atomic_bool running;
    pthread_t thread;
    char *event_json;        /* signed kind-1 event served for every REQ */
    atomic_int reqs_seen;
    bool close_first; /* pool regression: CLOSED forces worker reconciliation */
    atomic_int protocol_headers_seen;
} FixtureRelay;

typedef struct {
    char *queue[256];          /* pending outbound text frames */
    int n;
} FixtureConn;

static FixtureRelay g_fx;

static void fx_enqueue(FixtureConn *c, char *msg) {
    if (c->n < (int)(sizeof(c->queue) / sizeof(c->queue[0]))) c->queue[c->n++] = msg;
    else free(msg);
}

static int fx_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
    FixtureConn *c = (FixtureConn *)user;
    switch (reason) {
    case LWS_CALLBACK_HTTP_CONFIRM_UPGRADE:
        if (lws_hdr_total_length(wsi, WSI_TOKEN_PROTOCOL) != 0)
            atomic_fetch_add(&g_fx.protocol_headers_seen, 1);
        break;
    case LWS_CALLBACK_ESTABLISHED:
        memset(c, 0, sizeof(*c));
        break;
    case LWS_CALLBACK_RECEIVE: {
        /* Expect ["REQ","<sid>",{...}] — the client never fragments these. */
        const char *prefix = "[\"REQ\",\"";
        size_t plen = strlen(prefix);
        if (len > plen && memcmp(in, prefix, plen) == 0) {
            const char *sid = (const char *)in + plen;
            const char *end = memchr(sid, '"', len - plen);
            if (!end) break;
            int sid_len = (int)(end - sid);
            int req = atomic_fetch_add(&g_fx.reqs_seen, 1);
            if (g_fx.close_first && req == 0) {
                char *closed = malloc((size_t)sid_len + 48);
                CHECK(closed);
                snprintf(closed, (size_t)sid_len + 48,
                         "[\"CLOSED\",\"%.*s\",\"error: retry fixture\"]", sid_len, sid);
                fx_enqueue(c, closed);
                lws_callback_on_writable(wsi);
                break;
            }
            size_t need = strlen(g_fx.event_json) + (size_t)sid_len + 32;
            char *ev = malloc(need);
            snprintf(ev, need, "[\"EVENT\",\"%.*s\",%s]", sid_len, sid, g_fx.event_json);
            fx_enqueue(c, ev);
            char *eose = malloc((size_t)sid_len + 16);
            snprintf(eose, (size_t)sid_len + 16, "[\"EOSE\",\"%.*s\"]", sid_len, sid);
            fx_enqueue(c, eose);
            lws_callback_on_writable(wsi);
        }
        break;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE:
        if (c->n > 0) {
            char *msg = c->queue[0];
            memmove(&c->queue[0], &c->queue[1], sizeof(char *) * (size_t)(c->n - 1));
            c->n--;
            size_t mlen = strlen(msg);
            unsigned char *buf = malloc(LWS_PRE + mlen);
            memcpy(buf + LWS_PRE, msg, mlen);
            int wrote = lws_write(wsi, buf + LWS_PRE, mlen, LWS_WRITE_TEXT);
            free(buf);
            free(msg);
            if (wrote < (int)mlen) return -1;
            if (c->n > 0) lws_callback_on_writable(wsi);
        }
        break;
    case LWS_CALLBACK_CLOSED:
        for (int i = 0; i < c->n; i++) free(c->queue[i]);
        c->n = 0;
        break;
    default:
        break;
    }
    return 0;
}

/* The handler name is local; NIP-01 does not negotiate a WS subprotocol. */
static const struct lws_protocols fx_protocols[] = {
    { "wss", fx_callback, sizeof(FixtureConn), 128 * 1024, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

static void *fx_service(void *arg) {
    (void)arg;
    while (atomic_load(&g_fx.running)) lws_service(g_fx.ctx, 50);
    return NULL;
}

static int pick_free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    CHECK(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    socklen_t sl = sizeof(sa);
    CHECK(getsockname(fd, (struct sockaddr *)&sa, &sl) == 0);
    close(fd);
    return ntohs(sa.sin_port);
}

static void fx_start_mode(const char *event_json, bool close_first) {
    memset(&g_fx, 0, sizeof(g_fx));
    g_fx.close_first = close_first;
    g_fx.event_json = strdup(event_json);
    g_fx.port = pick_free_port();
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = g_fx.port;
    info.iface = "127.0.0.1";
    info.protocols = fx_protocols;
    info.gid = -1;
    info.uid = -1;
    g_fx.ctx = lws_create_context(&info);
    CHECK(g_fx.ctx != NULL);
    atomic_store(&g_fx.running, true);
    CHECK(pthread_create(&g_fx.thread, NULL, fx_service, NULL) == 0);
}

static void fx_start(const char *event_json) {
    fx_start_mode(event_json, false);
}

static void fx_stop(void) {
    atomic_store(&g_fx.running, false);
    lws_cancel_service(g_fx.ctx);
    pthread_join(g_fx.thread, NULL);
    lws_context_destroy(g_fx.ctx);
    free(g_fx.event_json);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Poll a channel until it yields a value or the deadline passes. */
static void *recv_before(GoChannel *ch, double deadline) {
    while (now_s() < deadline) {
        void *v = NULL;
        /* try_receive returns -1 for both "empty" and "closed". */
        if (go_channel_try_receive(ch, &v) == 0) return v ? v : (void *)1;
        if (go_channel_is_closed(ch)) return NULL;
        usleep(10000);
    }
    return NULL;
}

static void run_loopback_case(NostrRelay *relay, bool use_prepare, const char *want_id) {
    Error *err = NULL;
    NostrFilters *fs = kind1_filters();
    NostrSubscription *sub = use_prepare
        ? nostr_relay_prepare_subscription(relay, NULL, fs)
        : nostr_subscription_new(relay, fs);
    CHECK(sub != NULL);
    CHECK(nostr_subscription_fire(sub, &err));
    clear_error(&err);

    double deadline = now_s() + 5.0;
    NostrEvent *ev = (NostrEvent *)recv_before(sub->events, deadline);
    if (!ev) {
        fprintf(stderr, "FAIL (%s): no EVENT delivered to the subscription "
                        "(REQs seen by fixture: %d)\n",
                use_prepare ? "prepare(NULL ctx)" : "new()+fire",
                atomic_load(&g_fx.reqs_seen));
        exit(1);
    }
    char *got_id = nostr_event_get_id(ev);
    CHECK(got_id && strcmp(got_id, want_id) == 0);
    free(got_id);
    nostr_event_free(ev);
    CHECK(recv_before(sub->end_of_stored_events, now_s() + 5.0) != NULL);

    nostr_subscription_unsubscribe(sub);
    nostr_subscription_free(sub);
    nostr_filters_free(fs);
}

static void test_loopback_fixture_relay(void) {
    unsetenv("NOSTR_TEST_MODE");

    char *sk = nostr_key_generate_private();
    CHECK(sk);
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev, "prqu.4 fixture event");
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    CHECK(nostr_event_sign(ev, sk) == 0);
    char *want_id = nostr_event_get_id(ev);
    char *json = nostr_event_serialize(ev);
    CHECK(want_id && json);
    free(sk);
    nostr_event_free(ev);

    fx_start(json);
    free(json);

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d", g_fx.port);
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(go_context_background(), url, &err);
    CHECK(relay && err == NULL);
    CHECK(nostr_relay_connect(relay, &err));
    clear_error(&err);
    double deadline = now_s() + 5.0;
    while (!nostr_relay_is_established(relay) && now_s() < deadline) usleep(10000);
    CHECK(nostr_relay_is_established(relay));

    run_loopback_case(relay, false, want_id); /* notify_subs.c path */
    run_loopback_case(relay, true, want_id);  /* neg-client path (NULL ctx) */
    CHECK(atomic_load(&g_fx.reqs_seen) >= 2);

    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    CHECK(atomic_load(&g_fx.protocol_headers_seen) == 0);
    fx_stop();
    free(want_id);
    printf("  [ok] loopback fixture relay: EVENT+EOSE reach new()+fire and prepare(NULL) subs\n");
}

/* Concurrent public configuration against the real pool worker, transport and
 * relay fixture above. CLOSED, not a sleep/disconnect timer, makes the worker
 * prepare replacement subscriptions; EOSE progress gates the subscribe load
 * (not a per-REQ acknowledgement once the two subscriber threads overlap). */
typedef struct {
    NostrSimplePool *pool;
    char url[64];
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    unsigned eose, closed, delivered[4];
    atomic_bool stop;
} PoolConfigTest;

static PoolConfigTest config_test;
static int token_a, token_b;

static void config_boundary(NostrRelay *relay, const char *state,
                            const char *reason, void *data) {
    (void)relay; (void)reason;
    PoolConfigTest *t = data;
    pthread_mutex_lock(&t->mutex);
    if (strcmp(state, "EOSE") == 0) t->eose++;
    if (strcmp(state, "CLOSED") == 0) t->closed++;
    pthread_cond_broadcast(&t->changed);
    pthread_mutex_unlock(&t->mutex);
}

static void config_delivered(NostrIncomingEvent *ev, unsigned mode) {
    /* Re-enter a pool API: middleware must never execute under pool_mutex. */
    (void)nostr_simple_pool_get_brown_list_enabled(config_test.pool);
    pthread_mutex_lock(&config_test.mutex);
    config_test.delivered[mode]++;
    pthread_cond_broadcast(&config_test.changed);
    pthread_mutex_unlock(&config_test.mutex);
    nostr_event_free(ev->event);
}

static void config_legacy(NostrIncomingEvent *ev) { config_delivered(ev, 0); }
static void config_ex_a(NostrIncomingEvent *ev, void *data) {
    CHECK(data == &token_a);
    config_delivered(ev, 1);
}
static void config_ex_b(NostrIncomingEvent *ev, void *data) {
    CHECK(data == &token_b);
    config_delivered(ev, 2);
}
static void config_batch(NostrIncomingEvent *ev, size_t n) {
    for (size_t i = 0; i < n; i++) config_delivered(&ev[i], 3);
}

static void config_set_mode(NostrSimplePool *pool, unsigned mode) {
    nostr_simple_pool_set_batch_middleware(pool, mode == 3 ? config_batch : NULL);
    if (mode == 0) nostr_simple_pool_set_event_middleware(pool, config_legacy);
    else nostr_simple_pool_set_event_middleware_ex(pool,
        mode == 1 ? config_ex_a : config_ex_b, mode == 1 ? &token_a : &token_b);
}

static void *config_writer(void *arg) {
    PoolConfigTest *t = arg;
    unsigned i = 0;
    while (!atomic_load_explicit(&t->stop, memory_order_acquire)) {
        config_set_mode(t->pool, i % 4);
        nostr_simple_pool_set_auto_unsub_on_eose(t->pool, i % 2 != 0);
        CHECK(nostr_simple_pool_set_recovery_window(t->pool, i % 3));
        CHECK(nostr_simple_pool_set_closed_backoff(t->pool, i % 2, 2));
        nostr_simple_pool_set_subscription_callback(t->pool, config_boundary, t);
        /* Bound log volume while still overlapping the first dial/redial. */
        if (i < 32) nostr_simple_pool_set_brown_list_enabled(t->pool, i % 2 != 0);
        i++;
    }
    nostr_simple_pool_set_auto_unsub_on_eose(t->pool, false);
    return NULL;
}

static void config_subscribe(PoolConfigTest *t, const char *url, bool unique,
                             bool async) {
    pthread_mutex_lock(&t->mutex);
    unsigned target = t->eose + 1;
    pthread_mutex_unlock(&t->mutex);
    NostrFilters *filters = kind1_filters();
    /* Alternate filter content, checking replacement ownership under TSan/ASan. */
    nostr_filter_set_limit(&filters->filters[0], unique ? 1 : 2);
    if (async) nostr_simple_pool_subscribe_async(t->pool, &url, 1, *filters, unique);
    else nostr_simple_pool_subscribe(t->pool, &url, 1, *filters, unique);
    nostr_filters_free(filters);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 10;
    pthread_mutex_lock(&t->mutex);
    while (t->eose < target)
        CHECK(pthread_cond_timedwait(&t->changed, &t->mutex, &deadline) == 0);
    pthread_mutex_unlock(&t->mutex);
}

static void *config_subscriber(void *arg) {
    PoolConfigTest *t = arg;
    for (unsigned i = 0; i < 32; i++)
        config_subscribe(t, t->url, i % 2 == 0, true);
    return NULL;
}

static void test_pool_config_concurrent(void) {
    unsetenv("NOSTR_TEST_MODE");
    char *sk = nostr_key_generate_private();
    CHECK(sk);
    NostrEvent *event = nostr_event_new();
    nostr_event_set_kind(event, 1);
    nostr_event_set_content(event, "pool configuration race fixture");
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    CHECK(nostr_event_sign(event, sk) == 0);
    char *json = nostr_event_serialize(event);
    CHECK(json);
    free(sk);
    nostr_event_free(event);
    fx_start_mode(json, true);
    free(json);
    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d", g_fx.port);
    PoolConfigTest *t = &config_test;
    CHECK(pthread_mutex_init(&t->mutex, NULL) == 0);
    CHECK(pthread_cond_init(&t->changed, NULL) == 0);
    t->pool = nostr_simple_pool_new();
    CHECK(t->pool);
    nostr_simple_pool_set_subscription_callback(t->pool, config_boundary, t);
    nostr_simple_pool_start(t->pool);
    snprintf(t->url, sizeof(t->url), "%s", url);
    config_set_mode(t->pool, 0);
    pthread_t writer, subscriber;
    CHECK(pthread_create(&writer, NULL, config_writer, t) == 0);
    /* With no competing live subscription, the fixture's first CLOSED must
     * be recovered by reconcile (the caller only sent one subscribe). */
    config_subscribe(t, url, false, true);
    CHECK(atomic_load(&g_fx.reqs_seen) >= 2);
    CHECK(pthread_create(&subscriber, NULL, config_subscriber, t) == 0);
    for (unsigned i = 0; i < 32; i++) config_subscribe(t, url, i % 2 != 0, false);
    CHECK(pthread_join(subscriber, NULL) == 0);
    atomic_store_explicit(&t->stop, true, memory_order_release);
    CHECK(pthread_join(writer, NULL) == 0);
    CHECK(nostr_simple_pool_set_closed_backoff(t->pool, 0, 0));
    for (unsigned mode = 0; mode < 4; mode++) {
        config_set_mode(t->pool, mode);
        config_subscribe(t, url, false, false);
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 10;
        pthread_mutex_lock(&t->mutex);
        while (!t->delivered[mode])
            CHECK(pthread_cond_timedwait(&t->changed, &t->mutex, &deadline) == 0);
        pthread_mutex_unlock(&t->mutex);
    }
    nostr_simple_pool_stop(t->pool);
    CHECK(t->eose >= 36 && t->closed > 0);
    nostr_simple_pool_free(t->pool);
    CHECK(atomic_load(&g_fx.reqs_seen) > 36);
    CHECK(atomic_load(&g_fx.protocol_headers_seen) == 0);
    fx_stop();
    pthread_cond_destroy(&t->changed);
    pthread_mutex_destroy(&t->mutex);
    puts("  [ok] concurrent pool config/subscribe + CLOSED reconcile + EVENT/EOSE");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "pool-config") == 0) {
        test_pool_config_concurrent();
        return 0;
    }
    printf("test_subscription_dispatch (nostrc-prqu.4)\n");
    test_offline_registration();
    test_loopback_fixture_relay();
    printf("test_subscription_dispatch: OK\n");
    return 0;
}
