#include "nostr/metrics_collector.h"
#include "nostr/metrics.h"

#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef NOSTR_ENABLE_METRICS
#define NOSTR_ENABLE_METRICS 0
#endif

#if NOSTR_ENABLE_METRICS

/*
 * Rolling window implementation:
 * Keep a ring buffer of 60 counter snapshots (one per second).
 * Delta = current - ring[oldest].
 */

#define ROLLING_WINDOW_SECS 60
#define MAX_TRACKED_COUNTERS 128

typedef struct {
    char name[128];
    uint64_t values[ROLLING_WINDOW_SECS]; /* ring buffer of past values */
} RollingCounter;

typedef struct {
    pthread_t thread;
    _Atomic bool running;
    uint32_t interval_ms;
    char *export_path;

    /* Rolling counter history */
    RollingCounter counters[MAX_TRACKED_COUNTERS];
    int counter_count;
    int ring_pos; /* current write position in ring buffer */

    /* Latest snapshot (protected by mutex) */
    pthread_mutex_t snap_mu;
    NostrMetricsSnapshot latest;
    bool has_snapshot;
} Collector;

static Collector g_collector = {
    .running = false,
    .snap_mu = PTHREAD_MUTEX_INITIALIZER,
};

/*
 * To collect snapshots, we need access to the metrics registry internals.
 * We use the public API: nostr_metrics_prometheus() for export, and
 * walk the registry through a snapshot callback mechanism.
 *
 * For the snapshot, we parse the JSON dump output. But that's wasteful.
 * Instead, we expose a registry iteration function from metrics.c via
 * a package-private header. Since we don't have that, we'll use the
 * Prometheus export and parse it.
 *
 * Actually, the simplest approach: use the existing dump infrastructure.
 * We'll call nostr_metrics_prometheus() for file export, and build the
 * snapshot struct from the Prometheus text output.
 *
 * For the rolling window, we snapshot counter values each interval and
 * compute deltas from the ring buffer.
 */

/* Find or create a rolling counter slot */
static int find_or_add_counter(const char *name)
{
    for (int i = 0; i < g_collector.counter_count; i++) {
        if (strcmp(g_collector.counters[i].name, name) == 0) return i;
    }
    if (g_collector.counter_count >= MAX_TRACKED_COUNTERS) return -1;
    int idx = g_collector.counter_count++;
    strncpy(g_collector.counters[idx].name, name, sizeof(g_collector.counters[idx].name) - 1);
    g_collector.counters[idx].name[sizeof(g_collector.counters[idx].name) - 1] = '\0';
    memset(g_collector.counters[idx].values, 0, sizeof(g_collector.counters[idx].values));
    return idx;
}

static void snapshot_free_internals(NostrMetricsSnapshot *snap)
{
    free(snap->counters);
    free(snap->gauges);
    free(snap->histograms);
    snap->counters = NULL;
    snap->gauges = NULL;
    snap->histograms = NULL;
    snap->counter_count = 0;
    snap->gauge_count = 0;
    snap->histogram_count = 0;
}

static void collect_snapshot(NostrMetricsSnapshot *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->timestamp_ns = nostr_now_ns();

    /* Get Prometheus text output */
    size_t needed = nostr_metrics_prometheus(NULL, 0);
    if (needed == 0) return;

    char *buf = malloc(needed + 1);
    if (!buf) return;
    nostr_metrics_prometheus(buf, needed + 1);

    /* Parse the text to build snapshot arrays */
    /* Count metrics first */
    size_t n_counters = 0, n_gauges = 0, n_histograms = 0;
    const char *line = buf;
    while (line && *line) {
        if (strncmp(line, "# TYPE nostr_", 13) == 0) {
            const char *type_start = line + 13;
            const char *sp = strchr(type_start, ' ');
            if (sp) {
                sp++;
                if (strncmp(sp, "counter", 7) == 0) n_counters++;
                else if (strncmp(sp, "gauge", 5) == 0) n_gauges++;
                else if (strncmp(sp, "summary", 7) == 0) n_histograms++;
            }
        }
        const char *nl = strchr(line, '\n');
        line = nl ? nl + 1 : NULL;
    }

    snap->counters = n_counters ? calloc(n_counters, sizeof(NostrCounterSnapshot)) : NULL;
    snap->gauges = n_gauges ? calloc(n_gauges, sizeof(NostrGaugeSnapshot)) : NULL;
    snap->histograms = n_histograms ? calloc(n_histograms, sizeof(NostrHistogramSnapshot)) : NULL;

    /* Second pass: extract values */
    size_t ci = 0, gi = 0, hi = 0;
    line = buf;
    while (line && *line) {
        const char *nl = strchr(line, '\n');
        size_t line_len = nl ? (size_t)(nl - line) : strlen(line);

        if (strncmp(line, "# TYPE ", 7) == 0) {
            /* Skip TYPE lines */
        } else if (strncmp(line, "nostr_", 6) == 0 && !strchr(line, '{')) {
            /* Simple metric line: nostr_<name> <value> */
            char name[128];
            const char *p = line + 6;
            const char *sp = memchr(p, ' ', line_len - 6);
            if (sp) {
                size_t nlen = (size_t)(sp - p);
                if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
                memcpy(name, p, nlen);
                name[nlen] = '\0';

                /* Check if this is a _sum or _count suffix (histogram) */
                char *sum_suffix = strstr(name, "_sum");
                char *count_suffix = strstr(name, "_count");
                if (sum_suffix && sum_suffix == name + nlen - 4) {
                    /* histogram sum */
                    *sum_suffix = '\0';
                    for (size_t j = 0; j < hi; j++) {
                        if (snap->histograms[j].name &&
                            strcmp(snap->histograms[j].name, name) == 0) {
                            snap->histograms[j].sum_ns = strtoull(sp + 1, NULL, 10);
                            break;
                        }
                    }
                } else if (count_suffix && count_suffix == name + nlen - 6) {
                    /* histogram count */
                    *count_suffix = '\0';
                    for (size_t j = 0; j < hi; j++) {
                        if (snap->histograms[j].name &&
                            strcmp(snap->histograms[j].name, name) == 0) {
                            snap->histograms[j].count = strtoull(sp + 1, NULL, 10);
                            break;
                        }
                    }
                } else {
                    /* Regular counter or gauge — need to determine type */
                    /* Look back for the TYPE line to classify */
                    int64_t val = strtoll(sp + 1, NULL, 10);

                    /* Heuristic: if value is negative, it's a gauge.
                     * Otherwise, check if we've seen it as counter or gauge
                     * by looking at the rolling counter table. */
                    if (val < 0 || ci >= n_counters) {
                        if (gi < n_gauges) {
                            snap->gauges[gi].name = strdup(name);
                            snap->gauges[gi].value = val;
                            gi++;
                        }
                    } else {
                        if (ci < n_counters) {
                            uint64_t uval = (uint64_t)val;
                            /* Compute rolling delta */
                            uint64_t delta = 0;
                            int slot = find_or_add_counter(name);
                            if (slot >= 0) {
                                int oldest = (g_collector.ring_pos + 1) % ROLLING_WINDOW_SECS;
                                uint64_t old_val = g_collector.counters[slot].values[oldest];
                                delta = (uval >= old_val) ? uval - old_val : uval;
                                g_collector.counters[slot].values[g_collector.ring_pos] = uval;
                            }
                            snap->counters[ci].name = strdup(name);
                            snap->counters[ci].total = uval;
                            snap->counters[ci].delta_60s = delta;
                            ci++;
                        }
                    }
                }
            }
        } else if (strncmp(line, "nostr_", 6) == 0 && memchr(line, '{', line_len)) {
            /* Quantile line: nostr_<name>{quantile="0.5"} <value> */
            const char *brace = memchr(line, '{', line_len);
            if (brace) {
                size_t nlen = (size_t)(brace - line - 6);
                char name[128];
                if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
                memcpy(name, line + 6, nlen);
                name[nlen] = '\0';

                /* Find or create histogram entry */
                size_t hidx = hi;
                for (size_t j = 0; j < hi; j++) {
                    if (snap->histograms[j].name &&
                        strcmp(snap->histograms[j].name, name) == 0) {
                        hidx = j;
                        break;
                    }
                }
                if (hidx == hi && hi < n_histograms) {
                    snap->histograms[hi].name = strdup(name);
                    hi++;
                }
                if (hidx < n_histograms) {
                    const char *sp = memchr(brace, ' ', line_len - (size_t)(brace - line));
                    if (sp) {
                        uint64_t v = strtoull(sp + 1, NULL, 10);
                        if (strstr(line, "quantile=\"0.5\"")) snap->histograms[hidx].p50_ns = v;
                        else if (strstr(line, "quantile=\"0.9\"")) snap->histograms[hidx].p90_ns = v;
                        else if (strstr(line, "quantile=\"0.99\"")) snap->histograms[hidx].p99_ns = v;
                    }
                }
            }
        }

        line = nl ? nl + 1 : NULL;
    }

    snap->counter_count = ci;
    snap->gauge_count = gi;
    snap->histogram_count = hi;

    /* Advance ring position */
    g_collector.ring_pos = (g_collector.ring_pos + 1) % ROLLING_WINDOW_SECS;

    free(buf);
}

static void export_to_file(const char *path)
{
    size_t needed = nostr_metrics_prometheus(NULL, 0);
    if (needed == 0) return;

    char *buf = malloc(needed + 1);
    if (!buf) return;
    nostr_metrics_prometheus(buf, needed + 1);

    /* Write atomically: write to tmp, then rename */
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *f = fopen(tmp_path, "w");
    if (f) {
        fwrite(buf, 1, needed, f);
        fclose(f);
        rename(tmp_path, path);
    }
    free(buf);
}

static void *collector_thread(void *arg)
{
    (void)arg;
    while (atomic_load(&g_collector.running)) {
        NostrMetricsSnapshot snap;
        collect_snapshot(&snap);

        /* Store as latest */
        pthread_mutex_lock(&g_collector.snap_mu);
        snapshot_free_internals(&g_collector.latest);
        g_collector.latest = snap;
        g_collector.has_snapshot = true;
        pthread_mutex_unlock(&g_collector.snap_mu);

        /* Export to file if configured */
        if (g_collector.export_path) {
            export_to_file(g_collector.export_path);
        }

        /* Sleep for the configured interval */
        struct timespec ts;
        ts.tv_sec = g_collector.interval_ms / 1000;
        ts.tv_nsec = (long)(g_collector.interval_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
    return NULL;
}

void nostr_metrics_snapshot_free(NostrMetricsSnapshot *snap)
{
    if (!snap) return;
    for (size_t i = 0; i < snap->counter_count; i++)
        free((void *)snap->counters[i].name);
    for (size_t i = 0; i < snap->gauge_count; i++)
        free((void *)snap->gauges[i].name);
    for (size_t i = 0; i < snap->histogram_count; i++)
        free((void *)snap->histograms[i].name);
    snapshot_free_internals(snap);
}

void nostr_metrics_snapshot_collect(NostrMetricsSnapshot *snap)
{
    if (!snap) return;
    collect_snapshot(snap);
}

void nostr_metrics_collector_start(uint32_t interval_ms, const char *export_path)
{
    if (atomic_load(&g_collector.running)) return;

    g_collector.interval_ms = interval_ms > 0 ? interval_ms : 1000;
    free(g_collector.export_path);
    g_collector.export_path = export_path ? strdup(export_path) : NULL;
    g_collector.counter_count = 0;
    g_collector.ring_pos = 0;
    g_collector.has_snapshot = false;
    memset(g_collector.counters, 0, sizeof(g_collector.counters));

    atomic_store(&g_collector.running, true);
    pthread_create(&g_collector.thread, NULL, collector_thread, NULL);
    pthread_detach(g_collector.thread);
}

void nostr_metrics_collector_stop(void)
{
    if (!atomic_load(&g_collector.running)) return;
    atomic_store(&g_collector.running, false);
    /* Thread will exit on next wakeup */

    /* Clean up */
    pthread_mutex_lock(&g_collector.snap_mu);
    snapshot_free_internals(&g_collector.latest);
    g_collector.has_snapshot = false;
    pthread_mutex_unlock(&g_collector.snap_mu);

    free(g_collector.export_path);
    g_collector.export_path = NULL;
}

bool nostr_metrics_collector_running(void)
{
    return atomic_load(&g_collector.running);
}

bool nostr_metrics_collector_latest(NostrMetricsSnapshot *snap)
{
    if (!snap) return false;
    memset(snap, 0, sizeof(*snap));

    pthread_mutex_lock(&g_collector.snap_mu);
    if (!g_collector.has_snapshot) {
        pthread_mutex_unlock(&g_collector.snap_mu);
        return false;
    }

    /* Deep copy the latest snapshot */
    const NostrMetricsSnapshot *src = &g_collector.latest;
    snap->timestamp_ns = src->timestamp_ns;

    snap->counter_count = src->counter_count;
    if (src->counter_count > 0) {
        snap->counters = calloc(src->counter_count, sizeof(NostrCounterSnapshot));
        for (size_t i = 0; i < src->counter_count; i++) {
            snap->counters[i].name = src->counters[i].name ? strdup(src->counters[i].name) : NULL;
            snap->counters[i].total = src->counters[i].total;
            snap->counters[i].delta_60s = src->counters[i].delta_60s;
        }
    }

    snap->gauge_count = src->gauge_count;
    if (src->gauge_count > 0) {
        snap->gauges = calloc(src->gauge_count, sizeof(NostrGaugeSnapshot));
        for (size_t i = 0; i < src->gauge_count; i++) {
            snap->gauges[i].name = src->gauges[i].name ? strdup(src->gauges[i].name) : NULL;
            snap->gauges[i].value = src->gauges[i].value;
        }
    }

    snap->histogram_count = src->histogram_count;
    if (src->histogram_count > 0) {
        snap->histograms = calloc(src->histogram_count, sizeof(NostrHistogramSnapshot));
        for (size_t i = 0; i < src->histogram_count; i++) {
            snap->histograms[i].name = src->histograms[i].name ? strdup(src->histograms[i].name) : NULL;
            snap->histograms[i].count = src->histograms[i].count;
            snap->histograms[i].sum_ns = src->histograms[i].sum_ns;
            snap->histograms[i].min_ns = src->histograms[i].min_ns;
            snap->histograms[i].max_ns = src->histograms[i].max_ns;
            snap->histograms[i].p50_ns = src->histograms[i].p50_ns;
            snap->histograms[i].p90_ns = src->histograms[i].p90_ns;
            snap->histograms[i].p99_ns = src->histograms[i].p99_ns;
        }
    }

    pthread_mutex_unlock(&g_collector.snap_mu);
    return true;
}

#else /* !NOSTR_ENABLE_METRICS */

void nostr_metrics_snapshot_free(NostrMetricsSnapshot *snap) { if (snap) memset(snap, 0, sizeof(*snap)); }
void nostr_metrics_snapshot_collect(NostrMetricsSnapshot *snap) { if (snap) memset(snap, 0, sizeof(*snap)); }
void nostr_metrics_collector_start(uint32_t interval_ms, const char *export_path) { (void)interval_ms; (void)export_path; }
void nostr_metrics_collector_stop(void) { }
bool nostr_metrics_collector_running(void) { return false; }
bool nostr_metrics_collector_latest(NostrMetricsSnapshot *snap) { (void)snap; return false; }

#endif /* NOSTR_ENABLE_METRICS */
