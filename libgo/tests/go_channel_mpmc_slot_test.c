/* Regression test for nostrc-75rv: every MPMC channel path must honour the
 * per-slot sequence protocol.
 *
 * In MPMC mode a sender claims ticket t by advancing `in` and only then
 * stores the value and publishes slot_seq; a receiver claims by advancing
 * `out` and only then takes the value and releases the slot.  A thread
 * preempted between claim and publish/release leaves the slot BUSY.  The try
 * paths used to fall back, after 64 spins on a BUSY slot, to a mutex path
 * that ignored slot_seq and masked the monotonic tickets: try_receive
 * returned success with a NULL value and skipped the element, try_send wrote
 * over an element a receiver was still taking, and go_channel_receive /
 * go_channel_send_with_context had the same blind spot.  Under concurrent
 * load this lost GNostrSubscription EVENTs and relay frames
 * (test_nostr_gobject_subscription_eose_order timing out).
 *
 * The white-box cases below put a slot into exactly the state a preempted
 * peer leaves it in (claimed, not yet published/released), so they are
 * deterministic.  They are skipped when the library is built without MPMC
 * slots.  The API-level cases run in both modes, sized for the single-lock
 * ring's one-empty-slot discipline when slots are off.
 *
 * Capacity 1 (review F1): the slot protocol needs a physical ring of at least
 * two slots.  With one, "ticket t published" and "slot free for ticket t+1"
 * are the same sequence value, so a sender claimed the slot a receiver had
 * claimed but not yet read: the element was lost, slot_seq went backwards and
 * the channel wedged (try paths failed forever, a blocking receive spun at
 * 100% CPU and ignored close).  Cases 6 and 7 cover it.  Case 6 is
 * white-box (MPMC only); case 7 runs wherever a capacity-1 channel can hold
 * an element, which the index-derived single-lock ring cannot (nostrc-ecz3).
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "go.h"

/* assert() is compiled out with NDEBUG; never let the test pass vacuously. */
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s (%s) at %s:%d\n", msg, #cond, \
                    __FILE__, __LINE__);                                     \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define V(n) ((void *)(uintptr_t)(n))

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int mpmc_enabled(GoChannel *c) { return c->slot_seq != NULL; }
/* Physical slots.  With MPMC slots this can exceed the logical capacity. */
static size_t ring_slots(GoChannel *c) { return c->mask + 1; }

/* How many elements a fresh channel of @capacity actually holds.  MPMC slots
 * and the size-counted single-lock ring hold @capacity; the index-derived
 * single-lock ring (DERIVE_SIZE, what TSAN builds use) keeps one slot empty
 * and holds @capacity - 1 -- none at all for capacity 1 (nostrc-ecz3). */
static unsigned channel_holds(size_t capacity) {
    GoChannel *c = go_channel_create(capacity);
    unsigned n = 0;
    while (n <= capacity && go_channel_try_send(c, V(1 + n)) == 0)
        n++;
    void *v;
    while (go_channel_try_receive(c, &v) == 0) {}
    go_channel_unref(c);
    return n;
}

/* Move the tickets past the capacity so masking bugs cannot hide. */
static void advance_laps(GoChannel *c, unsigned laps) {
    for (unsigned i = 0; i < laps * c->capacity; i++) {
        void *v = NULL;
        CHECK(go_channel_try_send(c, V(1000 + i)) == 0, "warm-up send");
        CHECK(go_channel_try_receive(c, &v) == 0 && v == V(1000 + i), "warm-up receive");
    }
}

/* A sender preempted after claiming ticket t: `in` counts it, slot not published. */
static size_t claim_send_ticket(GoChannel *c) {
    return atomic_fetch_add_explicit(&c->in, 1, memory_order_acq_rel);
}
static void publish_send_ticket(GoChannel *c, size_t t, void *value) {
    size_t idx = t & c->mask;
    atomic_store_explicit(&c->buffer[idx], value, memory_order_release);
    atomic_store_explicit(&c->slot_seq[idx], t + 1, memory_order_release);
}

/* --- 1. try_receive must not consume a claimed but unpublished slot ------- */
static void test_try_receive_skips_nothing(void) {
    GoChannel *c = go_channel_create(4);
    if (!mpmc_enabled(c)) { printf("1: skipped (MPMC slots disabled)\n"); go_channel_unref(c); return; }
    advance_laps(c, 3);

    size_t t = claim_send_ticket(c);
    size_t out_before = atomic_load(&c->out);
    void *v = V(0xbad);
    CHECK(go_channel_try_receive(c, &v) != 0, "try_receive consumed an unpublished slot");
    CHECK(v == V(0xbad), "try_receive wrote an output for an unpublished slot");
    CHECK(atomic_load(&c->out) == out_before, "try_receive advanced out past an unpublished slot");
    CHECK(go_channel_get_depth(c) == 1, "depth must still count the in-flight element");

    /* go_select (the GNostrSubscription monitor's path) sees nothing yet. */
    void *sel_v = V(0xbad);
    GoSelectCase cases[1] = { { GO_SELECT_RECEIVE, c, NULL, &sel_v } };
    GoSelectResult r = go_select_timeout(cases, 1, 30);
    CHECK(r.selected_case == -1, "select returned an unpublished slot");

    publish_send_ticket(c, t, V(42));
    CHECK(go_channel_try_receive(c, &v) == 0 && v == V(42), "published element not received");
    CHECK(go_channel_try_send(c, V(43)) == 0, "send after publish");
    CHECK(go_channel_try_receive(c, &v) == 0 && v == V(43), "FIFO after publish");
    CHECK(go_channel_get_depth(c) == 0, "ring must be empty");
    CHECK(atomic_load(&c->in) == atomic_load(&c->out), "tickets must agree");
    CHECK(atomic_load(&c->in) > c->capacity, "tickets must stay monotonic (not masked)");
    go_channel_unref(c);
    printf("1: ok\n");
}

/* --- 2. try_send must not overwrite a slot a receiver is still taking ------ */
static void test_try_send_waits_for_release(void) {
    GoChannel *c = go_channel_create(4);
    if (!mpmc_enabled(c)) { printf("2: skipped (MPMC slots disabled)\n"); go_channel_unref(c); return; }
    advance_laps(c, 2);
    for (unsigned i = 0; i < 4; i++)
        CHECK(go_channel_try_send(c, V(10 + i)) == 0, "fill");

    /* A receiver preempted after claiming the head: out advanced, slot not released. */
    size_t t = atomic_fetch_add_explicit(&c->out, 1, memory_order_acq_rel);
    size_t idx = t & c->mask;
    CHECK(go_channel_try_send(c, V(99)) != 0, "try_send overwrote a slot still being received");
    CHECK(atomic_load(&c->buffer[idx]) == V(10), "in-flight element was clobbered");

    /* The receiver finishes. */
    void *taken = atomic_load(&c->buffer[idx]);
    atomic_store(&c->buffer[idx], NULL);
    atomic_store_explicit(&c->slot_seq[idx], t + ring_slots(c), memory_order_release);
    CHECK(taken == V(10), "receiver's element");

    CHECK(go_channel_try_send(c, V(14)) == 0, "send after release");
    for (unsigned i = 11; i <= 14; i++) {
        void *v = NULL;
        CHECK(go_channel_try_receive(c, &v) == 0 && v == V(i), "FIFO after release");
    }
    CHECK(go_channel_get_depth(c) == 0, "ring must be empty");
    go_channel_unref(c);
    printf("2: ok\n");
}

/* --- 3. blocking receives wait for an in-flight sender --------------------- */
typedef struct {
    GoChannel *c;
    int with_context;
    int rc;
    void *value;
    atomic_int done;
} RecvArg;

static void *blocking_receiver(void *p) {
    RecvArg *a = p;
    a->value = V(0xbad);
    a->rc = a->with_context ? go_channel_receive_with_context(a->c, &a->value, NULL)
                            : go_channel_receive(a->c, &a->value);
    atomic_store(&a->done, 1);
    return NULL;
}

static void test_blocking_receive_waits_for_publish(int with_context) {
    GoChannel *c = go_channel_create(4);
    if (!mpmc_enabled(c)) { printf("3: skipped (MPMC slots disabled)\n"); go_channel_unref(c); return; }
    advance_laps(c, 2);

    size_t t = claim_send_ticket(c);
    RecvArg a = { .c = c, .with_context = with_context };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, blocking_receiver, &a) == 0, "thread");
    sleep_ms(100);
    CHECK(!atomic_load(&a.done), "blocking receive returned before the element was published");

    publish_send_ticket(c, t, V(77));
    pthread_join(th, NULL);
    CHECK(a.rc == 0 && a.value == V(77), "blocking receive must return the published element");
    CHECK(go_channel_get_depth(c) == 0, "ring must be empty");
    go_channel_unref(c);
    printf("3%s: ok\n", with_context ? "b (with context)" : "a");
}

/* --- 4. every send/receive variant keeps FIFO and tickets across laps ------ */
static void test_mixed_variants_fifo(void) {
    GoChannel *c = go_channel_create(4);
    /* Sending more than the ring holds with a NULL context would block
     * forever, so measure it (see channel_holds). */
    const unsigned holds = channel_holds(4);
    if (mpmc_enabled(c))
        CHECK(holds == 4, "an MPMC channel must hold its full capacity");
    CHECK(holds == 4 || holds == 3, "unexpected ring size");
    /* send_with_context used the masked single-lock increment in MPMC mode:
     * filling the ring to capacity made it read as empty. */
    for (unsigned i = 0; i < holds; i++)
        CHECK(go_channel_send_with_context(c, V(1 + i), NULL) == 0, "send_with_context");
    CHECK(go_channel_get_depth(c) == holds, "a full ring must report its depth");
    CHECK(go_channel_try_send(c, V(5)) != 0, "full ring must refuse try_send");
    for (unsigned i = 0; i < holds; i++) {
        void *v = NULL;
        CHECK(go_channel_try_receive(c, &v) == 0 && v == V(1 + i), "FIFO after send_with_context");
    }
    CHECK(go_channel_get_depth(c) == 0, "empty after draining");

    unsigned next_send = 100, next_recv = 100;
    for (unsigned round = 0; round < 40; round++) {
        unsigned burst = 1 + round % holds;
        for (unsigned i = 0; i < burst; i++) {
            void *v = V(next_send++);
            switch ((round + i) % 3) {
            case 0: CHECK(go_channel_try_send(c, v) == 0, "try_send"); break;
            case 1: CHECK(go_channel_send(c, v) == 0, "send"); break;
            default: CHECK(go_channel_send_with_context(c, v, NULL) == 0, "send_with_context"); break;
            }
        }
        CHECK(go_channel_get_depth(c) == burst, "depth after burst");
        for (unsigned i = 0; i < burst; i++) {
            void *v = NULL;
            int rc;
            switch ((round + i) % 3) {
            case 0: rc = go_channel_try_receive(c, &v); break;
            case 1: rc = go_channel_receive(c, &v); break;
            default: rc = go_channel_receive_with_context(c, &v, NULL); break;
            }
            CHECK(rc == 0 && v == V(next_recv), "mixed variants must stay FIFO");
            next_recv++;
        }
    }
    CHECK(go_channel_get_depth(c) == 0, "empty at end");
    go_channel_unref(c);
    printf("4: ok\n");
}

/* --- 6. capacity 1: a send must not overwrite a claimed, unread element ---- */
typedef struct {
    GoChannel *c;
    int rc;
    void *value;
    atomic_int done;
} Cap1Recv;

static void *cap1_blocking_receiver(void *p) {
    Cap1Recv *a = p;
    a->rc = go_channel_receive(a->c, &a->value);
    atomic_store(&a->done, 1);
    return NULL;
}

static void test_capacity1_claimed_slot(int finish_with_blocking_receive) {
    const char *name = finish_with_blocking_receive ? "6b" : "6a";
    GoChannel *c = go_channel_create(1);
    if (!mpmc_enabled(c)) { printf("%s: skipped (MPMC slots disabled)\n", name); go_channel_unref(c); return; }
    CHECK(c->capacity == 1, "logical capacity stays 1");
    advance_laps(c, 3);

    CHECK(go_channel_try_send(c, V(10)) == 0, "send A");
    CHECK(go_channel_try_send(c, V(11)) != 0, "capacity 1 holds one element");
    /* A receiver preempted after claiming A: out advanced, A not yet read. */
    size_t t = atomic_fetch_add_explicit(&c->out, 1, memory_order_acq_rel);
    size_t idx = t & c->mask;

    /* Logically the channel is empty again, so a send may proceed -- but it
     * must land in a different slot, never on the element being received. */
    CHECK(go_channel_try_send(c, V(99)) == 0, "send B while A is being received");
    int clobbered = atomic_load(&c->buffer[idx]) != V(10);
    /* 6a asserts the overwrite itself; 6b goes on to the blocking receive,
     * which on the one-slot ring then spun forever. */
    if (!finish_with_blocking_receive)
        CHECK(!clobbered, "B overwrote the element being received");
    CHECK(go_channel_try_send(c, V(12)) != 0, "capacity 1 still holds one element");

    /* The receiver finishes taking A. */
    void *taken = atomic_load(&c->buffer[idx]);
    atomic_store(&c->buffer[idx], NULL);
    atomic_store_explicit(&c->slot_seq[idx], t + ring_slots(c), memory_order_release);

    void *v = NULL;
    if (finish_with_blocking_receive) {
        /* On the one-slot ring this receive spun forever at 100% CPU. */
        Cap1Recv a = { .c = c, .value = V(0xbad) };
        pthread_t th;
        CHECK(pthread_create(&th, NULL, cap1_blocking_receiver, &a) == 0, "thread");
        for (int waited = 0; !atomic_load(&a.done) && waited < 2000; waited += 5)
            sleep_ms(5);
        CHECK(atomic_load(&a.done), "blocking receive wedged on a capacity-1 channel");
        pthread_join(th, NULL);
        CHECK(a.rc == 0, "blocking receive");
        v = a.value;
    } else {
        CHECK(go_channel_try_receive(c, &v) == 0, "try_receive after the in-flight receive");
    }
    CHECK(!clobbered, "B overwrote the element being received");
    CHECK(taken == V(10), "the receiver must get A");
    CHECK(v == V(99), "B must be delivered intact");
    CHECK(go_channel_get_depth(c) == 0, "ring must be empty");

    /* Not wedged: the channel keeps working in both directions. */
    for (unsigned i = 0; i < 8; i++) {
        CHECK(go_channel_try_send(c, V(200 + i)) == 0, "send after recovery");
        CHECK(go_channel_try_receive(c, &v) == 0 && v == V(200 + i), "receive after recovery");
    }
    CHECK(ring_slots(c) >= 2, "the slot protocol needs at least two physical slots");
    go_channel_unref(c);
    printf("%s: ok\n", name);
}

/* --- 7. capacity 1 under threads: the ticker / wake-channel shapes ------- */
#define CAP1_RUN_MS 1500

typedef struct {
    GoChannel *c;
    atomic_int stop;
    atomic_ulong sent;
    atomic_ulong received;
    atomic_ulong nulls;
    atomic_int consumer_done;
} Cap1Stress;

static void *cap1_try_producer(void *p) {
    Cap1Stress *s = p;
    unsigned long n = 0;
    while (!atomic_load(&s->stop)) {
        if (go_channel_try_send(s->c, V(++n)) == 0)
            atomic_fetch_add(&s->sent, 1);
        else
            n--; /* coalesced: a capacity-1 wake channel already holds a token */
    }
    return NULL;
}

static void *cap1_blocking_consumer(void *p) {
    Cap1Stress *s = p;
    void *v = NULL;
    while (go_channel_receive(s->c, &v) == 0) { /* returns -1 once closed and drained */
        if (!v) atomic_fetch_add(&s->nulls, 1);
        atomic_fetch_add(&s->received, 1);
    }
    atomic_store(&s->consumer_done, 1);
    return NULL;
}

static void *cap1_try_consumer(void *p) {
    Cap1Stress *s = p;
    void *v = NULL;
    while (!atomic_load(&s->stop)) {
        if (go_channel_try_receive(s->c, &v) == 0) {
            if (!v) atomic_fetch_add(&s->nulls, 1);
            atomic_fetch_add(&s->received, 1);
        }
    }
    return NULL;
}

/* 1 try_send producer x 1 blocking go_channel_receive consumer, then close:
 * every sent token arrives and the consumer returns promptly on close. */
static void test_capacity1_try_send_blocking_receive(void) {
    if (channel_holds(1) != 1) { printf("7a: skipped (capacity-1 ring holds nothing; nostrc-ecz3)\n"); return; }

    Cap1Stress s = { .c = go_channel_create(1) };
    pthread_t prod, cons;
    CHECK(pthread_create(&cons, NULL, cap1_blocking_consumer, &s) == 0, "thread");
    CHECK(pthread_create(&prod, NULL, cap1_try_producer, &s) == 0, "thread");
    sleep_ms(CAP1_RUN_MS);
    atomic_store(&s.stop, 1);
    pthread_join(prod, NULL);
    go_channel_close(s.c);
    for (int waited = 0; !atomic_load(&s.consumer_done) && waited < 3000; waited += 5)
        sleep_ms(5);
    printf("7a: sent %lu, received %lu, NULL %lu\n", atomic_load(&s.sent),
           atomic_load(&s.received), atomic_load(&s.nulls));
    CHECK(atomic_load(&s.consumer_done), "blocking receiver did not return after close (wedged)");
    pthread_join(cons, NULL);
    CHECK(atomic_load(&s.nulls) == 0, "a receive returned a NULL token");
    CHECK(atomic_load(&s.received) == atomic_load(&s.sent), "tokens were lost");
    CHECK(atomic_load(&s.sent) > 1000, "too few transfers to exercise the race");
    go_channel_unref(s.c);
    printf("7a: ok\n");
}

/* 2 try_send producers x 2 try_receive consumers, then drain: nothing lost,
 * and the channel still works afterwards (it used to wedge in <2000 ops). */
static void test_capacity1_try_try(void) {
    if (channel_holds(1) != 1) { printf("7b: skipped (capacity-1 ring holds nothing; nostrc-ecz3)\n"); return; }

    Cap1Stress s = { .c = go_channel_create(1) };
    pthread_t prod[2], cons[2];
    for (int i = 0; i < 2; i++) {
        CHECK(pthread_create(&cons[i], NULL, cap1_try_consumer, &s) == 0, "thread");
        CHECK(pthread_create(&prod[i], NULL, cap1_try_producer, &s) == 0, "thread");
    }
    sleep_ms(CAP1_RUN_MS);
    atomic_store(&s.stop, 1);
    for (int i = 0; i < 2; i++) {
        pthread_join(prod[i], NULL);
        pthread_join(cons[i], NULL);
    }
    void *v = NULL;
    while (go_channel_try_receive(s.c, &v) == 0)
        atomic_fetch_add(&s.received, 1);
    printf("7b: sent %lu, received %lu, NULL %lu, depth %zu\n", atomic_load(&s.sent),
           atomic_load(&s.received), atomic_load(&s.nulls), go_channel_get_depth(s.c));
    CHECK(atomic_load(&s.nulls) == 0, "a receive returned a NULL token");
    CHECK(atomic_load(&s.received) == atomic_load(&s.sent), "tokens were lost");
    CHECK(go_channel_get_depth(s.c) == 0, "drained ring must be empty");
    CHECK(go_channel_try_send(s.c, V(1)) == 0, "channel wedged: try_send fails on an empty ring");
    CHECK(go_channel_try_receive(s.c, &v) == 0 && v == V(1), "channel wedged: try_receive");
    go_channel_unref(s.c);
    printf("7b: ok\n");
}

/* --- 8. select must not report "closed" over a claimed, unpublished element */
static void test_select_close_with_inflight_send(void) {
    GoChannel *c = go_channel_create(4);
    if (!mpmc_enabled(c)) { printf("8: skipped (MPMC slots disabled)\n"); go_channel_unref(c); return; }
    advance_laps(c, 2);
    /* A sender claimed a slot before the close and is still publishing. */
    size_t t = claim_send_ticket(c);
    go_channel_close(c);

    void *v = V(0xbad);
    GoSelectCase cases[1] = { { GO_SELECT_RECEIVE, c, NULL, &v } };
    GoSelectResult r = go_select_timeout(cases, 1, 30);
    CHECK(r.selected_case == -1, "select reported closed while an element was being published");

    publish_send_ticket(c, t, V(55));
    r = go_select_timeout(cases, 1, 1000);
    CHECK(r.selected_case == 0 && r.ok && v == V(55), "select must deliver the in-flight element");
    r = go_select_timeout(cases, 1, 1000);
    CHECK(r.selected_case == 0 && !r.ok, "then report closed");
    go_channel_unref(c);
    printf("8: ok\n");
}

/* --- 9. capacity 0 means 1 (no unbuffered channels): no out-of-bounds ring - */
static void test_capacity0(void) {
    GoChannel *c = go_channel_create(0);
    CHECK(c && c->capacity == 1, "capacity 0 must become capacity 1");
    void *v = NULL;
    /* Used to index a zero-byte slot_seq here (ASAN heap-buffer-overflow). */
    CHECK(go_channel_try_receive(c, &v) != 0, "empty channel");
    if (channel_holds(1) == 1) {
        CHECK(go_channel_try_send(c, V(7)) == 0, "one element fits");
        CHECK(go_channel_try_send(c, V(8)) != 0, "only one element fits");
        CHECK(go_channel_try_receive(c, &v) == 0 && v == V(7), "receive it back");
    }
    go_channel_unref(c);
    printf("9: ok\n");
}

/* --- 5. bounded mixed-mode stress: nothing lost, duplicated or NULL -------- */
#define STRESS_PRODUCERS 4
#define STRESS_CONSUMERS 4
#define STRESS_PER_PRODUCER 50000

static GoChannel *g_stress;
static GoContext *g_stress_ctx;      /* cancelled at the deadline */
static atomic_int g_stress_give_up;  /* ditto, for the try_send loop */
static atomic_uint g_received;
static atomic_uint g_null_values;
static atomic_uchar g_seen[STRESS_PRODUCERS * STRESS_PER_PRODUCER + 1];

static void *stress_producer(void *p) {
    unsigned id = (unsigned)(uintptr_t)p;
    for (unsigned i = 0; i < STRESS_PER_PRODUCER; i++) {
        unsigned n = 1 + id * STRESS_PER_PRODUCER + i;
        /* A corrupted ring can refuse sends forever; give up at the
         * deadline so the test fails instead of hanging. */
        if ((i + id) % 2) {
            while (go_channel_try_send(g_stress, V(n)) != 0) {
                if (atomic_load(&g_stress_give_up))
                    return NULL;
                sched_yield();
            }
        } else if (go_channel_send_with_context(g_stress, V(n), g_stress_ctx) != 0) {
            return NULL;
        }
    }
    return NULL;
}

static void record(void *v) {
    if (!v) { atomic_fetch_add(&g_null_values, 1); return; }
    uintptr_t n = (uintptr_t)v;
    CHECK(n >= 1 && n <= STRESS_PRODUCERS * STRESS_PER_PRODUCER, "value out of range");
    CHECK(atomic_fetch_add(&g_seen[n], 1) == 0, "element received twice");
    atomic_fetch_add(&g_received, 1);
}

static void *stress_consumer(void *p) {
    unsigned id = (unsigned)(uintptr_t)p;
    for (;;) {
        void *v = NULL;
        if (id % 2) {
            if (go_channel_receive(g_stress, &v) != 0)
                return NULL; /* closed and empty */
            record(v);
        } else if (go_channel_try_receive(g_stress, &v) == 0) {
            record(v);
        } else if (go_channel_is_closed(g_stress) && go_channel_get_depth(g_stress) == 0) {
            return NULL;
        } else {
            sched_yield();
        }
    }
}

static void test_mixed_stress(void) {
    g_stress = go_channel_create(8); /* small ring: constant wrap-around */
    CancelContextResult cc = go_context_with_cancel(go_context_background());
    g_stress_ctx = cc.context;
    pthread_t prod[STRESS_PRODUCERS], cons[STRESS_CONSUMERS];
    for (unsigned i = 0; i < STRESS_CONSUMERS; i++)
        CHECK(pthread_create(&cons[i], NULL, stress_consumer, V(i)) == 0, "thread");
    for (unsigned i = 0; i < STRESS_PRODUCERS; i++)
        CHECK(pthread_create(&prod[i], NULL, stress_producer, V(i)) == 0, "thread");
    const unsigned total = STRESS_PRODUCERS * STRESS_PER_PRODUCER;
    for (int waited = 0; atomic_load(&g_received) < total && waited < 20000; waited += 10)
        sleep_ms(10);
    /* Done, or the deadline passed: release blocked senders and receivers
     * either way (close wakes waiters parked on the channel). */
    atomic_store(&g_stress_give_up, 1);
    cc.cancel(g_stress_ctx);
    go_channel_close(g_stress);
    for (unsigned i = 0; i < STRESS_PRODUCERS; i++)
        pthread_join(prod[i], NULL);
    for (unsigned i = 0; i < STRESS_CONSUMERS; i++)
        pthread_join(cons[i], NULL);

    printf("5: received %u/%u, NULL values %u\n", atomic_load(&g_received), total,
           atomic_load(&g_null_values));
    CHECK(atomic_load(&g_null_values) == 0, "a receive returned success with a NULL value");
    CHECK(atomic_load(&g_received) == total, "elements were lost");
    go_channel_unref(g_stress);
    go_context_unref(g_stress_ctx);
    printf("5: ok\n");
}

int main(void) {
    test_try_receive_skips_nothing();
    test_try_send_waits_for_release();
    test_blocking_receive_waits_for_publish(0);
    test_blocking_receive_waits_for_publish(1);
    test_mixed_variants_fifo();
    test_mixed_stress();
    test_capacity1_claimed_slot(0);
    test_capacity1_claimed_slot(1);
    test_capacity1_try_send_blocking_receive();
    test_capacity1_try_try();
    test_select_close_with_inflight_send();
    test_capacity0();
    printf("go_channel_mpmc_slot_test: OK\n");
    return 0;
}
