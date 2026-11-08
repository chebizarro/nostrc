/* Real native subscription producers, deterministic consumer/producer barriers,
 * and the shipped pool worker. Interposition stays in this test TU only. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "channel.h"
#include "select.h"
#include "nostr-simple-pool.h"
#include "nostr-subscription.h"
#include "subscription-private.h"

static NostrOrderedKind test_next(NostrSubscription *sub, NostrOrderedDelivery *out);
static bool test_fire(NostrSubscription *sub, Error **err);
static GoSelectResult test_select_timeout(GoSelectCase *cases, size_t count, uint64_t timeout);
#define nostr_subscription_next_ordered test_next
#define nostr_subscription_fire test_fire
#define go_select_timeout test_select_timeout
#include "../src/simplepool.c"
#undef go_select_timeout
#undef nostr_subscription_fire
#undef nostr_subscription_next_ordered

static _Thread_local int query_selection = -1;
static _Thread_local NostrSimplePool *stop_after_select;
static _Thread_local NostrSubscription *live_sub;
static _Thread_local int live_sequence;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    NostrSubscription *sub;
    bool armed, entered, completed, refire;
    int trigger; /* 0: empty receive, otherwise the EVENT's sequence */
    int first, last, trailing;
    const char *state;
} ProducerBarrier;
static ProducerBarrier barrier;

static void enqueue_event(NostrSubscription *sub, int sequence) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    nostr_event_set_created_at(ev, sequence);
    nostr_event_set_kind(ev, 1);
    nostr_event_set_pubkey(ev, "0000000000000000000000000000000000000000000000000000000000000001");
    char content[32];
    snprintf(content, sizeof(content), "event-%d", sequence);
    nostr_event_set_content(ev, content);
    ev->id = nostr_event_get_id(ev);
    assert(ev->id);
    nostr_subscription_dispatch_event(sub, ev); /* native ownership transfer */
}

static void enqueue_boundary(NostrSubscription *sub, const char *state) {
    if (strcmp(state, "EOSE") == 0)
        nostr_subscription_dispatch_eose(sub);
    else
        nostr_subscription_dispatch_closed(sub, "error: fixture closed");
}

static void refire(NostrSubscription *sub) {
    int64_t since = nostr_subscription_prepare_refire(sub);
    Error *err = NULL;
    assert(since > 0);
    assert(nostr_subscription_refire_since(sub, since, &err));
    assert(!err);
}

static void *produce_at_barrier(void *unused) {
    (void)unused;
    pthread_mutex_lock(&barrier.mutex);
    while (!barrier.entered) pthread_cond_wait(&barrier.cond, &barrier.mutex);
    pthread_mutex_unlock(&barrier.mutex);
    if (barrier.refire) refire(barrier.sub);
    for (int i = barrier.first; i <= barrier.last; i++) enqueue_event(barrier.sub, i);
    enqueue_boundary(barrier.sub, barrier.state);
    if (barrier.trailing) enqueue_event(barrier.sub, barrier.trailing);
    pthread_mutex_lock(&barrier.mutex);
    barrier.completed = true;
    pthread_cond_broadcast(&barrier.cond);
    pthread_mutex_unlock(&barrier.mutex);
    nostr_subscription_unref(barrier.sub);
    return NULL;
}

static void arm_barrier(NostrSubscription *sub, int trigger, int first, int last,
                        const char *state, int trailing, bool do_refire) {
    memset(&barrier, 0, sizeof(barrier));
    pthread_mutex_init(&barrier.mutex, NULL);
    pthread_cond_init(&barrier.cond, NULL);
    barrier.sub = nostr_subscription_ref(sub);
    barrier.armed = true;
    barrier.trigger = trigger;
    barrier.first = first;
    barrier.last = last;
    barrier.trailing = trailing;
    barrier.state = state;
    barrier.refire = do_refire;
    assert(pthread_create(&barrier.thread, NULL, produce_at_barrier, NULL) == 0);
}

static void join_barrier(void) {
    assert(pthread_join(barrier.thread, NULL) == 0);
    assert(barrier.entered && barrier.completed && !barrier.armed);
    pthread_cond_destroy(&barrier.cond);
    pthread_mutex_destroy(&barrier.mutex);
}

static NostrOrderedKind test_next(NostrSubscription *sub, NostrOrderedDelivery *out) {
    NostrOrderedKind kind = nostr_subscription_next_ordered(sub, out);
    if (barrier.armed && sub == barrier.sub &&
        ((!barrier.trigger && kind == NOSTR_ORDERED_NONE) ||
         (kind == NOSTR_ORDERED_EVENT && out->event->created_at == barrier.trigger))) {
        barrier.armed = false;
        /* The real receive already returned, but pool_drain_all has not yet
         * collected it. The other thread publishes through the real native
         * producer before this existing bounded drain may receive again. */
        pthread_mutex_lock(&barrier.mutex);
        barrier.entered = true;
        pthread_cond_broadcast(&barrier.cond);
        while (!barrier.completed) pthread_cond_wait(&barrier.cond, &barrier.mutex);
        pthread_mutex_unlock(&barrier.mutex);
    }
    if (live_sub == sub && kind == NOSTR_ORDERED_EVENT) {
        assert(live_sequence < 4096); /* fail finitely if a drain chases live traffic */
        enqueue_event(sub, ++live_sequence);
    }
    return kind;
}

static bool test_fire(NostrSubscription *sub, Error **err) {
    bool ok = nostr_subscription_fire(sub, err);
    if (ok && query_selection >= 0) {
        if (query_selection == 0 || query_selection == 3) enqueue_event(sub, 1);
        if (query_selection != 0)
            enqueue_boundary(sub, query_selection == 2 ? "CLOSED" : "EOSE");
    }
    return ok;
}

static GoSelectResult test_select_timeout(GoSelectCase *cases, size_t count, uint64_t timeout) {
    if (!stop_after_select) return go_select_timeout(cases, count, timeout);
    assert(count == 1 && cases[0].chan == stop_after_select->wake_ch);
    assert(go_channel_try_receive(cases[0].chan, NULL) == 0);
    atomic_store(&stop_after_select->running, false);
    stop_after_select = NULL;
    return (GoSelectResult){ .selected_case = 0 };
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    NostrSimplePool *pool;
    GoContext *context;
    int events, boundaries, expected;
    size_t expected_subs;
    int events_at_boundary;
    int boundary_prefix[4];
    const char *boundary_state[4];
    int live_expected[1025]; /* -1: not asserted, 0: history/retired, 1: live */
    const char *state;
} Observations;

static void event_received(NostrIncomingEvent *incoming, void *data) {
    Observations *o = data;
    const NostrSubscription *source = nostr_simple_pool_get_callback_subscription(o->pool);
    int64_t live_since = nostr_simple_pool_get_callback_live_since(o->pool);
    if (!o->pool->batch_middleware) {
        assert(source);
        if (o->pool->subs_count) assert(source == o->pool->subs[0]);
    } else {
        assert(source == NULL && live_since == 0);
    }
    pthread_mutex_lock(&o->mutex);
    int sequence = (int)nostr_event_get_created_at(incoming->event);
    assert(sequence == o->events + 1);
    if (sequence < 1025 && o->live_expected[sequence] >= 0) {
        assert((live_since > 0) == (o->live_expected[sequence] == 1));
        if (live_since > 0) assert(o->boundaries > 0);
    }
    o->events++;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->mutex);
    nostr_event_free(incoming->event);
}

static void boundary_received(NostrRelay *relay, const char *state,
                              const char *reason, void *data) {
    (void)relay;
    Observations *o = data;
    const NostrSubscription *source = nostr_simple_pool_get_callback_subscription(o->pool);
    assert(source);
    if (o->pool->subs_count) assert(source == o->pool->subs[0]);
    pthread_mutex_lock(&o->mutex);
    assert(o->boundaries < 4);
    const char *expected_state = o->boundary_state[o->boundaries];
    assert(strcmp(state, expected_state ? expected_state : o->state) == 0);
    o->events_at_boundary = o->events;
    int expected = o->boundary_prefix[o->boundaries];
    assert(o->events == (expected >= 0 ? expected : o->expected));
    assert(o->pool->subs_count == o->expected_subs); /* callback before prune/free */
    if (strcmp(state, "CLOSED") == 0) {
        assert(reason && strcmp(reason, "error: fixture closed") == 0);
        assert(nostr_simple_pool_get_callback_live_since(o->pool) == 0);
    }
    o->boundaries++;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->mutex);
}

static NostrSubscription *setup(Observations *o, int expected, const char *state) {
    memset(o, 0, sizeof(*o));
    pthread_mutex_init(&o->mutex, NULL);
    pthread_cond_init(&o->cond, NULL);
    for (size_t i = 0; i < 1025; i++) o->live_expected[i] = -1;
    for (size_t i = 0; i < 4; i++) o->boundary_prefix[i] = -1;
    o->expected = expected;
    o->expected_subs = 1;
    o->state = state;
    o->pool = nostr_simple_pool_new();
    assert(o->pool);
    assert(nostr_simple_pool_get_callback_subscription(o->pool) == NULL);
    assert(nostr_simple_pool_get_callback_live_since(o->pool) == 0);
    nostr_simple_pool_set_event_middleware_ex(o->pool, event_received, o);
    nostr_simple_pool_set_auto_unsub_on_eose(o->pool, true);
    nostr_simple_pool_set_subscription_callback(o->pool, boundary_received, o);
    Error *err = NULL;
    o->context = go_context_background();
    NostrRelay *relay = nostr_relay_new(o->context, "wss://chqxa.invalid", &err);
    assert(relay && !err);
    assert(nostr_relay_connect(relay, &err) && !err); /* native offline test transport */
    NostrFilters *filters = nostr_filters_new();
    NostrFilter filter = {0};
    assert(filters && nostr_filters_add(filters, &filter));
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    assert(sub);
    nostr_subscription_set_filters(sub, filters);
    nostr_subscription_set_duplicate_delivery(sub, true);
    assert(nostr_subscription_enable_ordered(sub, o->pool->wake_ch));
    assert(nostr_subscription_fire(sub, &err) && !err);
    nostr_relay_unref(relay);
    o->pool->subs = malloc(sizeof(sub));
    assert(o->pool->subs);
    o->pool->subs[0] = sub;
    o->pool->subs_count = 1;
    return sub;
}

static void teardown(Observations *o) {
    nostr_simple_pool_free(o->pool);
    go_context_unref(o->context);
    pthread_cond_destroy(&o->cond);
    pthread_mutex_destroy(&o->mutex);
}

static void drain(Observations *o) {
    size_t count = o->pool->subs_count;
    NostrSubscription *snapshot[1] = {count ? o->pool->subs[0] : NULL};
    PoolIncomingEvent *batch = NULL;
    size_t len = 0, cap = 0;
    pool_drain_all(o->pool, snapshot, count, 256, &batch, &len, &cap);
    pool_deliver_batch(o->pool, batch, &len);
    free(batch);
    assert(nostr_simple_pool_get_callback_subscription(o->pool) == NULL);
    assert(nostr_simple_pool_get_callback_live_since(o->pool) == 0);
}

static void test_boundary(const char *state, bool race, int count) {
    Observations o;
    NostrSubscription *sub = setup(&o, count, state);
    if (race) {
        arm_barrier(sub, 0, 1, count, state, 0, false);
    } else {
        for (int i = 1; i <= count; i++) enqueue_event(sub, i);
        enqueue_event(sub, count); /* native duplicate accepted; pool dedup preserves fence */
        enqueue_boundary(sub, state);
    }
    drain(&o);
    drain(&o);
    if (race) join_barrier();
    assert(o.events == count && o.boundaries == 1 && o.events_at_boundary == count);
    assert(o.pool->subs_count == 0);
    teardown(&o);
}

static void test_wire_order(bool mid_drain, int history) {
    Observations o;
    NostrSubscription *sub = setup(&o, history, "EOSE");
    nostr_simple_pool_set_auto_unsub_on_eose(o.pool, false);
    for (int i = 1; i <= history; i++) {
        enqueue_event(sub, i);
        o.live_expected[i] = 0;
    }
    o.live_expected[history + 1] = 1;
    if (mid_drain) arm_barrier(sub, 1, 1, 0, "EOSE", history + 1, false);
    else {
        enqueue_boundary(sub, "EOSE");
        enqueue_event(sub, history + 1);
    }
    drain(&o);
    if (history > 256) assert(o.events == 256 && o.boundaries == 0);
    else assert(o.events == history + 1 && o.boundaries == 1);
    drain(&o);
    if (mid_drain) join_barrier();
    assert(o.events == history + 1 && o.boundaries == 1 && o.events_at_boundary == history);
    assert(o.pool->subs_count == 1);
    teardown(&o);
}

static void test_closed_mid_drain(void) {
    Observations o;
    NostrSubscription *sub = setup(&o, 1, "CLOSED");
    enqueue_event(sub, 1);
    o.live_expected[1] = 0;
    arm_barrier(sub, 1, 1, 0, "CLOSED", 2, false);
    drain(&o);
    join_barrier();
    assert(o.events == 1 && o.boundaries == 1 && o.pool->subs_count == 0);
    teardown(&o);
}

static void test_mixed_fences(bool closed_refire) {
    Observations o;
    NostrSubscription *sub = setup(&o, closed_refire ? 2 : 1, closed_refire ? "CLOSED" : "EOSE");
    nostr_simple_pool_set_auto_unsub_on_eose(o.pool, false);
    enqueue_event(sub, 1);
    enqueue_boundary(sub, "EOSE");
    enqueue_event(sub, 2);
    enqueue_boundary(sub, "CLOSED");
    if (closed_refire) refire(sub);
    enqueue_boundary(sub, "EOSE"); /* duplicate or terminal CLOSED: cannot create a new fence */
    enqueue_event(sub, 3); /* cannot revive a terminal subscription */
    if (!closed_refire) {
        o.boundary_prefix[1] = 2;
        o.boundary_state[1] = "CLOSED";
    }
    o.live_expected[1] = o.live_expected[2] = 0; /* already CLOSED when callbacks run */
    drain(&o);
    assert(o.events == 2 && o.boundaries == (closed_refire ? 1 : 2));
    assert(o.pool->subs_count == 0);
    teardown(&o);
}

static void test_refire(bool in_flight) {
    Observations o;
    NostrSubscription *sub = setup(&o, 3, "EOSE");
    nostr_simple_pool_set_auto_unsub_on_eose(o.pool, false);
    enqueue_event(sub, 1);
    enqueue_boundary(sub, "EOSE");
    enqueue_event(sub, 2);
    if (in_flight) {
        o.boundary_prefix[0] = 1;
        o.boundary_prefix[1] = 3;
        arm_barrier(sub, 2, 3, 3, "EOSE", 4, true);
    } else {
        refire(sub);
        enqueue_event(sub, 3);
        enqueue_boundary(sub, "EOSE");
        enqueue_event(sub, 4);
    }
    for (int i = 1; i <= 3; i++) o.live_expected[i] = 0;
    o.live_expected[4] = 1;
    drain(&o);
    if (in_flight) join_barrier();
    assert(o.events == 4 && o.boundaries == (in_flight ? 2 : 1));
    assert(o.events_at_boundary == 3);
    teardown(&o);
}

static void test_budget(void) {
    Observations o;
    NostrSubscription *sub = setup(&o, 257, "EOSE");
    for (int i = 1; i <= 257; i++) enqueue_event(sub, i);
    drain(&o);
    assert(o.events == 256);
    drain(&o);
    assert(o.events == 257);
    enqueue_boundary(sub, "EOSE");
    drain(&o);
    assert(o.boundaries == 1);
    teardown(&o);
}

static void test_full_queue(void) {
    Observations o;
    NostrSubscription *sub = setup(&o, 1024, "EOSE");
    nostr_simple_pool_set_auto_unsub_on_eose(o.pool, false);
    for (int i = 1; i <= 1025; i++) enqueue_event(sub, i);
    assert(atomic_load(&sub->priv->metrics.events_enqueued) == 1024);
    assert(atomic_load(&sub->priv->metrics.events_dropped) == 1);
    enqueue_boundary(sub, "EOSE"); /* fence counts accepted, not attempted EVENTs */
    drain(&o);
    assert(o.events == 256 && o.boundaries == 0);
    enqueue_event(sub, 1025); /* accepted live after room becomes available */
    drain(&o);
    drain(&o);
    drain(&o);
    assert(o.events == 1024 && o.boundaries == 0);
    drain(&o);
    assert(o.events == 1025 && o.boundaries == 1 && o.events_at_boundary == 1024);
    teardown(&o);
}

static void test_worker(const char *state) {
    Observations o;
    NostrSubscription *sub = setup(&o, 2, state);
    enqueue_event(sub, 1);
    enqueue_event(sub, 2);
    enqueue_boundary(sub, state);
    nostr_simple_pool_start(o.pool);
    pthread_mutex_lock(&o.mutex);
    while (!o.boundaries) pthread_cond_wait(&o.cond, &o.mutex);
    pthread_mutex_unlock(&o.mutex);
    nostr_simple_pool_stop(o.pool);
    assert(o.events == 2 && o.boundaries == 1 && o.pool->subs_count == 0);
    teardown(&o);
}

static Observations *batch_observations;
static void batch_received(NostrIncomingEvent *items, size_t count) {
    for (size_t i = 0; i < count; i++) event_received(&items[i], batch_observations);
}

static void test_batch(void) {
    Observations o;
    NostrSubscription *sub = setup(&o, 2, "EOSE");
    batch_observations = &o;
    nostr_simple_pool_set_batch_middleware(o.pool, batch_received);
    enqueue_event(sub, 1);
    enqueue_event(sub, 2);
    enqueue_boundary(sub, "EOSE");
    drain(&o);
    assert(o.events == 2 && o.boundaries == 1);
    teardown(&o);
}

static void test_live_eose(void) {
    Observations o;
    NostrSubscription *sub = setup(&o, 2, "EOSE");
    nostr_simple_pool_set_auto_unsub_on_eose(o.pool, false);
    enqueue_event(sub, 1);
    enqueue_event(sub, 2);
    enqueue_boundary(sub, "EOSE");
    o.live_expected[1] = o.live_expected[2] = 0;
    for (int i = 3; i <= 258; i++) o.live_expected[i] = 1;
    live_sub = sub;
    live_sequence = 2;
    drain(&o); /* native producer appends on every receive, but the drain is bounded */
    assert(o.events == 256 && o.boundaries == 1 && o.events_at_boundary == 2);
    live_sub = NULL;
    drain(&o);
    assert(o.events == 258 && o.boundaries == 1);
    teardown(&o);
}

static void test_stop_selected(void) {
    Observations o;
    NostrSubscription *sub = setup(&o, 300, "EOSE");
    for (int i = 1; i <= 300; i++) enqueue_event(sub, i);
    atomic_store(&o.pool->running, true);
    stop_after_select = o.pool;
    simple_pool_thread_func(o.pool);
    assert(o.events == 300 && o.boundaries == 0);
    teardown(&o);
}

static void test_cancel_backlog(void) {
    Observations o;
    NostrSubscription *sub = setup(&o, 300, "EOSE");
    for (int i = 1; i <= 300; i++) {
        enqueue_event(sub, i);
        o.live_expected[i] = 0;
    }
    enqueue_boundary(sub, "EOSE");
    nostr_subscription_unsubscribe(sub);
    nostr_subscription_wait(sub);
    drain(&o);
    drain(&o);
    assert(o.events == 300 && o.boundaries == 1 && o.pool->subs_count == 0);
    teardown(&o);
}

static void test_retained_notify(void) {
    Observations o;
    NostrSubscription *sub = nostr_subscription_ref(setup(&o, 0, "EOSE"));
    /* The retained sub also retains its relay. Its derived contexts borrow
     * this root, so keep the root alive until the relay workers have exited. */
    GoContext *context = go_context_ref(o.context);
    teardown(&o); /* retained subscription owns the wake channel, never a pool pointer */
    enqueue_event(sub, 1);
    enqueue_boundary(sub, "EOSE");
    enqueue_boundary(sub, "CLOSED");
    nostr_subscription_unref(sub);
    go_context_unref(context);
}

static void test_public_channels(void) {
    Error *err = NULL;
    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://chqxa.invalid", &err);
    assert(relay && !err && nostr_relay_connect(relay, &err) && !err);
    NostrFilters *filters = nostr_filters_new();
    assert(filters);
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    assert(sub && !sub->priv->ordered_notify);
    nostr_subscription_set_filters(sub, filters);
    assert(nostr_subscription_fire(sub, &err) && !err);
    GoChannel *events = nostr_subscription_get_events_channel(sub);
    GoChannel *eose = nostr_subscription_get_eose_channel(sub);
    GoChannel *closed = nostr_subscription_get_closed_channel(sub);
    enqueue_event(sub, 1);
    enqueue_boundary(sub, "EOSE");
    enqueue_event(sub, 2);
    enqueue_boundary(sub, "CLOSED");
    enqueue_event(sub, 3); /* terminal CLOSED cannot accept another EVENT */
    assert(events == sub->events && eose == sub->end_of_stored_events && closed == sub->closed_reason);
    for (int i = 1; i <= 2; i++) {
        void *payload = NULL;
        assert(go_channel_try_receive(events, &payload) == 0);
        NostrEvent *event = payload;
        assert(event && event->created_at == i);
        nostr_event_free(event); /* same public EVENT ownership */
    }
    assert(go_channel_try_receive(events, NULL) != 0);
    assert(go_channel_try_receive(eose, NULL) == 0); /* NULL signal payload */
    void *reason = NULL;
    assert(go_channel_try_receive(closed, &reason) == 0);
    assert(reason && strcmp(reason, "error: fixture closed") == 0);
    free(reason); /* same transferred public CLOSED string ownership */
    nostr_subscription_free(sub);
    nostr_relay_unref(relay);
    go_context_unref(ctx);
}

static void test_query(int selection) {
    Observations o;
    int expected = selection == 0 || selection == 3 ? 1 : 0;
    NostrSubscription *sub = setup(&o, expected, selection == 2 ? "CLOSED" : "EOSE");
    NostrRelay *relay = nostr_relay_ref(nostr_subscription_get_relay(sub));
    nostr_subscription_free(sub);
    free(o.pool->subs);
    o.pool->subs = NULL;
    o.pool->subs_count = 0;
    o.expected_subs = 0;
    o.pool->relays = malloc(sizeof(relay));
    assert(o.pool->relays);
    o.pool->relays[0] = relay;
    o.pool->relay_count = 1;
    const char *urls[] = {"wss://chqxa.invalid"};
    NostrFilter filter = {0};
    setenv("NOSTR_SIMPLE_POOL_ONESHOT", "1", 1);
    query_selection = selection;
    nostr_simple_pool_query_single(o.pool, urls, 1, filter);
    query_selection = -1;
    assert(o.events == expected && o.boundaries == (expected == 0));
    teardown(&o);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    setenv("NOSTR_TEST_MODE", "1", 1);
    setenv("NOSTR_SUB_EVENTS_CAP", "1024", 1);
    if (strcmp(argv[1], "budget") == 0) test_budget();
    else if (strcmp(argv[1], "batch") == 0) test_batch();
    else if (strcmp(argv[1], "queue-full") == 0) test_full_queue();
    else if (strcmp(argv[1], "live-eose") == 0) test_live_eose();
    else if (strcmp(argv[1], "stop-selected") == 0) test_stop_selected();
    else if (strcmp(argv[1], "cancel-backlog") == 0) test_cancel_backlog();
    else if (strcmp(argv[1], "retained-notify") == 0) test_retained_notify();
    else if (strcmp(argv[1], "public-channels") == 0) test_public_channels();
    else if (strcmp(argv[1], "query-event") == 0) test_query(0);
    else if (strcmp(argv[1], "query-eose") == 0) test_query(1);
    else if (strcmp(argv[1], "query-closed") == 0) test_query(2);
    else if (strcmp(argv[1], "query-event-eose") == 0) test_query(3);
    else if (strcmp(argv[1], "eose-race") == 0) test_boundary("EOSE", true, 2);
    else if (strcmp(argv[1], "closed-race") == 0) test_boundary("CLOSED", true, 2);
    else if (strcmp(argv[1], "eose-backlog") == 0) test_boundary("EOSE", false, 300);
    else if (strcmp(argv[1], "closed-backlog") == 0) test_boundary("CLOSED", false, 300);
    else if (strcmp(argv[1], "worker-eose") == 0) test_worker("EOSE");
    else if (strcmp(argv[1], "worker-closed") == 0) test_worker("CLOSED");
    else if (strcmp(argv[1], "wire-eose-queued") == 0) test_wire_order(false, 1);
    else if (strcmp(argv[1], "wire-eose-mid-drain") == 0) test_wire_order(true, 1);
    else if (strcmp(argv[1], "wire-eose-backlog-queued") == 0) test_wire_order(false, 300);
    else if (strcmp(argv[1], "wire-eose-backlog-mid-drain") == 0) test_wire_order(true, 300);
    else if (strcmp(argv[1], "wire-closed-mid-drain") == 0) test_closed_mid_drain();
    else if (strcmp(argv[1], "mixed-fences") == 0) test_mixed_fences(false);
    else if (strcmp(argv[1], "closed-refire") == 0) test_mixed_fences(true);
    else if (strcmp(argv[1], "refire-queued") == 0) test_refire(false);
    else if (strcmp(argv[1], "refire-inflight") == 0) test_refire(true);
    else abort();
    puts("SimplePool native producer-order regression passed");
    return 0;
}
