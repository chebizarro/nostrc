/* nostrc-flp7: a relay callback's user data outlives every invocation that
 * started before the callback was replaced or removed.
 *
 * libnostr copies a callback under the relay mutex and calls it after
 * unlocking. GNostrRelay freed its callback data right after removing the
 * callbacks, and a worker already past the lock read it afterwards (SIGSEGV
 * in on_core_state_changed -> g_weak_ref_get). With the _full setters the
 * relay owns the user data and destroys it only after the last such call has
 * returned; removing never waits for that call.
 *
 * Offline and deterministic: each callback blocks mid-call on a condition
 * variable until the test has removed or replaced it. The payload is freed by
 * the destroy notify and read by the callback after it resumes, so freeing it
 * early is a heap-use-after-free under ASan, and the ordering is also checked
 * directly. Every wait is bounded; the bound is a failure, never a pass. */
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nostr-envelope.h"
#include "nostr-relay.h"
#include "../src/relay-private.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    abort(); \
} } while (0)

#define WAIT_BOUND_S 10
#define PAYLOAD_MAGIC 0x666c7037 /* "flp7" */

typedef enum { KIND_STATE, KIND_AUTH, KIND_OK } Kind;

/* Test-owned record of what happened to one registration. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool block;          /* callback waits for `release` after `entered` */
    bool entered;
    bool release;
    bool returned;       /* set by the callback just before it returns */
    int calls;
    int destroyed;       /* destroy notify count */
    bool destroyed_after_return;
    bool remove_self;    /* callback removes its own registration */
    bool alive_after_self_remove;
} Probe;

/* The registered user data. Owned by the relay; freed by payload_destroy. */
typedef struct {
    int magic;
    Probe *probe;
    NostrRelay *relay;
    Kind kind;
} Payload;

static void probe_init(Probe *p, bool block) {
    memset(p, 0, sizeof *p);
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->block = block;
}

static void probe_clear(Probe *p) {
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
}

static void deadline_in(struct timespec *ts, int seconds) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += seconds;
}

/* Waits (bounded) until @flag is set; returns false on timeout. */
static bool probe_wait(Probe *p, bool *flag) {
    struct timespec deadline;
    deadline_in(&deadline, WAIT_BOUND_S);
    pthread_mutex_lock(&p->mu);
    int rc = 0;
    while (!*flag && rc == 0)
        rc = pthread_cond_timedwait(&p->cv, &p->mu, &deadline);
    bool ok = *flag;
    pthread_mutex_unlock(&p->mu);
    return ok;
}

static void probe_set(Probe *p, bool *flag) {
    pthread_mutex_lock(&p->mu);
    *flag = true;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

static int probe_destroyed(Probe *p) {
    pthread_mutex_lock(&p->mu);
    int n = p->destroyed;
    pthread_mutex_unlock(&p->mu);
    return n;
}

static Payload *payload_new(Probe *probe, NostrRelay *relay, Kind kind) {
    Payload *pl = calloc(1, sizeof *pl);
    CHECK(pl);
    pl->magic = PAYLOAD_MAGIC;
    pl->probe = probe;
    pl->relay = relay;
    pl->kind = kind;
    return pl;
}

static void payload_destroy(void *data) {
    Payload *pl = data;
    CHECK(pl->magic == PAYLOAD_MAGIC);
    Probe *p = pl->probe;
    pthread_mutex_lock(&p->mu);
    p->destroyed++;
    p->destroyed_after_return = p->returned;
    pthread_mutex_unlock(&p->mu);
    pl->magic = 0;
    free(pl);
}

/* Body shared by the three callbacks. */
static void payload_called(Payload *pl) {
    Probe *p = pl->probe;
    pthread_mutex_lock(&p->mu);
    p->calls++;
    pthread_mutex_unlock(&p->mu);

    if (p->block) {
        probe_set(p, &p->entered);
        CHECK(probe_wait(p, &p->release));
    }
    if (p->remove_self) {
        switch (pl->kind) {
        case KIND_STATE: nostr_relay_set_state_callback(pl->relay, NULL, NULL); break;
        case KIND_AUTH:  nostr_relay_set_auth_callback(pl->relay, NULL, NULL); break;
        case KIND_OK:    nostr_relay_set_ok_callback(pl->relay, NULL, NULL); break;
        }
        pthread_mutex_lock(&p->mu);
        p->alive_after_self_remove = (p->destroyed == 0);
        pthread_mutex_unlock(&p->mu);
    }
    /* The registration may be gone by now; its user data must not be. */
    CHECK(pl->magic == PAYLOAD_MAGIC);
    probe_set(p, &p->returned);
}

static void state_cb(NostrRelay *relay, NostrRelayConnectionState old_state,
                     NostrRelayConnectionState new_state, void *user_data) {
    (void)relay; (void)old_state; (void)new_state;
    payload_called(user_data);
}

static void auth_cb(NostrRelay *relay, const char *challenge, void *user_data) {
    (void)relay;
    CHECK(challenge && strcmp(challenge, "flp7-challenge") == 0);
    payload_called(user_data);
}

static void ok_cb(const char *event_id, bool ok, const char *reason, void *user_data) {
    CHECK(event_id && strlen(event_id) == 64);
    CHECK(ok);
    CHECK(reason);
    payload_called(user_data);
}

static void register_full(NostrRelay *relay, Kind kind, Payload *pl) {
    switch (kind) {
    case KIND_STATE: nostr_relay_set_state_callback_full(relay, state_cb, pl, payload_destroy); break;
    case KIND_AUTH:  nostr_relay_set_auth_callback_full(relay, auth_cb, pl, payload_destroy); break;
    case KIND_OK:    nostr_relay_set_ok_callback_full(relay, ok_cb, pl, payload_destroy); break;
    }
}

static void unregister(NostrRelay *relay, Kind kind) {
    switch (kind) {
    case KIND_STATE: nostr_relay_set_state_callback(relay, NULL, NULL); break;
    case KIND_AUTH:  nostr_relay_set_auth_callback(relay, NULL, NULL); break;
    case KIND_OK:    nostr_relay_set_ok_callback(relay, NULL, NULL); break;
    }
}

static void dispatch_frame(NostrRelay *relay, const char *json) {
    NostrEnvelope *env = nostr_envelope_parse(json);
    CHECK(env);
    nostr_relay_dispatch_control_envelope(relay, env);
    nostr_envelope_free(env);
}

/* Makes the relay invoke its @kind callback on the calling thread, the way
 * its worker threads do: state through relay_set_state() (a failed pool dial
 * moves the relay to BACKOFF), AUTH and OK through control-frame dispatch. */
static void invoke(NostrRelay *relay, Kind kind) {
    switch (kind) {
    case KIND_STATE:
        nostr_relay_dial_release(relay, false);
        break;
    case KIND_AUTH:
        dispatch_frame(relay, "[\"AUTH\",\"flp7-challenge\"]");
        break;
    case KIND_OK:
        dispatch_frame(relay, "[\"OK\",\"0000000000000000000000000000000000000000000000000000000000000001\",true,\"\"]");
        break;
    }
}

typedef struct { NostrRelay *relay; Kind kind; } InvokeArgs;

static void *invoke_thread(void *arg) {
    InvokeArgs *a = arg;
    invoke(a->relay, a->kind);
    return NULL;
}

static NostrRelay *relay_new(void) {
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(NULL, "ws://127.0.0.1:1", &err);
    CHECK(relay && !err);
    nostr_relay_set_auto_reconnect(relay, false);
    return relay;
}

static const char *kind_name(Kind kind) {
    return kind == KIND_STATE ? "state" : kind == KIND_AUTH ? "auth" : "ok";
}

/* The owner removes the callback while a worker is inside it: removal returns
 * at once, and the user data is destroyed only after that call returns. */
static void test_removed_mid_call(Kind kind) {
    NostrRelay *relay = relay_new();
    Probe probe;
    probe_init(&probe, true);
    register_full(relay, kind, payload_new(&probe, relay, kind));

    pthread_t worker;
    InvokeArgs args = { relay, kind };
    CHECK(pthread_create(&worker, NULL, invoke_thread, &args) == 0);
    CHECK(probe_wait(&probe, &probe.entered));

    /* Returning here while the worker is parked proves removal does not wait. */
    unregister(relay, kind);
    CHECK(probe_destroyed(&probe) == 0);

    probe_set(&probe, &probe.release);
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(probe.calls == 1);
    CHECK(probe.destroyed == 1);
    CHECK(probe.destroyed_after_return);

    nostr_relay_free(relay);
    CHECK(probe.destroyed == 1);
    probe_clear(&probe);
    fprintf(stderr, "ok - %s callback removed mid-call\n", kind_name(kind));
}

/* Replacing works like removing for the old registration, and later
 * invocations reach only the new one. */
static void test_replaced_mid_call(void) {
    NostrRelay *relay = relay_new();
    Probe first, second;
    probe_init(&first, true);
    probe_init(&second, false);
    register_full(relay, KIND_AUTH, payload_new(&first, relay, KIND_AUTH));

    pthread_t worker;
    InvokeArgs args = { relay, KIND_AUTH };
    CHECK(pthread_create(&worker, NULL, invoke_thread, &args) == 0);
    CHECK(probe_wait(&first, &first.entered));

    register_full(relay, KIND_AUTH, payload_new(&second, relay, KIND_AUTH));
    CHECK(probe_destroyed(&first) == 0);

    probe_set(&first, &first.release);
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(first.destroyed == 1 && first.destroyed_after_return);

    invoke(relay, KIND_AUTH);
    CHECK(first.calls == 1);
    CHECK(second.calls == 1 && second.destroyed == 0);

    nostr_relay_free(relay);
    CHECK(second.destroyed == 1);
    probe_clear(&first);
    probe_clear(&second);
    fprintf(stderr, "ok - auth callback replaced mid-call\n");
}

/* Freeing the relay releases whatever is still registered, once. */
static void test_free_releases_registrations(void) {
    NostrRelay *relay = relay_new();
    Probe probes[3];
    for (int k = KIND_STATE; k <= KIND_OK; k++) {
        probe_init(&probes[k], false);
        register_full(relay, (Kind)k, payload_new(&probes[k], relay, (Kind)k));
    }
    nostr_relay_free(relay);
    for (int k = KIND_STATE; k <= KIND_OK; k++) {
        CHECK(probes[k].calls == 0);
        CHECK(probes[k].destroyed == 1);
        probe_clear(&probes[k]);
    }
    fprintf(stderr, "ok - relay free releases registrations\n");
}

/* A callback may remove itself; its user data lives until it returns. */
static void test_removed_from_inside(void) {
    NostrRelay *relay = relay_new();
    Probe probe;
    probe_init(&probe, false);
    probe.remove_self = true;
    register_full(relay, KIND_OK, payload_new(&probe, relay, KIND_OK));

    invoke(relay, KIND_OK);
    CHECK(probe.calls == 1);
    CHECK(probe.alive_after_self_remove);
    CHECK(probe.destroyed == 1 && probe.destroyed_after_return);

    invoke(relay, KIND_OK); /* removed: not called again */
    CHECK(probe.calls == 1);
    nostr_relay_free(relay);
    CHECK(probe.destroyed == 1);
    probe_clear(&probe);
    fprintf(stderr, "ok - callback removes itself\n");
}

/* Nothing can hold the user data of a removal or of a NULL relay. */
static void test_unheld_user_data_destroyed_at_once(void) {
    NostrRelay *relay = relay_new();
    Probe a, b;
    probe_init(&a, false);
    probe_init(&b, false);
    nostr_relay_set_ok_callback_full(relay, NULL, payload_new(&a, relay, KIND_OK), payload_destroy);
    CHECK(a.destroyed == 1);
    nostr_relay_set_state_callback_full(NULL, state_cb, payload_new(&b, NULL, KIND_STATE), payload_destroy);
    CHECK(b.destroyed == 1);
    nostr_relay_free(relay);
    probe_clear(&a);
    probe_clear(&b);
    fprintf(stderr, "ok - unheld user data destroyed at once\n");
}

static int legacy_calls;

static void legacy_ok_cb(const char *event_id, bool ok, const char *reason, void *user_data) {
    (void)event_id; (void)ok; (void)reason;
    CHECK(user_data == &legacy_calls);
    legacy_calls++;
}

/* The original setters keep their meaning: the relay never owns the data. */
static void test_legacy_setters(void) {
    NostrRelay *relay = relay_new();
    nostr_relay_set_ok_callback(relay, legacy_ok_cb, &legacy_calls);
    invoke(relay, KIND_OK);
    CHECK(legacy_calls == 1);
    nostr_relay_set_ok_callback(relay, NULL, NULL);
    invoke(relay, KIND_OK);
    CHECK(legacy_calls == 1);
    nostr_relay_free(relay);
    fprintf(stderr, "ok - legacy setters\n");
}

int main(void) {
    test_removed_mid_call(KIND_STATE);
    test_removed_mid_call(KIND_AUTH);
    test_removed_mid_call(KIND_OK);
    test_replaced_mid_call();
    test_free_releases_registrations();
    test_removed_from_inside();
    test_unheld_user_data_destroyed_at_once();
    test_legacy_setters();
    return 0;
}
