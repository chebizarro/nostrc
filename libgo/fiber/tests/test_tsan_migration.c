/* nostrc-9965o: exercise TSan fiber lifetimes and cross-worker resumes.
 * Run in every build; under TSan repeat to cover worker-thread teardown. */
#define _POSIX_C_SOURCE 200809L
#include "libgo/fiber.h"
#include "channel.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if defined(_WIN32)
/* The CRT has _putenv_s, not POSIX setenv; these tests always overwrite. */
#define setenv(name, value, overwrite) ((void)(overwrite), _putenv_s((name), (value)))
#endif

#define PAIRS 32
#define WORKERS 4

struct observations {
    unsigned workers;
    unsigned long turns;
};
struct pair {
    GoChannel *request;
    GoChannel *reply;
    struct observations client;
    struct observations server;
};
static struct pair pairs[PAIRS];
static uint64_t end_ns;
static int stop_message;
static atomic_uint next_worker;
static _Thread_local atomic_uint worker_bit;

static uint64_t now_ns(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Resolve TLS anew on every call: a suspension may have migrated the fiber.
 * Stable integer slots remain valid after workers exit (unlike pthread_t). */
static __attribute__((noinline)) void observe(struct observations *o) {
    unsigned bit = atomic_load_explicit(&worker_bit, memory_order_relaxed);
    if (bit == 0) {
        unsigned slot = atomic_fetch_add_explicit(&next_worker, 1, memory_order_relaxed);
        assert(slot < WORKERS);
        bit = 1u << slot;
        atomic_store_explicit(&worker_bit, bit, memory_order_relaxed);
    }
    o->workers |= bit;
}

static void client(void *arg) {
    struct pair *p = arg;
    do {
        observe(&p->client);
        int stack_value = 12345;
        assert(go_channel_send(p->request, &stack_value) == 0);
        void *value = NULL;
        assert(go_channel_receive(p->reply, &value) == 0);
        assert(value == &stack_value && stack_value == 12346);
        ++p->client.turns;
        gof_yield();
        if ((p->client.turns % 8) == 0) gof_sleep_ms(1);
    } while (now_ns() < end_ns);
    assert(go_channel_send(p->request, &stop_message) == 0);
}

static void server(void *arg) {
    struct pair *p = arg;
    for (;;) {
        observe(&p->server);
        void *value = NULL;
        assert(go_channel_receive(p->request, &value) == 0);
        if (value == &stop_message) break;
        ++*(int *)value;
        gof_yield();
        assert(go_channel_send(p->reply, value) == 0);
        ++p->server.turns;
        if ((p->server.turns % 8) == 0) gof_sleep_ms(1);
    }
}

int main(void) {
    assert(setenv("GOF_NWORKERS", "4", 1) == 0);
    assert(setenv("GOF_WORKSTEAL", "1", 1) == 0);
    assert(setenv("GOF_AFFINITY", "0", 1) == 0);
    assert(setenv("GOF_STEAL_MIN_LIVE", "0", 1) == 0);
    assert(setenv("GOF_STEAL_MIN_VICTIM", "2", 1) == 0);
    gof_init(0);
    for (int i = 0; i < PAIRS; ++i) {
        pairs[i].request = go_channel_create(1);
        pairs[i].reply = go_channel_create(1);
        assert(pairs[i].request && pairs[i].reply);
        assert(gof_spawn(client, &pairs[i], 0));
        assert(gof_spawn(server, &pairs[i], 0));
    }
    end_ns = now_ns() + 3 * 1000000000ull;
    gof_run();

    int migrated = 0;
    unsigned long turns = 0;
    unsigned workers = 0;
    for (int i = 0; i < PAIRS; ++i) {
        struct pair *p = &pairs[i];
        assert(p->client.turns > 0 && p->client.turns == p->server.turns);
        turns += p->client.turns;
        struct observations *both[] = {&p->client, &p->server};
        for (int j = 0; j < 2; ++j) {
            unsigned seen = both[j]->workers;
            migrated += (seen & (seen - 1)) != 0;
            workers |= seen;
        }
        go_channel_free(p->request);
        go_channel_free(p->reply);
    }
    assert(workers == (1u << WORKERS) - 1);
    assert(migrated > 0);
    printf("migration: %d workers, %d/%d migrated fibers, %lu round trips\n",
           WORKERS, migrated, PAIRS * 2, turns);
    return 0;
}
