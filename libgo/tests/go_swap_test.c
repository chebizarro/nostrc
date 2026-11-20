#include "go.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

enum { READERS = 8, STORES = 20000 };
static const uint64_t MAGIC = UINT64_C(0xd41c7286fa3509be);

typedef struct {
    atomic_int destroyed;
    atomic_int held;
} Audit;

typedef struct {
    unsigned generation;
    uint64_t checksum;
    uint64_t words[8];
    Audit *audit;
    GoSwap *observe_on_destroy;
} Value;

static void check_value(const Value *value) {
    CHECK(value != NULL);
    CHECK(value->checksum == (MAGIC ^ value->generation));
    for (unsigned i = 0; i < 8; ++i)
        CHECK(value->words[i] == value->checksum + i);
    CHECK(atomic_load(&value->audit->destroyed) == 0);
}

static void destroy_value(void *ptr) {
    Value *value = ptr;
    check_value(value);
    CHECK(atomic_load(&value->audit->held) == 0);
    /* This would deadlock if store invoked a destructor under its lock. */
    if (value->observe_on_destroy) {
        GoRefPtr current = go_swap_load(value->observe_on_destroy);
        CHECK(current.ptr != value);
        check_value(current.ptr);
        go_refptr_release(&current);
    }
    CHECK(atomic_fetch_add(&value->audit->destroyed, 1) == 0);
    free(value);
}

static Value *new_value(unsigned generation, Audit *audit) {
    Value *value = malloc(sizeof(*value));
    CHECK(value != NULL);
    atomic_init(&audit->destroyed, 0);
    atomic_init(&audit->held, 0);
    value->generation = generation;
    value->checksum = MAGIC ^ generation;
    for (unsigned i = 0; i < 8; ++i) value->words[i] = value->checksum + i;
    value->audit = audit;
    value->observe_on_destroy = NULL;
    return value;
}

static void test_lifetime(void) {
    Audit audit[4];
    Value *initial = new_value(0, &audit[0]);
    CHECK(go_swap_create(initial, NULL) == NULL);
    CHECK(!go_swap_store(NULL, initial));
    check_value(initial); /* Failed operations did not take ownership. */
    GoSwap *swap = go_swap_create(initial, destroy_value);
    CHECK(swap != NULL);
    GoRefPtr first = go_swap_load(swap);
    GoRefPtr copy = first;
    go_refptr_retain(&copy);
    CHECK(go_swap_store(swap, new_value(1, &audit[1])));
    CHECK(atomic_load(&audit[0].destroyed) == 0);
    check_value(first.ptr);
    go_refptr_release(&first);
    CHECK(atomic_load(&audit[0].destroyed) == 0);
    go_refptr_release(&copy);
    CHECK(atomic_load(&audit[0].destroyed) == 1);

    CHECK(go_swap_store(swap, NULL));
    CHECK(atomic_load(&audit[1].destroyed) == 1);
    GoRefPtr empty = go_swap_load(swap);
    CHECK(!empty.ptr && !empty.ref_count && !empty.destructor);
    go_refptr_release(&empty);
    CHECK(go_swap_store(swap, new_value(2, &audit[2])));
    {
        go_auto(GoRefPtr) snapshot = go_swap_load(swap);
        check_value(snapshot.ptr);
    }
    GoRefPtr survivor = go_swap_load(swap);
    go_swap_free(swap);
    CHECK(atomic_load(&audit[2].destroyed) == 0);
    check_value(survivor.ptr);
    go_refptr_release(&survivor);
    CHECK(atomic_load(&audit[2].destroyed) == 1);

    swap = go_swap_create(new_value(3, &audit[3]), destroy_value);
    CHECK(swap != NULL);
    go_swap_free(swap);
    CHECK(atomic_load(&audit[3].destroyed) == 1);
    swap = go_swap_create(NULL, destroy_value);
    CHECK(swap != NULL);
    CHECK(go_swap_store(swap, NULL));
    go_swap_free(swap);
    go_swap_free(NULL);
    empty = go_swap_load(NULL);
    CHECK(!empty.ptr && !empty.ref_count && !empty.destructor);
    go_refptr_release(&empty);
}

static void test_destructor_reentry(void) {
    Audit audit[2];
    Value *initial = new_value(0, &audit[0]);
    GoSwap *swap = go_swap_create(initial, destroy_value);
    CHECK(swap != NULL);
    initial->observe_on_destroy = swap; /* Before publishing swap to any reader. */
    CHECK(go_swap_store(swap, new_value(1, &audit[1])));
    CHECK(atomic_load(&audit[0].destroyed) == 1);
    go_swap_free(swap);
    CHECK(atomic_load(&audit[1].destroyed) == 1);
}

typedef struct {
    GoSwap *swap;
    Audit *audit;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned ready;
    bool start;
    atomic_bool done;
} Stress;

typedef struct {
    Stress *stress;
    unsigned first;
    unsigned count;
} Writer;

static void hold(GoRefPtr snapshot) {
    Value *value = snapshot.ptr;
    check_value(value);
    CHECK(value->generation <= STORES);
    atomic_fetch_add(&value->audit->held, 1);
}

static void drop(GoRefPtr *snapshot) {
    Value *value = snapshot->ptr;
    check_value(value);
    CHECK(atomic_fetch_sub(&value->audit->held, 1) > 0);
    go_refptr_release(snapshot);
}

static void *reader(void *arg) {
    Stress *stress = arg;
    GoRefPtr pinned = go_swap_load(stress->swap);
    hold(pinned);
    CHECK(pthread_mutex_lock(&stress->mutex) == 0);
    ++stress->ready;
    CHECK(pthread_cond_broadcast(&stress->condition) == 0);
    while (!stress->start)
        CHECK(pthread_cond_wait(&stress->condition, &stress->mutex) == 0);
    CHECK(pthread_mutex_unlock(&stress->mutex) == 0);
    /* Main has replaced generation zero while every reader still holds it. */
    CHECK(((Value *)pinned.ptr)->generation == 0);
    drop(&pinned);
    unsigned loads = 0;
    do {
        GoRefPtr old = go_swap_load(stress->swap);
        hold(old);
        GoRefPtr newer = go_swap_load(stress->swap);
        hold(newer);
        check_value(old.ptr); /* Old remains valid across another acquisition. */
        drop(&newer);
        drop(&old);
        ++loads;
    } while (!atomic_load(&stress->done) || loads < 1024);
    return NULL;
}

static void *writer(void *arg) {
    Writer *writer = arg;
    for (unsigned i = writer->first; i < writer->first + writer->count; ++i)
        CHECK(go_swap_store(writer->stress->swap,
                            new_value(i, &writer->stress->audit[i])));
    return NULL;
}

static void test_stress(unsigned writers) {
    Stress stress = { .mutex = PTHREAD_MUTEX_INITIALIZER,
                      .condition = PTHREAD_COND_INITIALIZER };
    atomic_init(&stress.done, false);
    stress.audit = calloc(STORES + 1, sizeof(*stress.audit));
    CHECK(stress.audit != NULL);
    stress.swap = go_swap_create(new_value(0, &stress.audit[0]), destroy_value);
    CHECK(stress.swap != NULL);
    pthread_t readers[READERS], producers[2];
    Writer args[2];
    for (unsigned i = 0; i < READERS; ++i)
        CHECK(pthread_create(&readers[i], NULL, reader, &stress) == 0);
    CHECK(pthread_mutex_lock(&stress.mutex) == 0);
    while (stress.ready != READERS)
        CHECK(pthread_cond_wait(&stress.condition, &stress.mutex) == 0);
    CHECK(go_swap_store(stress.swap, new_value(1, &stress.audit[1])));
    CHECK(atomic_load(&stress.audit[0].destroyed) == 0);
    CHECK(atomic_load(&stress.audit[0].held) == READERS);
    stress.start = true;
    CHECK(pthread_cond_broadcast(&stress.condition) == 0);
    CHECK(pthread_mutex_unlock(&stress.mutex) == 0);
    unsigned first = 2;
    for (unsigned i = 0; i < writers; ++i) {
        unsigned count = (STORES - 1) / writers + (i < (STORES - 1) % writers);
        args[i] = (Writer){ .stress = &stress, .first = first, .count = count };
        first += count;
        CHECK(pthread_create(&producers[i], NULL, writer, &args[i]) == 0);
    }
    for (unsigned i = 0; i < writers; ++i)
        CHECK(pthread_join(producers[i], NULL) == 0);
    atomic_store(&stress.done, true);
    for (unsigned i = 0; i < READERS; ++i)
        CHECK(pthread_join(readers[i], NULL) == 0);
    go_swap_free(stress.swap);
    for (unsigned i = 0; i <= STORES; ++i) {
        CHECK(atomic_load(&stress.audit[i].held) == 0);
        CHECK(atomic_load(&stress.audit[i].destroyed) == 1);
    }
    CHECK(pthread_cond_destroy(&stress.condition) == 0);
    CHECK(pthread_mutex_destroy(&stress.mutex) == 0);
    free(stress.audit);
    printf("swap: %u writers, %u readers, %u stores: passed\n", writers, READERS, STORES);
}

int main(void) {
    test_lifetime();
    test_destructor_reentry();
    test_stress(1);
    test_stress(2);
    return 0;
}
