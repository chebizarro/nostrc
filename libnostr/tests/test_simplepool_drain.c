/* Deterministic coverage of nostrc-chqxa. Compile the real implementation in
 * this TU to control the exact empty-EVENT/queued-boundary interleaving without
 * adding a shipped hook. Public worker entrypoints are exercised separately. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "channel.h"
#include "select.h"
#include "nostr-simple-pool.h"
#include "nostr-subscription.h"
#include "subscription-private.h"

static int test_try_receive(GoChannel *ch, void **msg);
static int test_select(GoSelectCase *cases, size_t count);
static GoSelectResult test_select_timeout(GoSelectCase *cases, size_t count, uint64_t timeout);
#define go_select test_select
#define go_select_timeout test_select_timeout
#define go_channel_try_receive test_try_receive
#include "../src/simplepool.c"
#undef go_channel_try_receive
#undef go_select
#undef go_select_timeout

static _Thread_local GoChannel *inject_after_empty;
static _Thread_local NostrSubscription *inject_sub;
static _Thread_local const char *inject_boundary;
static _Thread_local int query_selection = -1;
static _Thread_local NostrSimplePool *stop_after_select;
static _Thread_local NostrSubscription *live_sub;
static _Thread_local int live_sequence;

static void enqueue_event(NostrSubscription *sub, int sequence) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    nostr_event_set_created_at(ev, sequence);
    nostr_event_set_kind(ev, 1);
    nostr_event_set_pubkey(ev, "0000000000000000000000000000000000000000000000000000000000000001");
    char content[32];
    snprintf(content, sizeof(content), "event-%d", sequence);
    nostr_event_set_content(ev, content);
    assert(go_channel_try_send(nostr_subscription_get_events_channel(sub), ev) == 0);
}

static void enqueue_boundary(NostrSubscription *sub, const char *state) {
    if (strcmp(state, "EOSE") == 0)
        nostr_subscription_dispatch_eose(sub);
    else
        nostr_subscription_dispatch_closed(sub, "error: fixture closed");
}

static int test_try_receive(GoChannel *ch, void **msg) {
    int rc = go_channel_try_receive(ch, msg);
    if (live_sub && ch == nostr_subscription_get_events_channel(live_sub) && rc == 0) {
        assert(live_sequence < 4096); /* fail finitely if the drain chases live traffic */
        enqueue_event(live_sub, ++live_sequence);
    }
    if (ch == inject_after_empty && rc != 0) {
        inject_after_empty = NULL;
        /* The reader accepts EVENT, EVENT, boundary immediately after the
         * consumer observes an empty EVENT channel. No scheduler luck. */
        enqueue_event(inject_sub, 1);
        enqueue_event(inject_sub, 2);
        enqueue_boundary(inject_sub, inject_boundary);
    }
    return rc;
}

/* Select is allowed to choose any ready case. Force each legal outcome with
 * real channel receives to cover boundary selection while EVENT is buffered. */
static int test_select(GoSelectCase *cases, size_t count) {
    if (query_selection < 0) return go_select(cases, count);
    assert(count == 3);
    for (int i = 1; i <= (query_selection == 0 ? 1 : 2); i++) {
        NostrEvent *ev = nostr_event_new();
        assert(ev);
        nostr_event_set_pubkey(ev, "0000000000000000000000000000000000000000000000000000000000000001");
        nostr_event_set_created_at(ev, i);
        nostr_event_set_kind(ev, 1);
        assert(go_channel_try_send(cases[0].chan, ev) == 0);
    }
    if (query_selection == 1) assert(go_channel_try_send(cases[1].chan, NULL) == 0);
    if (query_selection == 2)
        assert(go_channel_try_send(cases[2].chan, strdup("error: fixture closed")) == 0);
    assert(go_channel_try_receive(cases[query_selection].chan,
                                  cases[query_selection].recv_buf) == 0);
    return query_selection;
}

static GoSelectResult test_select_timeout(GoSelectCase *cases, size_t count, uint64_t timeout) {
    if (!stop_after_select) return go_select_timeout(cases, count, timeout);
    assert(count == 2 && cases[1].recv_buf);
    assert(go_channel_try_receive(cases[1].chan, cases[1].recv_buf) == 0);
    atomic_store(&stop_after_select->running, false);
    stop_after_select = NULL;
    return (GoSelectResult){ .selected_case = 1 };
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    NostrSimplePool *pool;
    GoContext *context;
    int events;
    int boundaries;
    int expected;
    size_t expected_subs;
    int events_at_boundary;
    bool check_boundary_order;
    const char *state;
} Observations;

static void event_received(NostrIncomingEvent *incoming, void *data) {
    Observations *o = data;
    pthread_mutex_lock(&o->mutex);
    assert(nostr_event_get_created_at(incoming->event) == o->events + 1);
    o->events++;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->mutex);
    nostr_event_free(incoming->event);
}

static void boundary_received(NostrRelay *relay, const char *state,
                              const char *reason, void *data) {
    (void)relay;
    Observations *o = data;
    pthread_mutex_lock(&o->mutex);
    assert(strcmp(state, o->state) == 0);
    o->events_at_boundary = o->events;
    if (o->check_boundary_order)
        assert(o->events == o->expected); /* middleware MUST precede boundary */
    assert(o->pool->subs_count == o->expected_subs); /* notification precedes auto-unsub/free */
    if (strcmp(state, "CLOSED") == 0)
        assert(reason && strcmp(reason, "error: fixture closed") == 0);
    o->boundaries++;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->mutex);
}

static NostrSubscription *setup(Observations *o, int expected, const char *state) {
    memset(o, 0, sizeof(*o));
    pthread_mutex_init(&o->mutex, NULL);
    pthread_cond_init(&o->cond, NULL);
    o->expected = expected;
    o->expected_subs = 1;
    o->check_boundary_order = true;
    o->state = state;
    o->pool = nostr_simple_pool_new();
    assert(o->pool);
    nostr_simple_pool_set_event_middleware_ex(o->pool, event_received, o);
    nostr_simple_pool_set_auto_unsub_on_eose(o->pool, true);
    nostr_simple_pool_set_subscription_callback(o->pool, boundary_received, o);
    Error *err = NULL;
    o->context = go_context_background();
    NostrRelay *relay = nostr_relay_new(o->context, "wss://chqxa.invalid", &err);
    assert(relay && !err);
    NostrFilters *filters = nostr_filters_new();
    assert(filters);
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, NULL, filters);
    assert(sub);
    nostr_subscription_set_filters(sub, filters);
    nostr_relay_unref(relay);
    /* No connection or fire is needed for channel injection. */
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

/* Match the worker's final delivery on the unfixed implementation too, so the
 * same test source builds and runs before and after the production fix. */
static void drain(Observations *o) {
    size_t count = o->pool->subs_count;
    NostrSubscription *snapshot[1] = {count ? o->pool->subs[0] : NULL};
    NostrIncomingEvent *batch = NULL;
    size_t len = 0, cap = 0;
    pool_drain_all(o->pool, snapshot, count, &batch, &len, &cap);
    for (size_t i = 0; i < len; i++) event_received(&batch[i], o);
    free(batch);
}

static void test_boundary(const char *state, bool race, int count) {
    Observations o;
    NostrSubscription *sub = setup(&o, count, state);
    if (race) {
        o.check_boundary_order = false; /* also expose actual loss before the fix */
        inject_after_empty = nostr_subscription_get_events_channel(sub);
        inject_sub = sub;
        inject_boundary = state;
    } else {
        for (int i = 1; i <= count; i++) enqueue_event(sub, i);
        /* Deduplication remains by event ID, without disturbing order. */
        enqueue_event(sub, count);
        enqueue_boundary(sub, state);
    }
    drain(&o);
    drain(&o); /* a boundary arriving after sampling belongs to the next pass */
    assert(o.events == count && o.boundaries == 1);
    assert(o.events_at_boundary == count);
    assert(o.pool->subs_count == 0);
    teardown(&o);
}

static void test_budget(void) {
    Observations o;
    NostrSubscription *sub = setup(&o, 257, "EOSE");
    for (int i = 1; i <= 257; i++) enqueue_event(sub, i);
    drain(&o);
    assert(o.events == 256); /* fairness: do not greedily drain a live sub */
    drain(&o);
    assert(o.events == 257); /* the budget+1 receive must not be discarded */
    enqueue_boundary(sub, "EOSE");
    drain(&o);
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
    live_sub = sub;
    live_sequence = 2;
    /* Each receive makes another EVENT ready: a greedy-until-empty drain
     * would never reach the observer. The sampled FIFO prefix must finish. */
    drain(&o);
    assert(o.events == 2 && o.boundaries == 1 && o.pool->subs_count == 1);
    live_sub = NULL;
    drain(&o);
    assert(o.events == 4 && o.boundaries == 1);
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

static void test_query(int selection) {
    Observations o;
    NostrSubscription *sub = setup(&o, selection == 0 ? 1 : 2,
                                  selection == 2 ? "CLOSED" : "EOSE");
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
    assert(o.events == o.expected && o.boundaries == (selection != 0));
    teardown(&o);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    setenv("NOSTR_TEST_MODE", "1", 1);
    setenv("NOSTR_SUB_EVENTS_CAP", "1024", 1);
    if (strcmp(argv[1], "budget") == 0) test_budget();
    else if (strcmp(argv[1], "batch") == 0) test_batch();
    else if (strcmp(argv[1], "live-eose") == 0) test_live_eose();
    else if (strcmp(argv[1], "stop-selected") == 0) test_stop_selected();
    else if (strcmp(argv[1], "query-event") == 0) test_query(0);
    else if (strcmp(argv[1], "query-eose") == 0) test_query(1);
    else if (strcmp(argv[1], "query-closed") == 0) test_query(2);
    else if (strcmp(argv[1], "eose-race") == 0) test_boundary("EOSE", true, 2);
    else if (strcmp(argv[1], "closed-race") == 0) test_boundary("CLOSED", true, 2);
    else if (strcmp(argv[1], "eose-backlog") == 0) test_boundary("EOSE", false, 300);
    else if (strcmp(argv[1], "closed-backlog") == 0) test_boundary("CLOSED", false, 300);
    else if (strcmp(argv[1], "worker-eose") == 0) test_worker("EOSE");
    else if (strcmp(argv[1], "worker-closed") == 0) test_worker("CLOSED");
    else abort();
    puts("SimplePool drain regression passed");
    return 0;
}
