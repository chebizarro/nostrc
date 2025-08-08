#ifndef GO_COUNTER_H
#define GO_COUNTER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define CACHE_LINE_SIZE 64

typedef struct {
    _Alignas(CACHE_LINE_SIZE) atomic_long *counters; // Dynamically allocated counters
    int num_counters;                                // Number of counters
} LongAdder;

LongAdder *long_adder_create(void);

void long_adder_increment(LongAdder *adder);

long long long_adder_sum(LongAdder *adder);

void long_adder_reset(LongAdder *adder);

void long_adder_destroy(LongAdder *adder);

#endif // GO_COUNTER_H
