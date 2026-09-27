/* Loopback regression: an inclusive reconnect REQ replays the boundary second,
 * but an already delivered ID must not be delivered again. */
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

#include "error.h"
#include "json.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-keys.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "select.h"
#include "../src/subscription-private.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    abort(); \
} } while (0)

typedef enum {
    FIXTURE_ORDERED,
    FIXTURE_NEWEST_FIRST,
    FIXTURE_EOSE_PRESSURE
} FixtureMode;

typedef struct {
    struct lws_context *ctx;
    pthread_t thread;
    atomic_bool running;
    atomic_int req_count;
    atomic_bool bad_cursor;
    int port;
    int64_t created_at;
    FixtureMode mode;
    char *events[4];
} Fixture;

typedef struct {
    char *queue[6];
    size_t count;
    bool close_after_queue;
} FixtureConn;

static Fixture fx;

static void enqueue(FixtureConn *c, const char *type, const char *sid,
                    int sid_len, const char *event_json) {
    size_t need = strlen(type) + (size_t)sid_len +
                  (event_json ? strlen(event_json) : 0) + 16;
    char *msg = malloc(need);
    CHECK(msg && c->count < 6);
    if (event_json)
        snprintf(msg, need, "[\"%s\",\"%.*s\",%s]", type, sid_len, sid, event_json);
    else
        snprintf(msg, need, "[\"%s\",\"%.*s\"]", type, sid_len, sid);
    c->queue[c->count++] = msg;
}

static int callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len) {
    FixtureConn *c = user;
    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        memset(c, 0, sizeof(*c));
        break;
    case LWS_CALLBACK_RECEIVE: {
        const char prefix[] = "[\"REQ\",\"";
        if (len <= sizeof(prefix) - 1 ||
            memcmp(in, prefix, sizeof(prefix) - 1) != 0) break;
        char *request = strndup(in, len);
        CHECK(request);
        const char *sid = request + sizeof(prefix) - 1;
        const char *end = strchr(sid, '"');
        CHECK(end);
        int sid_len = (int)(end - sid);
        int round = atomic_fetch_add(&fx.req_count, 1);
        if (fx.mode == FIXTURE_EOSE_PRESSURE) {
            if (round < 9) {
                enqueue(c, "EOSE", sid, sid_len, NULL);
                c->close_after_queue = true;
            } else if (round == 9) {
                enqueue(c, "EOSE", sid, sid_len, NULL);
                enqueue(c, "EVENT", sid, sid_len, fx.events[2]);
            }
        } else if (round == 0) {
            enqueue(c, "EVENT", sid, sid_len, fx.events[0]);
            enqueue(c, "EOSE", sid, sid_len, NULL);
            c->close_after_queue = true;
        } else if (round == 1) {
            const char *since = strstr(end, "\"since\":");
            long long cursor = -1;
            if (!since || sscanf(since, "\"since\":%lld", &cursor) != 1 ||
                cursor != fx.created_at)
                atomic_store(&fx.bad_cursor, true);
            if (fx.mode == FIXTURE_ORDERED) {
                if (cursor <= fx.created_at) {
                    enqueue(c, "EVENT", sid, sid_len, fx.events[0]);
                    enqueue(c, "EVENT", sid, sid_len, fx.events[1]);
                }
                enqueue(c, "EOSE", sid, sid_len, NULL);
                enqueue(c, "EVENT", sid, sid_len, fx.events[0]); /* live duplicate */
                enqueue(c, "EVENT", sid, sid_len, fx.events[2]); /* barrier */
            } else {
                /* Stored events newest-first: advancing to T+1 must not
                 * forget the already delivered boundary ID at T. */
                enqueue(c, "EVENT", sid, sid_len, fx.events[2]);
                if (cursor <= fx.created_at) {
                    enqueue(c, "EVENT", sid, sid_len, fx.events[1]);
                    enqueue(c, "EVENT", sid, sid_len, fx.events[0]);
                }
                enqueue(c, "EOSE", sid, sid_len, NULL);
                enqueue(c, "EVENT", sid, sid_len, fx.events[0]); /* live replay */
                enqueue(c, "EVENT", sid, sid_len, fx.events[3]); /* barrier */
            }
        }
        free(request);
        if (c->count) lws_callback_on_writable(wsi);
        break;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE:
        if (c->count) {
            char *msg = c->queue[0];
            memmove(&c->queue[0], &c->queue[1],
                    sizeof(c->queue[0]) * (c->count - 1));
            c->count--;
            size_t n = strlen(msg);
            unsigned char *buf = malloc(LWS_PRE + n);
            CHECK(buf);
            memcpy(buf + LWS_PRE, msg, n);
            int written = lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
            free(buf);
            free(msg);
            if (written != (int)n) return -1;
            if (c->count) lws_callback_on_writable(wsi);
            else if (c->close_after_queue) return -1;
        }
        break;
    case LWS_CALLBACK_CLOSED:
        for (size_t i = 0; i < c->count; i++) free(c->queue[i]);
        c->count = 0;
        break;
    default:
        break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    { "wss", callback, sizeof(FixtureConn), 128 * 1024, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

static void *service(void *unused) {
    (void)unused;
    while (atomic_load(&fx.running)) lws_service(fx.ctx, 50);
    return NULL;
}

static int free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    CHECK(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
    close(fd);
    return ntohs(addr.sin_port);
}

static void fixture_start(void) {
    fx.port = free_port();
    struct lws_context_creation_info info = {0};
    info.port = fx.port;
    info.iface = "127.0.0.1";
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    fx.ctx = lws_create_context(&info);
    CHECK(fx.ctx);
    atomic_store(&fx.running, true);
    CHECK(pthread_create(&fx.thread, NULL, service, NULL) == 0);
}

static void fixture_stop(void) {
    atomic_store(&fx.running, false);
    lws_cancel_service(fx.ctx);
    pthread_join(fx.thread, NULL);
    lws_context_destroy(fx.ctx);
}

static char *signed_event(const char *sk, int64_t created_at,
                          const char *content, char **id) {
    NostrEvent *ev = nostr_event_new();
    CHECK(ev);
    nostr_event_set_kind(ev, 1);
    nostr_event_set_created_at(ev, created_at);
    nostr_event_set_content(ev, content);
    CHECK(nostr_event_sign(ev, sk) == 0);
    *id = nostr_event_get_id(ev);
    char *json = nostr_event_serialize(ev);
    CHECK(*id && json);
    nostr_event_free(ev);
    return json;
}

static void *receive_for(GoChannel *channel, uint64_t timeout_ms) {
    void *value = NULL;
    GoSelectCase c = { .op = GO_SELECT_RECEIVE, .chan = channel,
                       .recv_buf = &value };
    GoSelectResult result = go_select_timeout(&c, 1, timeout_ms);
    if (result.selected_case != 0)
        fprintf(stderr, "receive timeout: requests=%d bad_cursor=%d\n",
                atomic_load(&fx.req_count), atomic_load(&fx.bad_cursor));
    CHECK(result.selected_case == 0);
    CHECK(!go_channel_is_closed(channel));
    return value;
}

static void *receive(GoChannel *channel) {
    return receive_for(channel, 10000);
}

static void expect_event_for(GoChannel *channel, const char *id,
                             uint64_t timeout_ms) {
    NostrEvent *ev = receive_for(channel, timeout_ms);
    CHECK(ev);
    CHECK(ev->id && strcmp(ev->id, id) == 0);
    nostr_event_free(ev);
}

static void expect_event(GoChannel *channel, const char *id) {
    expect_event_for(channel, id, 10000);
}

static void run_case(FixtureMode mode, char *ids[4]) {
    fx.mode = mode;
    atomic_store(&fx.req_count, 0);
    atomic_store(&fx.bad_cursor, false);
    fixture_start();

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d", fx.port);
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(go_context_background(), url, &err);
    CHECK(relay && !err);
    CHECK(nostr_relay_connect(relay, &err));
    CHECK(nostr_relay_wait_established(relay, 10000, &err));
    CHECK(!err);
    NostrFilters *filters = nostr_filters_new();
    NostrFilter *filter = nostr_filter_new();
    int kind = 1;
    nostr_filter_set_kinds(filter, &kind, 1);
    CHECK(nostr_filters_add(filters, filter));
    nostr_filter_free(filter);
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    CHECK(sub);
    CHECK(nostr_subscription_fire(sub, &err));
    CHECK(!err);

    if (mode == FIXTURE_EOSE_PRESSURE) {
        /* Nine disconnects with no EOSE consumer must not strand the reader
         * at the ninth signal. The tenth REQ supplies this event barrier. */
        expect_event_for(sub->events, ids[2], 30000);
        CHECK(atomic_load(&fx.req_count) == 10);
        CHECK(go_channel_get_depth(sub->end_of_stored_events) == 8);
    } else {
        expect_event(sub->events, ids[0]);
        (void)receive(sub->end_of_stored_events);
        if (mode == FIXTURE_NEWEST_FIRST) {
            expect_event(sub->events, ids[2]);
            expect_event(sub->events, ids[1]);
        } else {
            expect_event(sub->events, ids[1]);
        }
        (void)receive(sub->end_of_stored_events);
        if (mode == FIXTURE_ORDERED) expect_event(sub->events, ids[2]);
        else expect_event(sub->events, ids[3]);
        CHECK(atomic_load(&fx.req_count) == 2);
        CHECK(!atomic_load(&fx.bad_cursor));
        void *extra = NULL;
        CHECK(go_channel_try_receive(sub->events, &extra) != 0);
        CHECK(go_channel_try_receive(sub->end_of_stored_events, &extra) != 0);
    }

    nostr_subscription_unsubscribe(sub);
    nostr_subscription_free(sub);
    nostr_filters_free(filters);
    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    fixture_stop();
    printf("  [ok] reconnect case %d\n", (int)mode);
}

int main(void) {
    unsetenv("NOSTR_TEST_MODE");
    fx.created_at = 1700000000;
    char *sk = nostr_key_generate_private();
    CHECK(sk);
    char *ids[4];
    fx.events[0] = signed_event(sk, fx.created_at, "first", &ids[0]);
    fx.events[1] = signed_event(sk, fx.created_at, "second", &ids[1]);
    fx.events[2] = signed_event(sk, fx.created_at + 1, "next second", &ids[2]);
    fx.events[3] = signed_event(sk, fx.created_at + 2, "live barrier", &ids[3]);
    free(sk);
    CHECK(strcmp(ids[0], ids[1]) != 0);

    run_case(FIXTURE_ORDERED, ids);
    run_case(FIXTURE_NEWEST_FIRST, ids);
    run_case(FIXTURE_EOSE_PRESSURE, ids);

    for (int i = 0; i < 4; i++) { free(ids[i]); free(fx.events[i]); }
    puts("test_reconnect_same_second: OK");
    return 0;
}
