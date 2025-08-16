#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Metrics API
 * - Build-time toggle: NOSTR_ENABLE_METRICS (0/1). When 0, functions are no-ops.
 * - Runtime toggle handled in init.c via environment: NOSTR_METRICS_DUMP, NOSTR_METRICS_INTERVAL_MS.
 */

/* Monotonic clock in nanoseconds */
uint64_t nostr_now_ns(void);

/* Opaque histogram handle used with timers */
typedef struct nostr_metric_histogram nostr_metric_histogram;

typedef struct nostr_metric_timer {
    uint64_t t0_ns;
} nostr_metric_timer;

/* Lookup or create a histogram by name */
nostr_metric_histogram *nostr_metric_histogram_get(const char *name);

/* Start/stop a timer and record duration into histogram */
void nostr_metric_timer_start(nostr_metric_timer *t);
void nostr_metric_timer_stop(nostr_metric_timer *t, nostr_metric_histogram *h);

/* Counters: add delta to named counter */
void nostr_metric_counter_add(const char *name, uint64_t delta);

/* Dump all metrics to stdout as a single JSON object per call */
void nostr_metrics_dump(void);

#ifdef __cplusplus
}
#endif
