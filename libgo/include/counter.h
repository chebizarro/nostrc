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

#endif // GO_COUNTER_H