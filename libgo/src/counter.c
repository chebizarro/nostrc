#include "counter.h"
#include <unistd.h>  // For sysconf

LongAdder *long_adder_create() {
    int num_threads = sysconf(_SC_NPROCESSORS_ONLN);  // Get the number of available processors
    if (num_threads < 1) {
        num_threads = 1;  // Fallback to at least 1 counter
    }

    LongAdder *adder = malloc(sizeof(LongAdder));
    if (adder == NULL) {
        return NULL;  // Handle allocation failure
    }

    adder->counters = malloc(sizeof(atomic_long) * num_threads);
    if (adder->counters == NULL) {
        free(adder);
        return NULL;  // Handle allocation failure
    }

    adder->num_counters = num_threads;
    for (int i = 0; i < num_threads; ++i) {
        atomic_init(&adder->counters[i], 0);  // Initialize all counters to 0
    }

    return adder;
}

void long_adder_increment(LongAdder *adder) {
    unsigned long thread_id = pthread_self();
    int index = thread_id % adder->num_counters;  // Spread increments across the available counters
    atomic_fetch_add(&adder->counters[index], 1);
}

long long long_adder_sum(LongAdder *adder) {
    long long total = 0;
    for (int i = 0; i < adder->num_counters; ++i) {
        total += atomic_load(&adder->counters[i]);  // Sum all counters
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
    free(adder);
}
