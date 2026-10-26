#ifndef GO_COUNTER_H
#define GO_COUNTER_H

#include "libgo-export.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define CACHE_LINE_SIZE 64

typedef struct {
    _Alignas(CACHE_LINE_SIZE) atomic_long *counters; // Dynamically allocated counters
    int num_counters;                                // Number of counters
} LongAdder;

LIBGO_API LongAdder *long_adder_create(void);

LIBGO_API void long_adder_increment(LongAdder *adder);

LIBGO_API long long long_adder_sum(LongAdder *adder);

LIBGO_API void long_adder_reset(LongAdder *adder);

LIBGO_API void long_adder_destroy(LongAdder *adder);

#endif // GO_COUNTER_H
