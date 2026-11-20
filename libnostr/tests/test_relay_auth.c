/* nostrc-7i6w: NIP-42 session and SimplePool CLOSED recovery against a
 * private, in-process AUTH-required relay (libwebsockets on 127.0.0.1).
 * No public relays and no sleeps: every wait is for a protocol event, with a
 * generous deadline that only turns a hang into a failure.
 *
 * Cases (argv[1]):
 *   accept        ON_CHALLENGE signer, ["AUTH",...] on the wire, OK -> ACCEPTED
 *   reject-hold   OK false is held; explicit nostr_relay_auth_start() retries;
 *                 a re-challenge is answered again
 *   cancel        cancellation mid-sign: re-challenge, reconnect, signer
 *                 replaced, relay freed; a stale completion is never sent
 *   unanswered    an OK for another id is ignored; explicit start supersedes
 *   legacy        deprecated nostr_relay_auth_wait() and nostr_relay_send_auth()
 *                 go through the session (correlated OK, stale refusal)
 *   pool-auth     CLOSED auth-required -> ON_DEMAND AUTH -> exactly one reissue
 *   pool-reject   rejected AUTH holds the CLOSED REQ; explicit retry releases it
 *   pool-refused  auth-required after accepted AUTH: AUTH_REFUSED, one reissue
 *                 per later AUTH OK; ordering stamps
 *   pool-handrolled a caller's own AUTH frame (nostr_relay_write) is correlated
 *   pool-reconnect new connection generation: AUTH again, REQ recovered
 *   pool-blocked  blocked: yields one REQ per connection over N reconnects
 *   pool-backoff  rate-limited: reissued after the ladder delay
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
#ifndef LWS_PROTOCOL_LIST_TERM
#define LWS_PROTOCOL_LIST_TERM { .name = NULL, .callback = NULL }
#endif

#include "channel.h"
#include "error.h"
#include "json.h"
#include "nostr-envelope.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-keys.h"
#include "nostr-relay.h"
#include "nostr-simple-pool.h"
#include "nostr-subscription.h"
#include "nostr-tag.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    abort(); \
} } while (0)

/* ---- the private relay -------------------------------------------------- */

typedef enum {
    REQ_GATED,          /* auth-required: until authenticated, then EVENT+EOSE */
    REQ_REFUSE_ALWAYS,  /* auth-required: even after AUTH (identity not enough) */
    REQ_BLOCKED,        /* blocked: */
    REQ_RATE_ONCE       /* first REQ rate-limited:, then EVENT+EOSE */
} ReqPolicy;

enum { CMD_RECHALLENGE = 1, CMD_DROP = 2 };

typedef struct {
    struct lws_context *ctx;
    pthread_t thread;
    atomic_bool running;
    int port;
    char url[64];
    /* configuration, set by the test thread between protocol barriers */
    atomic_int policy;
    atomic_bool accept_auth;
    atomic_bool foreign_ok;    /* answer AUTH with an OK for another event id */
    atomic_int pending_cmd;
    /* observations */
    atomic_int connections;
    atomic_int reqs;
    atomic_int auths;          /* ["AUTH",...] frames received */
    atomic_int auths_valid;    /* ... that answer the connection's challenge */
    atomic_int auth_as_event;  /* kind 22242 inside ["EVENT",...]: must stay 0 */
    struct lws *active;        /* service thread only */
    char *event_json;          /* a signed kind-1 event to serve */
} Fixture;

typedef struct {
    char *queue[16];
    size_t count;
    bool close_now;
    bool authed;
    int serial;
    char challenge[64];
} Conn;

static Fixture fx;

static void push(struct lws *wsi, Conn *c, char *msg) {
    CHECK(msg && c->count < 16);
    c->queue[c->count++] = msg;
    lws_callback_on_writable(wsi);
}

static char *fmt(const char *f, const char *a, const char *b) {
    size_t n = strlen(f) + (a ? strlen(a) : 0) + (b ? strlen(b) : 0) + 1;
    char *m = malloc(n);
    CHECK(m);
    snprintf(m, n, f, a ? a : "", b ? b : "");
    return m;
}

static void challenge(struct lws *wsi, Conn *c) {
    static atomic_int counter;
    snprintf(c->challenge, sizeof(c->challenge), "challenge-%d-%d", c->serial,
             atomic_fetch_add(&counter, 1));
    c->authed = false;
    push(wsi, c, fmt("[\"AUTH\",\"%s\"]", c->challenge, NULL));
}

static const char *tag(NostrEvent *ev, const char *key) {
    for (size_t i = 0; ev->tags && i < nostr_tags_size(ev->tags); i++) {
        NostrTag *t = nostr_tags_get(ev->tags, i);
        if (t && nostr_tag_get_key(t) && !strcmp(nostr_tag_get_key(t), key))
            return nostr_tag_get_value(t);
    }
    return NULL;
}

static void on_auth_frame(struct lws *wsi, Conn *c, const char *text) {
    atomic_fetch_add(&fx.auths, 1);
    NostrEnvelope *env = nostr_envelope_parse(text);
    NostrEvent *ev = env && env->type == NOSTR_ENVELOPE_AUTH ? ((NostrAuthEnvelope *)env)->event : NULL;
    char id[65] = "";
    bool valid = ev && ev->kind == 22242 &&
                 nostr_event_validate(ev, id) == NOSTR_EVENT_VALIDATION_OK &&
                 tag(ev, "challenge") && !strcmp(tag(ev, "challenge"), c->challenge) &&
                 tag(ev, "relay") && !strcmp(tag(ev, "relay"), fx.url);
    if (valid) atomic_fetch_add(&fx.auths_valid, 1);
    if (valid && atomic_load(&fx.foreign_ok)) {
        push(wsi, c, fmt("[\"OK\",\"%s\",true,\"%s\"]",
                         "0000000000000000000000000000000000000000000000000000000000000001", ""));
        nostr_envelope_free(env);
        return;
    }
    bool ok = valid && atomic_load(&fx.accept_auth);
    if (ok) c->authed = true;
    if (ev)
        push(wsi, c, fmt(ok ? "[\"OK\",\"%s\",true,\"%s\"]" : "[\"OK\",\"%s\",false,\"%s\"]",
                         id, ok ? "" : valid ? "restricted: not on the list" : "invalid: bad auth"));
    if (env) nostr_envelope_free(env);
}

static void on_req(struct lws *wsi, Conn *c, const char *text) {
    const char *sid = text + strlen("[\"REQ\",\"");
    const char *end = strchr(sid, '"');
    CHECK(end && end - sid < 100);
    char id[128];
    memcpy(id, sid, (size_t)(end - sid));
    id[end - sid] = '\0';
    int n = atomic_fetch_add(&fx.reqs, 1);
    ReqPolicy policy = (ReqPolicy)atomic_load(&fx.policy);
    const char *closed = NULL;
    if (policy == REQ_BLOCKED) closed = "blocked: you are blocked";
    else if (policy == REQ_REFUSE_ALWAYS) closed = "auth-required: members only";
    else if (policy == REQ_GATED && !c->authed) closed = "auth-required: authenticate first";
    else if (policy == REQ_RATE_ONCE && n == 0) closed = "rate-limited: slow down";
    if (closed) {
        push(wsi, c, fmt("[\"CLOSED\",\"%s\",\"%s\"]", id, closed));
        return;
    }
    push(wsi, c, fmt("[\"EVENT\",\"%s\",%s]", id, fx.event_json));
    push(wsi, c, fmt("[\"EOSE\",\"%s\"]", id, NULL));
}

static int callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len) {
    Conn *c = user;
    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        memset(c, 0, sizeof(*c));
        c->serial = atomic_fetch_add(&fx.connections, 1) + 1;
        fx.active = wsi;
        challenge(wsi, c);
        break;
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
        int cmd = atomic_exchange(&fx.pending_cmd, 0);
        if (!cmd || !fx.active) break;
        Conn *a = lws_wsi_user(fx.active);
        if (cmd == CMD_RECHALLENGE) challenge(fx.active, a);
        if (cmd == CMD_DROP) { a->close_now = true; lws_callback_on_writable(fx.active); }
        break;
    }
    case LWS_CALLBACK_RECEIVE: {
        char *text = strndup(in, len);
        CHECK(text);
        if (!strncmp(text, "[\"REQ\",\"", 8)) on_req(wsi, c, text);
        else if (!strncmp(text, "[\"AUTH\",", 8)) on_auth_frame(wsi, c, text);
        else if (!strncmp(text, "[\"EVENT\",", 9) && strstr(text, "22242"))
            atomic_fetch_add(&fx.auth_as_event, 1);
        free(text);
        break;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE:
        if (c->close_now) return -1;
        if (c->count) {
            char *msg = c->queue[0];
            memmove(&c->queue[0], &c->queue[1], sizeof(c->queue[0]) * (c->count - 1));
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
        }
        break;
    case LWS_CALLBACK_CLOSED:
        for (size_t i = 0; i < c->count; i++) free(c->queue[i]);
        c->count = 0;
        if (fx.active == wsi) fx.active = NULL;
        break;
    default:
        break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    { "nostr", callback, sizeof(Conn), 128 * 1024, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

static void *service(void *unused) {
    (void)unused;
    while (atomic_load(&fx.running)) lws_service(fx.ctx, 0);
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

static void fixture_start(ReqPolicy policy, bool accept) {
    atomic_store(&fx.policy, (int)policy);
    atomic_store(&fx.accept_auth, accept);
    atomic_store(&fx.foreign_ok, false);
    atomic_store(&fx.pending_cmd, 0);
    atomic_store(&fx.connections, 0);
    atomic_store(&fx.reqs, 0);
    atomic_store(&fx.auths, 0);
    atomic_store(&fx.auths_valid, 0);
    atomic_store(&fx.auth_as_event, 0);
    fx.active = NULL;
    fx.port = free_port();
    snprintf(fx.url, sizeof(fx.url), "ws://127.0.0.1:%d", fx.port);
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

static void fixture_command(int cmd) {
    atomic_store(&fx.pending_cmd, cmd);
    lws_cancel_service(fx.ctx);
}

static void fixture_stop(void) {
    CHECK(atomic_load(&fx.auth_as_event) == 0);
    atomic_store(&fx.running, false);
    lws_cancel_service(fx.ctx);
    pthread_join(fx.thread, NULL);
    lws_context_destroy(fx.ctx);
}

/* ---- client-side observation ------------------------------------------- */

typedef struct {
    NostrRelayAuthState state;
    char event_id[65];
    char reason[128];
    NostrRelayFrameInfo frame;
    uint64_t identity;
} Result;

typedef struct {
    char state[8];
    char reason[128];
    NostrPoolCallbackInfo info;
} Boundary;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Result results[32];
    int n_results;
    Boundary boundaries[64];
    int n_boundaries;
    int events;
    NostrRelayFrameInfo last_ok_frame;
    int ok_frames;
    char *sk;
    char *pk;
    /* async signer */
    NostrRelayAuthRequest *held[8];
    int n_held;
    int cancels;
    int challenges;
    bool async;
} obs;

static void obs_reset(void) {
    pthread_mutex_lock(&obs.mu);
    for (int i = 0; i < obs.n_held; i++) nostr_relay_auth_request_unref(obs.held[i]);
    obs.n_held = obs.n_results = obs.n_boundaries = obs.events = obs.cancels = obs.ok_frames = 0;
    obs.challenges = 0;
    obs.async = false;
    pthread_mutex_unlock(&obs.mu);
}

/* Waits (bounded only to fail instead of hang) until @pred holds. */
#define WAIT_FOR(pred) do { \
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 20; \
    pthread_mutex_lock(&obs.mu); \
    while (!(pred)) { \
        int rc = pthread_cond_timedwait(&obs.cv, &obs.mu, &dl); \
        if (rc) { fprintf(stderr, "%s:%d: timed out waiting for %s (results=%d boundaries=%d reqs=%d auths=%d)\n", \
            __FILE__, __LINE__, #pred, obs.n_results, obs.n_boundaries, \
            atomic_load(&fx.reqs), atomic_load(&fx.auths)); abort(); } \
    } \
    pthread_mutex_unlock(&obs.mu); \
} while (0)

static void on_result(NostrRelay *relay, const NostrRelayAuthResult *r, void *data) {
    (void)relay; (void)data;
    pthread_mutex_lock(&obs.mu);
    CHECK(obs.n_results < 32);
    Result *o = &obs.results[obs.n_results++];
    o->state = r->state;
    snprintf(o->event_id, sizeof(o->event_id), "%s", r->event_id);
    snprintf(o->reason, sizeof(o->reason), "%s", r->reason);
    o->frame = r->frame;
    o->identity = r->identity_generation;
    fprintf(stderr, "[test] AUTH result %s event=%.8s reason=%s seq=%llu\n", nostr_relay_auth_state_name(r->state),
            r->event_id, r->reason, (unsigned long long)r->frame.sequence);
    pthread_cond_broadcast(&obs.cv);
    pthread_mutex_unlock(&obs.mu);
}

static void on_ok(const char *event_id, bool ok, const char *reason, void *data) {
    (void)event_id; (void)ok; (void)reason;
    NostrRelayFrameInfo f;
    CHECK(nostr_relay_get_callback_frame((NostrRelay *)data, &f));
    pthread_mutex_lock(&obs.mu);
    obs.last_ok_frame = f;
    obs.ok_frames++;
    pthread_cond_broadcast(&obs.cv);
    pthread_mutex_unlock(&obs.mu);
}

/* The session has already moved to CHALLENGED when this runs. */
static void on_challenge(NostrRelay *relay, const char *challenge, void *data) {
    (void)relay; (void)challenge; (void)data;
    pthread_mutex_lock(&obs.mu);
    obs.challenges++;
    pthread_cond_broadcast(&obs.cv);
    pthread_mutex_unlock(&obs.mu);
}

static void on_cancel(NostrRelayAuthRequest *req, void *data) {
    (void)req; (void)data;
    pthread_mutex_lock(&obs.mu);
    obs.cancels++;
    pthread_cond_broadcast(&obs.cv);
    pthread_mutex_unlock(&obs.mu);
}

static NostrEvent *sign_template(NostrRelayAuthRequest *req) {
    NostrEvent *ev = nostr_event_copy(nostr_relay_auth_request_get_event(req));
    CHECK(ev);
    CHECK(nostr_event_sign(ev, obs.sk) == 0);
    return ev;
}

static void sign_fn(NostrRelayAuthRequest *req, void *data) {
    (void)data;
    pthread_mutex_lock(&obs.mu);
    bool async = obs.async;
    if (async) {
        CHECK(obs.n_held < 8);
        obs.held[obs.n_held++] = nostr_relay_auth_request_ref(req);
        pthread_cond_broadcast(&obs.cv);
    }
    pthread_mutex_unlock(&obs.mu);
    if (async) {
        nostr_relay_auth_request_set_cancel_handler(req, on_cancel, NULL);
        return;
    }
    NostrEvent *ev = sign_template(req);
    (void)nostr_relay_auth_request_complete(req, ev, NULL);
    nostr_event_free(ev);
}

static atomic_int destroyed;
static void signer_destroy(void *data) { (void)data; atomic_fetch_add(&destroyed, 1); }

static void on_boundary(NostrRelay *relay, const char *state, const char *reason, void *data) {
    (void)relay;
    NostrSimplePool *pool = data;
    pthread_mutex_lock(&obs.mu);
    CHECK(obs.n_boundaries < 64);
    Boundary *b = &obs.boundaries[obs.n_boundaries++];
    snprintf(b->state, sizeof(b->state), "%s", state);
    snprintf(b->reason, sizeof(b->reason), "%s", reason ? reason : "");
    CHECK(nostr_simple_pool_get_callback_info(pool, &b->info));
    pthread_cond_broadcast(&obs.cv);
    pthread_mutex_unlock(&obs.mu);
}

static void on_event(NostrIncomingEvent *incoming, void *data) {
    (void)data;
    nostr_event_free(incoming->event);
    pthread_mutex_lock(&obs.mu);
    obs.events++;
    pthread_cond_broadcast(&obs.cv);
    pthread_mutex_unlock(&obs.mu);
}

static int count_boundaries(const char *state) {
    int n = 0;
    for (int i = 0; i < obs.n_boundaries; i++) n += !strcmp(obs.boundaries[i].state, state);
    return n;
}

static NostrRelay *relay_connect(void) {
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(NULL, fx.url, &err);
    CHECK(relay && !err);
    nostr_relay_set_auth_result_callback_full(relay, on_result, NULL, NULL);
    nostr_relay_set_ok_callback(relay, on_ok, relay);
    nostr_relay_set_auth_callback(relay, on_challenge, NULL);
    CHECK(nostr_relay_connect(relay, &err));
    CHECK(nostr_relay_wait_established(relay, 10000, &err));
    return relay;
}

static NostrRelayAuthStatus status_of(NostrRelay *relay) {
    NostrRelayAuthStatus st;
    CHECK(nostr_relay_get_auth_status(relay, &st));
    return st;
}

/* ---- relay-level cases -------------------------------------------------- */

static void test_accept(void) {
    fixture_start(REQ_GATED, true);
    NostrRelay *relay = relay_connect();
    CHECK(nostr_relay_set_auth_signer_full(relay, NOSTR_RELAY_AUTH_ON_CHALLENGE, 7, obs.pk,
                                           sign_fn, NULL, signer_destroy));
    WAIT_FOR(obs.n_results == 1);
    Result r = obs.results[0];
    CHECK(r.state == NOSTR_RELAY_AUTH_STATE_ACCEPTED && r.identity == 7);
    CHECK(r.frame.auth_state == NOSTR_RELAY_AUTH_STATE_SENT && r.frame.sequence >= 2);
    CHECK(r.frame.connection_generation == nostr_relay_get_connection_generation(relay));
    NostrRelayAuthStatus st = status_of(relay);
    CHECK(st.state == NOSTR_RELAY_AUTH_STATE_ACCEPTED && st.sequence == r.frame.sequence);
    CHECK(!strcmp(st.event_id, r.event_id));
    /* The OK callback saw the same frame; it runs after the session settled
     * (and after the result callback), so wait for it. */
    WAIT_FOR(obs.ok_frames == 1);
    pthread_mutex_lock(&obs.mu);
    CHECK(obs.ok_frames == 1 && obs.last_ok_frame.sequence == r.frame.sequence);
    pthread_mutex_unlock(&obs.mu);
    CHECK(atomic_load(&fx.auths) == 1 && atomic_load(&fx.auths_valid) == 1);
    Error *err = NULL;
    CHECK(!nostr_relay_auth_start(relay, &err) && err); /* already authenticated */
    free_error(err);
    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    CHECK(atomic_load(&destroyed) == 1);
    fixture_stop();
}

static void test_reject_hold(void) {
    fixture_start(REQ_GATED, false);
    NostrRelay *relay = relay_connect();
    CHECK(nostr_relay_set_auth_signer_full(relay, NOSTR_RELAY_AUTH_ON_CHALLENGE, 1, NULL,
                                           sign_fn, NULL, NULL));
    WAIT_FOR(obs.n_results == 1);
    CHECK(obs.results[0].state == NOSTR_RELAY_AUTH_STATE_REJECTED);
    CHECK(!strncmp(obs.results[0].reason, "restricted:", 11));
    CHECK(status_of(relay).state == NOSTR_RELAY_AUTH_STATE_REJECTED);
    /* Held: an auth-required refusal on this connection signs nothing. */
    NostrFilters *filters = nostr_filters_new();
    NostrFilter *filter = nostr_filter_new();
    int kind = 1;
    nostr_filter_set_kinds(filter, &kind, 1);
    CHECK(nostr_filters_add(filters, filter));
    nostr_filter_free(filter);
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    Error *err = NULL;
    CHECK(sub && nostr_subscription_fire(sub, &err));
    /* The explicit action retries; the fixture now accepts. If the refusal
     * above had re-signed, this would be the third AUTH, not the second. */
    atomic_store(&fx.accept_auth, true);
    CHECK(nostr_relay_auth_start(relay, &err));
    WAIT_FOR(obs.n_results == 2);
    CHECK(obs.results[1].state == NOSTR_RELAY_AUTH_STATE_ACCEPTED);
    CHECK(atomic_load(&fx.auths) == 2);
    /* A re-challenge is a new session: answered again (ON_CHALLENGE). */
    uint64_t generation = status_of(relay).auth_generation;
    fixture_command(CMD_RECHALLENGE);
    WAIT_FOR(obs.n_results == 3);
    CHECK(obs.results[2].state == NOSTR_RELAY_AUTH_STATE_ACCEPTED);
    CHECK(obs.results[2].frame.auth_generation > generation);
    CHECK(atomic_load(&fx.auths) == 3 && atomic_load(&fx.auths_valid) == 3);
    nostr_subscription_unsubscribe(sub);
    nostr_subscription_free(sub);
    nostr_filters_free(filters);
    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    fixture_stop();
}

/* An OK for another event id is not this AUTH's answer: the session stays
 * SENT. The explicit action supersedes an AUTH the relay never answers. */
static void test_unanswered(void) {
    fixture_start(REQ_GATED, true);
    atomic_store(&fx.foreign_ok, true);
    NostrRelay *relay = relay_connect();
    CHECK(nostr_relay_set_auth_signer_full(relay, NOSTR_RELAY_AUTH_ON_CHALLENGE, 1, NULL,
                                           sign_fn, NULL, NULL));
    WAIT_FOR(obs.ok_frames == 1);
    CHECK(obs.n_results == 0);
    CHECK(status_of(relay).state == NOSTR_RELAY_AUTH_STATE_SENT);
    atomic_store(&fx.foreign_ok, false);
    Error *err = NULL;
    CHECK(nostr_relay_auth_start(relay, &err));
    WAIT_FOR(obs.n_results == 1);
    CHECK(obs.results[0].state == NOSTR_RELAY_AUTH_STATE_ACCEPTED);
    CHECK(atomic_load(&fx.auths) == 2);
    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    fixture_stop();
}

static NostrRelayAuthRequest *held(int i) {
    pthread_mutex_lock(&obs.mu);
    NostrRelayAuthRequest *r = obs.held[i];
    pthread_mutex_unlock(&obs.mu);
    return r;
}

static void test_cancel(void) {
    fixture_start(REQ_GATED, true);
    obs.async = true;
    NostrRelay *relay = relay_connect();
    CHECK(nostr_relay_set_auth_signer_full(relay, NOSTR_RELAY_AUTH_ON_CHALLENGE, 1, NULL,
                                           sign_fn, NULL, NULL));
    WAIT_FOR(obs.n_held == 1);
    CHECK(status_of(relay).state == NOSTR_RELAY_AUTH_STATE_SIGNING);
    /* 1. A new challenge cancels the attempt for the old one. */
    fixture_command(CMD_RECHALLENGE);
    WAIT_FOR(obs.cancels == 1 && obs.n_held == 2);
    NostrRelayAuthRequest *stale = held(0);
    CHECK(nostr_relay_auth_request_is_cancelled(stale));
    NostrEvent *ev = sign_template(stale);
    CHECK(!nostr_relay_auth_request_complete(stale, ev, NULL)); /* never sent */
    nostr_event_free(ev);
    CHECK(strcmp(nostr_relay_auth_request_get_challenge(stale),
                 nostr_relay_auth_request_get_challenge(held(1))) != 0);
    /* 2. A reconnect (new connection generation) cancels the next one. */
    uint64_t conn = nostr_relay_get_connection_generation(relay);
    fixture_command(CMD_DROP);
    WAIT_FOR(obs.cancels == 2 && obs.n_held == 3);
    CHECK(nostr_relay_auth_request_get_connection_generation(held(2)) > conn);
    /* 3. Replacing the signer cancels it; the new identity signs at once. */
    obs.async = false;
    CHECK(nostr_relay_set_auth_signer_full(relay, NOSTR_RELAY_AUTH_ON_CHALLENGE, 2, NULL,
                                           sign_fn, NULL, NULL));
    WAIT_FOR(obs.cancels == 3 && obs.n_results == 1);
    CHECK(obs.results[0].state == NOSTR_RELAY_AUTH_STATE_ACCEPTED && obs.results[0].identity == 2);
    for (int i = 0; i < 3; i++) {
        ev = sign_template(held(i));
        CHECK(!nostr_relay_auth_request_complete(held(i), ev, NULL));
        nostr_event_free(ev);
    }
    CHECK(atomic_load(&fx.auths) == 1); /* only the current identity's answer */
    /* 4. Freeing the relay cancels a request the signer still holds; the
     * request outlives the relay and completing it is a no-op. */
    obs.async = true;
    fixture_command(CMD_RECHALLENGE);
    WAIT_FOR(obs.n_held == 4);
    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    WAIT_FOR(obs.cancels == 4);
    ev = sign_template(held(3));
    CHECK(!nostr_relay_auth_request_complete(held(3), ev, NULL));
    nostr_event_free(ev);
    fixture_stop();
}

static void legacy_sign(NostrEvent *ev, Error **err) {
    (void)err;
    CHECK(nostr_event_sign(ev, obs.sk) == 0);
}

static void test_legacy(void) {
    fixture_start(REQ_GATED, true);
    NostrRelay *relay = relay_connect();
    WAIT_FOR(obs.challenges == 1);
    Error *err = NULL;
    CHECK(nostr_relay_auth_wait(relay, legacy_sign, 10000, &err));
    CHECK(!err);
    WAIT_FOR(obs.n_results == 1);
    CHECK(obs.results[0].state == NOSTR_RELAY_AUTH_STATE_ACCEPTED);
    CHECK(status_of(relay).state == NOSTR_RELAY_AUTH_STATE_ACCEPTED);
    /* A caller-signed answer for a stale generation is refused locally. */
    uint64_t generation = 0;
    char *ch = nostr_relay_get_auth_challenge(relay, &generation);
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 22242);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, "");
    nostr_event_set_tags(ev, nostr_tags_new(2, nostr_tag_new("relay", fx.url, NULL),
                                            nostr_tag_new("challenge", ch, NULL)));
    CHECK(nostr_event_sign(ev, obs.sk) == 0);
    CHECK(!nostr_relay_send_auth(relay, ev, generation + 5, &err) && err);
    free_error(err); err = NULL;
    /* A wrong challenge is refused before the wire. */
    nostr_event_set_tags(ev, nostr_tags_new(2, nostr_tag_new("relay", fx.url, NULL),
                                            nostr_tag_new("challenge", "nope", NULL)));
    CHECK(nostr_event_sign(ev, obs.sk) == 0);
    CHECK(!nostr_relay_send_auth(relay, ev, 0, &err) && err);
    free_error(err); err = NULL;
    CHECK(atomic_load(&fx.auths) == 1);
    /* The right one goes, as AUTH, and is correlated. */
    nostr_event_set_tags(ev, nostr_tags_new(2, nostr_tag_new("relay", fx.url, NULL),
                                            nostr_tag_new("challenge", ch, NULL)));
    CHECK(nostr_event_sign(ev, obs.sk) == 0);
    CHECK(nostr_relay_send_auth(relay, ev, generation, &err));
    WAIT_FOR(obs.n_results == 2);
    CHECK(obs.results[1].state == NOSTR_RELAY_AUTH_STATE_ACCEPTED);
    CHECK(atomic_load(&fx.auths) == 2 && atomic_load(&fx.auths_valid) == 2);
    free(ch);
    nostr_event_free(ev);
    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    fixture_stop();
}

/* ---- pool cases --------------------------------------------------------- */

typedef struct {
    NostrSimplePool *pool;
    NostrRelay *relay;
} PoolCase;

static PoolCase pool_case(ReqPolicy policy, bool accept, NostrRelayAuthPolicy auth_policy,
                          bool signer) {
    fixture_start(policy, accept);
    PoolCase pc = {0};
    pc.relay = relay_connect();
    if (signer)
        CHECK(nostr_relay_set_auth_signer_full(pc.relay, auth_policy, 1, NULL, sign_fn, NULL, NULL));
    pc.pool = nostr_simple_pool_new();
    nostr_simple_pool_set_brown_list_enabled(pc.pool, false);
    nostr_simple_pool_set_subscription_callback(pc.pool, on_boundary, pc.pool);
    nostr_simple_pool_set_event_middleware_ex(pc.pool, on_event, NULL);
    nostr_simple_pool_add_relay(pc.pool, nostr_relay_ref(pc.relay));
    nostr_simple_pool_start(pc.pool);
    return pc;
}

static void pool_subscribe(PoolCase *pc) {
    NostrFilters filters = {0};
    NostrFilter *filter = nostr_filter_new();
    int kind = 1;
    nostr_filter_set_kinds(filter, &kind, 1);
    NostrFilters *fs = nostr_filters_new();
    CHECK(nostr_filters_add(fs, filter));
    nostr_filter_free(filter);
    filters = *fs;
    const char *url = fx.url;
    nostr_simple_pool_subscribe(pc->pool, &url, 1, filters, true);
    nostr_filters_free(fs);
}

static void pool_case_end(PoolCase *pc) {
    nostr_simple_pool_stop(pc->pool);
    nostr_simple_pool_free(pc->pool); /* drops the pool's reference */
    nostr_relay_close(pc->relay, NULL);
    nostr_relay_free(pc->relay);
    fixture_stop();
}

static Boundary boundary(int i) {
    pthread_mutex_lock(&obs.mu);
    Boundary b = obs.boundaries[i];
    pthread_mutex_unlock(&obs.mu);
    return b;
}

static void test_pool_auth(void) {
    PoolCase pc = pool_case(REQ_GATED, true, NOSTR_RELAY_AUTH_ON_DEMAND, true);
    pool_subscribe(&pc);
    WAIT_FOR(count_boundaries("EOSE") == 1 && obs.events == 1);
    Boundary closed = boundary(0);
    CHECK(!strcmp(closed.state, "CLOSED"));
    CHECK(closed.info.recovery == NOSTR_POOL_RECOVERY_AWAIT_AUTH);
    CHECK(closed.info.frame.auth_state == NOSTR_RELAY_AUTH_STATE_CHALLENGED);
    WAIT_FOR(obs.n_results == 1);
    CHECK(obs.results[0].state == NOSTR_RELAY_AUTH_STATE_ACCEPTED);
    /* Wire order: CLOSED < AUTH OK < the reissued REQ's EOSE. */
    CHECK(closed.info.frame.sequence < obs.results[0].frame.sequence);
    Boundary eose = boundary(1);
    CHECK(!strcmp(eose.state, "EOSE") && eose.info.recovery == NOSTR_POOL_RECOVERY_NONE);
    CHECK(eose.info.frame.sequence > obs.results[0].frame.sequence);
    CHECK(eose.info.frame.auth_state == NOSTR_RELAY_AUTH_STATE_ACCEPTED);
    CHECK(atomic_load(&fx.reqs) == 2 && atomic_load(&fx.auths) == 1);
    pool_case_end(&pc);
}

static void test_pool_reject(void) {
    PoolCase pc = pool_case(REQ_GATED, false, NOSTR_RELAY_AUTH_ON_DEMAND, true);
    pool_subscribe(&pc);
    WAIT_FOR(obs.n_results == 1 && obs.n_boundaries == 1);
    CHECK(obs.results[0].state == NOSTR_RELAY_AUTH_STATE_REJECTED);
    CHECK(boundary(0).info.recovery == NOSTR_POOL_RECOVERY_AWAIT_AUTH);
    /* Held after rejection. The explicit retry is accepted and releases the
     * REQ once: a re-REQ on rejection would make this the third REQ. */
    atomic_store(&fx.accept_auth, true);
    Error *err = NULL;
    CHECK(nostr_relay_auth_start(pc.relay, &err));
    WAIT_FOR(count_boundaries("EOSE") == 1);
    CHECK(atomic_load(&fx.reqs) == 2 && atomic_load(&fx.auths) == 2);
    pool_case_end(&pc);
}

static void test_pool_refused(void) {
    PoolCase pc = pool_case(REQ_REFUSE_ALWAYS, true, NOSTR_RELAY_AUTH_ON_DEMAND, true);
    pool_subscribe(&pc);
    /* REQ1 auth-required -> AUTH OK -> exactly one reissue -> REQ2 refused
     * after AUTH: the stamp says so, and it is held. */
    WAIT_FOR(obs.n_boundaries == 2);
    Boundary first = boundary(0), second = boundary(1);
    CHECK(first.info.recovery == NOSTR_POOL_RECOVERY_AWAIT_AUTH);
    CHECK(second.info.recovery == NOSTR_POOL_RECOVERY_AUTH_REFUSED);
    CHECK(second.info.frame.auth_state == NOSTR_RELAY_AUTH_STATE_ACCEPTED);
    CHECK(first.info.frame.sequence < obs.results[0].frame.sequence &&
          obs.results[0].frame.sequence < second.info.frame.sequence);
    CHECK(second.info.frame.auth_generation == obs.results[0].frame.auth_generation);
    CHECK(atomic_load(&fx.reqs) == 2);
    /* A new challenge alone (ON_DEMAND, no new refusal) signs nothing; the
     * explicit AUTH for it is the next OK, releasing exactly one REQ. */
    fixture_command(CMD_RECHALLENGE);
    WAIT_FOR(obs.challenges == 2);
    CHECK(status_of(pc.relay).auth_generation > second.info.frame.auth_generation);
    CHECK(status_of(pc.relay).state == NOSTR_RELAY_AUTH_STATE_CHALLENGED);
    Error *err = NULL;
    CHECK(nostr_relay_auth_start(pc.relay, &err));
    WAIT_FOR(obs.n_boundaries == 3);
    CHECK(boundary(2).info.recovery == NOSTR_POOL_RECOVERY_AUTH_REFUSED);
    CHECK(atomic_load(&fx.reqs) == 3 && atomic_load(&fx.auths) == 2);
    pool_case_end(&pc);
}

/* A caller that writes its own ["AUTH",...] (metadesk md-xcy today) still
 * gets the pool's recovery: the session observes the frame it wrote. */
static void test_pool_handrolled(void) {
    PoolCase pc = pool_case(REQ_GATED, true, NOSTR_RELAY_AUTH_ON_CHALLENGE, false);
    pool_subscribe(&pc);
    WAIT_FOR(obs.n_boundaries == 1 && obs.challenges == 1);
    CHECK(boundary(0).info.recovery == NOSTR_POOL_RECOVERY_AWAIT_AUTH);
    char *ch = nostr_relay_get_auth_challenge(pc.relay, NULL);
    CHECK(ch);
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 22242);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, "");
    nostr_event_set_tags(ev, nostr_tags_new(2, nostr_tag_new("relay", fx.url, NULL),
                                            nostr_tag_new("challenge", ch, NULL)));
    CHECK(nostr_event_sign(ev, obs.sk) == 0);
    NostrAuthEnvelope auth = { .base.type = NOSTR_ENVELOPE_AUTH, .event = ev };
    char *wire = nostr_envelope_serialize((NostrEnvelope *)&auth);
    CHECK(wire);
    GoChannel *answer = nostr_relay_write(pc.relay, wire);
    CHECK(answer);
    go_channel_close(answer);
    void *e = NULL;
    while (go_channel_try_receive(answer, &e) == 0) { if (e) free_error(e); e = NULL; }
    go_channel_unref(answer);
    WAIT_FOR(count_boundaries("EOSE") == 1 && obs.n_results == 1);
    CHECK(obs.results[0].state == NOSTR_RELAY_AUTH_STATE_ACCEPTED && obs.results[0].identity == 0);
    CHECK(atomic_load(&fx.reqs) == 2 && atomic_load(&fx.auths) == 1);
    free(wire);
    free(ch);
    nostr_event_free(ev);
    pool_case_end(&pc);
}

static void test_pool_reconnect(void) {
    PoolCase pc = pool_case(REQ_GATED, true, NOSTR_RELAY_AUTH_ON_CHALLENGE, true);
    pool_subscribe(&pc);
    WAIT_FOR(count_boundaries("EOSE") == 1 && obs.n_results == 1);
    uint64_t conn = nostr_relay_get_connection_generation(pc.relay);
    int reqs = atomic_load(&fx.reqs);
    CHECK(reqs <= 2);
    fixture_command(CMD_DROP);
    /* The new connection authenticates again and its REQ is answered. */
    WAIT_FOR(obs.n_results == 2 && count_boundaries("EOSE") == 2);
    CHECK(obs.results[1].frame.connection_generation > conn);
    Boundary last = obs.boundaries[obs.n_boundaries - 1];
    CHECK(last.info.frame.connection_generation == obs.results[1].frame.connection_generation);
    CHECK(atomic_load(&fx.connections) == 2);
    CHECK(atomic_load(&fx.reqs) - reqs <= 2); /* at most: refused once, reissued once */
    pool_case_end(&pc);
}

static void test_pool_blocked(void) {
    enum { RECONNECTS = 3 };
    PoolCase pc = pool_case(REQ_BLOCKED, true, NOSTR_RELAY_AUTH_ON_CHALLENGE, false);
    pool_subscribe(&pc);
    for (int round = 0; round <= RECONNECTS; round++) {
        WAIT_FOR(obs.n_boundaries == round + 1);
        Boundary b = boundary(round);
        CHECK(!strcmp(b.state, "CLOSED") && b.info.recovery == NOSTR_POOL_RECOVERY_RECONNECT);
        CHECK(atomic_load(&fx.reqs) == round + 1); /* one REQ per connection */
        if (round < RECONNECTS) fixture_command(CMD_DROP);
    }
    /* Barrier: a new subscribe() is the next REQ; a timer re-REQ in between
     * would have made this count larger. */
    pool_subscribe(&pc);
    WAIT_FOR(obs.n_boundaries == RECONNECTS + 2);
    CHECK(atomic_load(&fx.reqs) == RECONNECTS + 2);
    CHECK(atomic_load(&fx.connections) == RECONNECTS + 1);
    pool_case_end(&pc);
}

static void test_pool_backoff(void) {
    PoolCase pc = pool_case(REQ_RATE_ONCE, true, NOSTR_RELAY_AUTH_ON_CHALLENGE, false);
    CHECK(nostr_simple_pool_set_closed_backoff(pc.pool, 50, 100));
    pool_subscribe(&pc);
    WAIT_FOR(count_boundaries("EOSE") == 1);
    Boundary b = boundary(0);
    CHECK(b.info.recovery == NOSTR_POOL_RECOVERY_BACKOFF && b.info.retry_after_ms > 0 &&
          b.info.retry_after_ms <= 50);
    CHECK(atomic_load(&fx.reqs) == 2);
    pool_case_end(&pc);
}

int main(int argc, char **argv) {
    unsetenv("NOSTR_TEST_MODE");
    lws_set_log_level(LLL_ERR, NULL);
    pthread_mutex_init(&obs.mu, NULL);
    pthread_cond_init(&obs.cv, NULL);
    obs.sk = nostr_key_generate_private();
    obs.pk = nostr_key_get_public(obs.sk);
    CHECK(obs.sk && obs.pk);
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, 1);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, "protected");
    CHECK(nostr_event_sign(ev, obs.sk) == 0);
    fx.event_json = nostr_event_serialize(ev);
    nostr_event_free(ev);

    struct { const char *name; void (*fn)(void); } cases[] = {
        { "accept", test_accept }, { "reject-hold", test_reject_hold },
        { "cancel", test_cancel }, { "legacy", test_legacy },
        { "unanswered", test_unanswered },
        { "pool-auth", test_pool_auth }, { "pool-reject", test_pool_reject },
        { "pool-refused", test_pool_refused }, { "pool-handrolled", test_pool_handrolled }, { "pool-reconnect", test_pool_reconnect },
        { "pool-blocked", test_pool_blocked }, { "pool-backoff", test_pool_backoff },
    };
    int ran = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) continue;
        obs_reset();
        cases[i].fn();
        obs_reset();
        printf("  [ok] %s\n", cases[i].name);
        ran++;
    }
    CHECK(ran > 0);
    free(fx.event_json);
    free(obs.sk);
    free(obs.pk);
    puts("test_relay_auth: OK");
    return 0;
}
