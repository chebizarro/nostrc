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
 * slots.  The API-level cases run in both modes.
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
    atomic_store_explicit(&c->slot_seq[idx], t + c->capacity, memory_order_release);
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
    /* send_with_context used the masked single-lock increment in MPMC mode:
     * filling the ring to capacity made it read as empty. */
    for (unsigned i = 0; i < 4; i++)
        CHECK(go_channel_send_with_context(c, V(1 + i), NULL) == 0, "send_with_context");
    CHECK(go_channel_get_depth(c) == 4, "full ring must report depth 4");
    CHECK(go_channel_try_send(c, V(5)) != 0, "full ring must refuse try_send");
    for (unsigned i = 0; i < 4; i++) {
        void *v = NULL;
        CHECK(go_channel_try_receive(c, &v) == 0 && v == V(1 + i), "FIFO after send_with_context");
    }
    CHECK(go_channel_get_depth(c) == 0, "empty after draining");

    unsigned next_send = 100, next_recv = 100;
    for (unsigned round = 0; round < 40; round++) {
        unsigned burst = 1 + round % 4;
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
    printf("go_channel_mpmc_slot_test: OK\n");
    return 0;
}
