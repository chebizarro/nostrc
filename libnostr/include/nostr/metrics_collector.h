#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MetricsCollector — Central metrics aggregation with rolling windows.
 *
 * Provides:
 *   - Periodic snapshots of all metrics (counters, gauges, histograms)
 *   - Rolling window deltas for counters (last 60s)
 *   - Rolling window histograms (current window only, reset each period)
 *   - File export in Prometheus text format
 *   - Background collection thread with configurable interval
 *
 * The collector reads from the core metrics registry (metrics.h) and
 * produces snapshots that higher layers (UI dashboard, HTTP endpoint)
 * can consume.
 */

/* Snapshot of a single counter metric */
typedef struct {
    const char *name;
    uint64_t total;       /* Cumulative value */
    uint64_t delta_60s;   /* Change over the last 60 seconds */
} NostrCounterSnapshot;

/* Snapshot of a single gauge metric */
typedef struct {
    const char *name;
    int64_t value;
} NostrGaugeSnapshot;

/* Snapshot of a single histogram metric */
typedef struct {
    const char *name;
    uint64_t count;
    uint64_t sum_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t p50_ns;
    uint64_t p90_ns;
    uint64_t p99_ns;
} NostrHistogramSnapshot;

/* Full metrics snapshot — all metrics at a point in time */
typedef struct {
    uint64_t timestamp_ns;          /* When this snapshot was taken */

    NostrCounterSnapshot *counters;
    size_t counter_count;

    NostrGaugeSnapshot *gauges;
    size_t gauge_count;

    NostrHistogramSnapshot *histograms;
    size_t histogram_count;
} NostrMetricsSnapshot;

/* Free a snapshot's internal arrays (not the struct itself if stack-allocated) */
void nostr_metrics_snapshot_free(NostrMetricsSnapshot *snap);

/*
 * Take an immediate snapshot of all registered metrics.
 * Caller must call nostr_metrics_snapshot_free() when done.
 */
void nostr_metrics_snapshot_collect(NostrMetricsSnapshot *snap);

/*
 * Start the background collector thread.
 *   interval_ms: collection interval in milliseconds (e.g. 1000 for 1s)
 *   export_path: if non-NULL, write Prometheus text to this file each interval
 *
 * Call nostr_metrics_collector_stop() to shut down.
 * Only one collector can run at a time; repeated calls are no-ops.
 */
void nostr_metrics_collector_start(uint32_t interval_ms, const char *export_path);

/* Stop the background collector thread. Safe to call if not started. */
void nostr_metrics_collector_stop(void);

/* Returns true if the collector is currently running */
bool nostr_metrics_collector_running(void);

/*
 * Get the most recent snapshot from the collector.
 * Returns false if no snapshot is available (collector not started).
 * The snapshot is a copy — caller must call nostr_metrics_snapshot_free().
 */
bool nostr_metrics_collector_latest(NostrMetricsSnapshot *snap);

#ifdef __cplusplus
}
#endif
