#include "counter.h"
#include "go_auto.h"

#if defined(_WIN32) || defined(__MINGW32__)
#include <windows.h>
#include <malloc.h>
static int go_get_nprocs(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}
#else
#include <unistd.h> // For sysconf
static int go_get_nprocs(void) {
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
}
#endif

GO_DEFINE_AUTOPTR_CLEANUP_FUNC(LongAdder, long_adder_destroy)

LongAdder *long_adder_create(void) {
    int num_threads = go_get_nprocs(); // Get the number of available processors
    if (num_threads < 1) {
        num_threads = 1; // Fallback to at least 1 counter
    }

    /* LongAdder is over-aligned; malloc only guarantees fundamental alignment. */
#if defined(_WIN32) || defined(__MINGW32__)
    go_autoptr(LongAdder) adder = _aligned_malloc(sizeof(LongAdder), _Alignof(LongAdder));
#else
    go_autoptr(LongAdder) adder = aligned_alloc(_Alignof(LongAdder), sizeof(LongAdder));
#endif
    if (adder == NULL) {
        return NULL; // Handle allocation failure
    }
    adder->counters = NULL; // Init before potential early return

    adder->counters = malloc(sizeof(atomic_long) * num_threads);
    if (adder->counters == NULL) {
        return NULL; // adder auto-freed
    }

    adder->num_counters = num_threads;
    for (int i = 0; i < num_threads; ++i) {
        atomic_init(&adder->counters[i], 0); // Initialize all counters to 0
    }

    return go_steal_pointer(&adder); // Transfer ownership to caller
}

void long_adder_increment(LongAdder *adder) {
    size_t tid_hash = (size_t)pthread_self();
    int index = (int)(tid_hash % (size_t)adder->num_counters); // Spread increments across the available counters
    atomic_fetch_add(&adder->counters[index], 1);
}

long long long_adder_sum(LongAdder *adder) {
    long long total = 0;
    for (int i = 0; i < adder->num_counters; ++i) {
        total += atomic_load(&adder->counters[i]); // Sum all counters
    }
    return total;
}

void long_adder_reset(LongAdder *adder) {
    for (int i = 0; i < adder->num_counters; ++i) {
        atomic_store(&adder->counters[i], 0);
    }
}

void long_adder_destroy(LongAdder *adder) {
    free(adder->counters);
#if defined(_WIN32) || defined(__MINGW32__)
    _aligned_free(adder);
#else
    free(adder);
#endif
}
