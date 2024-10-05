#ifndef GO_COUNTER_H
#define GO_COUNTER_H

#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

#define CACHE_LINE_SIZE 64

typedef struct {
    _Alignas(CACHE_LINE_SIZE) atomic_long *counters;  // Dynamically allocated counters
    int num_counters;  // Number of counters
} LongAdder;

#endif // GO_COUNTER_H