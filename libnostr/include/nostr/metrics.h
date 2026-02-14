#ifndef LIBNOSTR_NOSTR_METRICS_H
#define LIBNOSTR_NOSTR_METRICS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Metrics API
 * - Build-time toggle: NOSTR_ENABLE_METRICS (0/1). When 0, functions are no-ops.
 * - Runtime toggle handled in init.c via environment: NOSTR_METRICS_DUMP, NOSTR_METRICS_INTERVAL_MS.
 *
 * Three metric types:
 *   Counter   - monotonically increasing value (e.g. events_received)
 *   Gauge     - point-in-time value that can go up/down (e.g. active_subscriptions)
 *   Histogram - distribution of values in exponential bins (e.g. dispatch_latency_ns)
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

/* Record an explicit value into a histogram (in nanoseconds) */
void nostr_metric_histogram_record(nostr_metric_histogram *h, uint64_t value_ns);

/* Start/stop a timer and record duration into histogram */
void nostr_metric_timer_start(nostr_metric_timer *t);
void nostr_metric_timer_stop(nostr_metric_timer *t, nostr_metric_histogram *h);

/* Counters: add delta to named counter */
void nostr_metric_counter_add(const char *name, uint64_t delta);

/* Gauges: set a named gauge to a specific value */
void nostr_metric_gauge_set(const char *name, int64_t value);

/* Gauges: increment/decrement a named gauge by delta */
void nostr_metric_gauge_inc(const char *name);
void nostr_metric_gauge_dec(const char *name);

/* Dump all metrics to stdout as a single JSON object per call */
void nostr_metrics_dump(void);

/*
 * Prometheus text exposition format export.
 * Writes metrics in Prometheus text format to the provided buffer.
 * Returns the number of bytes written (excluding null terminator),
 * or the required buffer size if buf is NULL or too small.
 */
size_t nostr_metrics_prometheus(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
#endif /* LIBNOSTR_NOSTR_METRICS_H */
