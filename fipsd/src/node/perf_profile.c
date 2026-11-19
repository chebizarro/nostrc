#include "node/perf_profile.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <errno.h>
static _Atomic unsigned long long totals[FIPSD_PERF_STAGES], counts[FIPSD_PERF_STAGES], maxima[FIPSD_PERF_STAGES];
static _Atomic unsigned long long histograms[FIPSD_PERF_STAGES][FIPSD_PERF_HIST_BUCKETS], events[FIPSD_PERF_EVENTS];
static pthread_once_t once = PTHREAD_ONCE_INIT;
static bool enabled;
static uint64_t report_interval;
static void init(void)
{
    const char *perf = getenv("FIPS_PERF"), *trace = getenv("FIPS_PIPELINE_TRACE");
    enabled = (perf && (!strcmp(perf, "1") || !strcasecmp(perf, "true"))) ||
        (trace && (!strcmp(trace, "1") || !strcasecmp(trace, "true")));
    const char *value = getenv("FIPS_PERF_INTERVAL_SECS");
    char *end = NULL; errno = 0;
    unsigned long long n = value ? strtoull(value, &end, 10) : 5;
    report_interval = !value || errno || end == value || *end || value[0] == '-' ? 5 : n;
    if (!report_interval) report_interval = 1;
    /* Keep timespec arithmetic representable; an excessive interval is inert
     * in practice, but still interruptible by shutdown. */
    if (report_interval > 31536000) report_interval = 31536000;
}
bool fipsd_perf_enabled(void) { pthread_once(&once, init); return enabled; }
unsigned fipsd_perf_bucket(uint64_t ns)
{
    if (ns <= 1) return 0;
    unsigned b = 0; --ns;
    while (ns) { ++b; ns >>= 1; }
    return b < FIPSD_PERF_HIST_BUCKETS ? b : FIPSD_PERF_HIST_BUCKETS - 1;
}
static uint64_t upper(unsigned bucket) { return UINT64_C(1) << bucket; }
static uint64_t sat_add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
uint64_t fipsd_perf_percentile(const uint64_t *hist, uint64_t total, uint64_t pct)
{
    if (!total) return 0;
    uint64_t product = pct && total > UINT64_MAX / pct ? UINT64_MAX : total * pct;
    uint64_t target = sat_add(product, 99) / 100, seen = 0;
    for (unsigned b = 0; b < FIPSD_PERF_HIST_BUCKETS; ++b) {
        seen = sat_add(seen, hist[b]);
        if (seen >= target) return upper(b);
    }
    return upper(FIPSD_PERF_HIST_BUCKETS - 1);
}
void fipsd_perf_record(fipsd_perf_stage stage, uint64_t ns)
{
    if (!fipsd_perf_enabled()) return;
    if (!ns) ns = 1;
    atomic_fetch_add_explicit(&totals[stage], ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&counts[stage], 1, memory_order_relaxed);
    unsigned long long old = atomic_load_explicit(&maxima[stage], memory_order_relaxed);
    while (old < ns && !atomic_compare_exchange_weak_explicit(&maxima[stage], &old, ns,
        memory_order_relaxed, memory_order_relaxed)) {}
    atomic_fetch_add_explicit(&histograms[stage][fipsd_perf_bucket(ns)], 1, memory_order_relaxed);
}
void fipsd_perf_event_count(fipsd_perf_event event, uint64_t n)
{
    if (n && fipsd_perf_enabled()) atomic_fetch_add_explicit(&events[event], n, memory_order_relaxed);
}
static void fmt_ns(char *buf, size_t cap, uint64_t ns)
{
    if (ns >= UINT64_C(1000000000)) snprintf(buf, cap, "%.1fs", (double)ns / 1e9);
    else if (ns >= 1000000) snprintf(buf, cap, "%.1fms", (double)ns / 1e6);
    else if (ns >= 1000) snprintf(buf, cap, "%.1fus", (double)ns / 1e3);
    else snprintf(buf, cap, "%" PRIu64 "ns", ns);
}
int fipsd_perf_report(FILE *file, uint64_t interval, fipsd_perf_snapshot *p)
{
    static const char *const stage_names[] = {
        "udp_recv",
        "fmp_decrypt",
        "link_dispatch",
        "fsp_decrypt",
        "tun_write",
        "fsp_encrypt",
        "fmp_encrypt",
        "udp_send",
        "process_packet",
        "endpoint_deliver",
        "fsp_handle",
        "endpoint_send",
        "endpoint_command_wait",
        "fmp_worker_queue_wait",
        "transport_queue_wait",
        "endpoint_event_wait",
        "publish_snapshot",
        "snapshot_header", "snapshot_peers", "snapshot_sessions",
        "snapshot_transports", "snapshot_tree", "snapshot_cache",
        "snapshot_counters", "snapshot_history",
    };
    static const char *const event_names[] = {
        "udp_send_connected",
        "udp_send_wildcard",
        "udp_send_backpressure",
        "connected_udp_installed",
        "connected_udp_activation_failed",
        "udp_send_backpressure_sleep",
    };
    if (fprintf(file, "[pipe %" PRIu64 "s]", interval) < 0) return -1;
    for (unsigned i = 0; i < FIPSD_PERF_STAGES; ++i) {
        uint64_t t = atomic_load_explicit(&totals[i], memory_order_relaxed);
        uint64_t c = atomic_load_explicit(&counts[i], memory_order_relaxed);
        uint64_t dt = t >= p->total[i] ? t - p->total[i] : 0;
        uint64_t dc = c >= p->count[i] ? c - p->count[i] : 0;
        p->total[i] = t; p->count[i] = c;
        uint64_t hist[FIPSD_PERF_HIST_BUCKETS], max = 0;
        for (unsigned b = 0; b < FIPSD_PERF_HIST_BUCKETS; ++b) {
            uint64_t h = atomic_load_explicit(&histograms[i][b], memory_order_relaxed);
            hist[b] = h >= p->hist[i][b] ? h - p->hist[i][b] : 0; p->hist[i][b] = h;
            if (hist[b]) max = upper(b);
        }
        if (!dc) continue;
        char avg[32], p50[32], p95[32], p99[32], mx[32], all[32];
        fmt_ns(avg, sizeof avg, dt / dc);
        fmt_ns(p50, sizeof p50, fipsd_perf_percentile(hist, dc, 50));
        fmt_ns(p95, sizeof p95, fipsd_perf_percentile(hist, dc, 95));
        fmt_ns(p99, sizeof p99, fipsd_perf_percentile(hist, dc, 99));
        fmt_ns(mx, sizeof mx, max);
        fmt_ns(all, sizeof all, atomic_load_explicit(&maxima[i], memory_order_relaxed));
        if (fprintf(file, " %s=%" PRIu64 "/s avg=%s p50<=%s p95<=%s p99<=%s max<=%s allmax=%s",
            stage_names[i], interval ? dc / interval : 0, avg, p50, p95, p99, mx, all) < 0) return -1;
    }
    for (unsigned i = 0; i < FIPSD_PERF_EVENTS; ++i) {
        uint64_t n = atomic_load_explicit(&events[i], memory_order_relaxed);
        uint64_t delta = n >= p->events[i] ? n - p->events[i] : 0; p->events[i] = n;
        if (delta && fprintf(file, " %s=%" PRIu64 "/s", event_names[i], interval ? delta / interval : 0) < 0) return -1;
    }
    if (fputc('\n', file) == EOF) return -1;
    return fflush(file);
}
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER, wake_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool stop_requested, started;
static pthread_t thread;
static void *run(void *unused)
{
    (void)unused; fipsd_perf_snapshot previous = {0};
    for (;;) {
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += (time_t)report_interval;
        pthread_mutex_lock(&wake_mu);
        int rc = 0;
        while (!stop_requested && rc == 0) rc = pthread_cond_timedwait(&wake, &wake_mu, &until);
        bool stop = stop_requested; pthread_mutex_unlock(&wake_mu);
        if (stop || fipsd_perf_report(stderr, report_interval, &previous)) return NULL;
    }
}
void fipsd_perf_start(void)
{
    if (!fipsd_perf_enabled()) return;
    pthread_mutex_lock(&mu);
    if (!started) {
        pthread_mutex_lock(&wake_mu); stop_requested = false; pthread_mutex_unlock(&wake_mu);
        int rc = pthread_create(&thread, NULL, run, NULL);
        if (rc) fprintf(stderr, "cannot start pipeline profiler: %s\n", strerror(rc));
        else started = true;
    }
    pthread_mutex_unlock(&mu);
}
void fipsd_perf_shutdown(void)
{
    pthread_mutex_lock(&mu);
    if (started) {
        pthread_mutex_lock(&wake_mu); stop_requested = true; pthread_cond_signal(&wake); pthread_mutex_unlock(&wake_mu);
        pthread_join(thread, NULL); started = false;
    }
    pthread_mutex_unlock(&mu);
}
