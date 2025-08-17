// Full metrics backend
#include "nostr/metrics.h"

#include <stdatomic.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NOSTR_CACHELINE
#define NOSTR_CACHELINE 64
#endif

#ifndef NOSTR_ENABLE_METRICS
#define NOSTR_ENABLE_METRICS 0
#endif

uint64_t nostr_now_ns(void) {
#if defined(CLOCK_MONOTONIC_RAW)
    const clockid_t clk = CLOCK_MONOTONIC_RAW;
#else
    const clockid_t clk = CLOCK_MONOTONIC;
#endif
    struct timespec ts;
    clock_gettime(clk, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#if NOSTR_ENABLE_METRICS

// Configuration
#define NOSTR_METRICS_SHARDS 64
#define NOSTR_HIST_NUM_BINS 64
static const double k_hist_base_ns = 1000.0;         // 1 us
static const double k_hist_factor = 1.5;             // exponential growth

typedef enum { MET_COUNTER = 1, MET_HISTOGRAM = 2 } metric_type_t;

typedef struct metrics_histogram {
    // bins[i] counts samples <= bounds_ns[i]
    uint64_t bins[NOSTR_HIST_NUM_BINS];
    uint64_t count;
    __uint128_t sum_ns; // to reduce overflow; printed as 64-bit capped
    uint64_t min_ns;
    uint64_t max_ns;
} metrics_histogram __attribute__((aligned(NOSTR_CACHELINE)));

// Forward-declare histogram handle used by public API (opaque to callers)
typedef struct nostr_metric_histogram {
    int shard;
    struct metrics_entry *entry; // type == MET_HISTOGRAM
} nostr_metric_histogram;

typedef struct metrics_entry {
    // Place hot data first and align to its own cache line
    union {
        struct {
            _Atomic unsigned long long counter;
            // Pad to dedicate a full cache line to the counter to reduce ping-pong
            char _pad_counter[NOSTR_CACHELINE - sizeof(unsigned long long)];
        } c;
        metrics_histogram hist;
    } u __attribute__((aligned(NOSTR_CACHELINE)));
    struct metrics_entry *next;
    char *name;
    metric_type_t type;
    int shard_index;                         // cached shard index for this name
    nostr_metric_histogram *hist_handle;     // cached handle for fast returns
} metrics_entry __attribute__((aligned(NOSTR_CACHELINE)));

typedef struct metrics_shard {
    pthread_mutex_t mu;
    metrics_entry *head;
} metrics_shard;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static metrics_shard g_shards[NOSTR_METRICS_SHARDS];
static uint64_t g_bounds_ns[NOSTR_HIST_NUM_BINS];
// Forward declaration for TLS flush interval (defined later with default)
static uint64_t g_tls_flush_ns;

static void metrics_init_once(void)
{
    for (int i = 0; i < NOSTR_METRICS_SHARDS; ++i) {
        pthread_mutex_init(&g_shards[i].mu, NULL);
        g_shards[i].head = NULL;
    }
    double v = k_hist_base_ns;
    for (int i = 0; i < NOSTR_HIST_NUM_BINS; ++i) {
        g_bounds_ns[i] = (uint64_t)(v);
        v *= k_hist_factor;
    }
    // Optional: configure TLS counter flush interval from environment
    const char *env_flush = getenv("NOSTR_COUNTER_FLUSH_NS");
    if (env_flush && *env_flush) {
        char *endp = NULL;
        unsigned long long x = strtoull(env_flush, &endp, 10);
        if (endp != env_flush && x > 0) {
            g_tls_flush_ns = (uint64_t)x;
        }
    }
}

static inline uint64_t fnv1a_64(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) { h ^= (uint64_t)(*p++); h *= 1099511628211ull; }
    return h;
}

static inline int shard_index(uint64_t h) { return (int)(h & (NOSTR_METRICS_SHARDS - 1)); }

static metrics_entry *registry_get_or_create(const char *name, metric_type_t type)
{
    pthread_once(&g_once, metrics_init_once);
    uint64_t h = fnv1a_64(name);
    int si = shard_index(h);
    metrics_shard *sh = &g_shards[si];
    pthread_mutex_lock(&sh->mu);
    for (metrics_entry *e = sh->head; e; e = e->next) {
        if (e->type == type && strcmp(e->name, name) == 0) {
            // Ensure cached fields populated for histograms
            e->shard_index = si;
            if (type == MET_HISTOGRAM && e->hist_handle == NULL) {
                nostr_metric_histogram *handle = (nostr_metric_histogram *)malloc(sizeof(*handle));
                if (handle) { handle->shard = si; handle->entry = e; e->hist_handle = handle; }
            }
            pthread_mutex_unlock(&sh->mu);
            return e;
        }
    }
    // create
    metrics_entry *e = (metrics_entry *)calloc(1, sizeof(*e));
    if (!e) { pthread_mutex_unlock(&sh->mu); return NULL; }
    e->name = strdup(name);
    e->type = type;
    e->shard_index = si;
    if (type == MET_HISTOGRAM) {
        e->u.hist.min_ns = UINT64_MAX;
        e->u.hist.max_ns = 0;
        e->u.hist.sum_ns = 0;
        // bins/count zeroed by calloc
        // Create and cache a single handle per histogram metric
        nostr_metric_histogram *handle = (nostr_metric_histogram *)malloc(sizeof(*handle));
        if (handle) { handle->shard = si; handle->entry = e; e->hist_handle = handle; }
    } else {
        atomic_store(&e->u.c.counter, 0);
    }
    e->next = sh->head;
    sh->head = e;
    pthread_mutex_unlock(&sh->mu);
    return e;
}

// ----------------------------
// Per-thread counter batching
// ----------------------------
// We keep a tiny TLS cache of (name,pending) and periodically flush to the
// global registry to reduce contention on atomic_fetch_add and shard mutexes.
// This preserves the public API and call sites.

#ifndef NOSTR_COUNTER_TLS_SLOTS
#define NOSTR_COUNTER_TLS_SLOTS 32
#endif
#ifndef NOSTR_COUNTER_FLUSH_NS
#define NOSTR_COUNTER_FLUSH_NS 1000000ull /* 1 ms */
#endif
static uint64_t g_tls_flush_ns = NOSTR_COUNTER_FLUSH_NS;

typedef struct tls_counter_slot {
    const char *name;     // key (assumed long-lived string literal in hot paths)
    uint64_t pending;     // accumulated delta
} tls_counter_slot;

typedef struct tls_counter_cache {
    uint64_t last_flush_ns;
    int used;
    tls_counter_slot slots[NOSTR_COUNTER_TLS_SLOTS];
} tls_counter_cache;

#if defined(__APPLE__) || defined(__GNUC__)
static __thread tls_counter_cache g_tls_counters;
#else
static _Thread_local tls_counter_cache g_tls_counters;
#endif

static inline void tls_counters_flush(tls_counter_cache *c)
{
    if (!c) return;
    for (int i = 0; i < c->used; ++i) {
        tls_counter_slot *s = &c->slots[i];
        if (!s->name || s->pending == 0) continue;
        metrics_entry *e = registry_get_or_create(s->name, MET_COUNTER);
        if (e) {
            atomic_fetch_add(&e->u.c.counter, (unsigned long long)s->pending);
        }
        s->pending = 0;
    }
    c->last_flush_ns = nostr_now_ns();
}

static inline void tls_counters_add(const char *name, uint64_t delta)
{
    tls_counter_cache *c = &g_tls_counters;
    // Time-based flush
    uint64_t now = nostr_now_ns();
    if (c->last_flush_ns == 0) c->last_flush_ns = now;
    if (now - c->last_flush_ns > g_tls_flush_ns) {
        tls_counters_flush(c);
    }

    // Try to find existing slot (pointer match for speed)
    for (int i = 0; i < c->used; ++i) {
        if (c->slots[i].name == name) {
            c->slots[i].pending += delta;
            return;
        }
    }
    // New slot if space
    if (c->used < NOSTR_COUNTER_TLS_SLOTS) {
        c->slots[c->used].name = name;
        c->slots[c->used].pending = delta;
        c->used++;
        return;
    }
    // Cache full: flush everything then insert into first slot
    tls_counters_flush(c);
    c->used = 0;
    c->slots[c->used].name = name;
    c->slots[c->used].pending = delta;
    c->used = 1;
}

static inline int hist_bin_index(uint64_t ns)
{
    // find first bound >= ns
    int lo = 0, hi = NOSTR_HIST_NUM_BINS - 1, ans = hi;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (g_bounds_ns[mid] >= ns) { ans = mid; hi = mid - 1; }
        else lo = mid + 1;
    }
    return ans;
}

static void hist_record_locked(metrics_histogram *h, uint64_t ns)
{
    int idx = hist_bin_index(ns);
    h->bins[idx]++;
    h->count++;
    __uint128_t s = h->sum_ns + (__uint128_t)ns;
    h->sum_ns = s;
    if (ns < h->min_ns) h->min_ns = ns;
    if (ns > h->max_ns) h->max_ns = ns;
}

nostr_metric_histogram *nostr_metric_histogram_get(const char *name)
{
    pthread_once(&g_once, metrics_init_once);
    metrics_entry *e = registry_get_or_create(name, MET_HISTOGRAM);
    if (!e) return NULL;
    // Return cached handle (created once under shard mutex)
    return e->hist_handle;
}

void nostr_metric_timer_start(nostr_metric_timer *t)
{
    if (!t) return;
    t->t0_ns = nostr_now_ns();
}

void nostr_metric_timer_stop(nostr_metric_timer *t, nostr_metric_histogram *h)
{
    if (!t || !h || !h->entry) return;
    uint64_t dt = nostr_now_ns() - t->t0_ns;
    metrics_shard *sh = &g_shards[h->shard];
    pthread_mutex_lock(&sh->mu);
    hist_record_locked(&h->entry->u.hist, dt);
    pthread_mutex_unlock(&sh->mu);
}

void __attribute__((hot)) nostr_metric_counter_add(const char *name, uint64_t delta)
{
    // Fast-path: batch into a thread-local cache
    tls_counters_add(name, delta);
}

static uint64_t percentile_estimate(const metrics_histogram *h, double p)
{
    if (h->count == 0) return 0;
    uint64_t target = (uint64_t)((double)h->count * p);
    if (target == 0) target = 1;
    uint64_t cum = 0;
    for (int i = 0; i < NOSTR_HIST_NUM_BINS; ++i) {
        cum += h->bins[i];
        if (cum >= target) {
            return g_bounds_ns[i];
        }
    }
    return h->max_ns;
}

void nostr_metrics_dump(void)
{
    // Flush TLS counters for this thread before snapshotting
    tls_counters_flush(&g_tls_counters);
    // Output a compact JSON object containing counters and histograms
    // {"counters":{...},"histograms":{...}}
    pthread_once(&g_once, metrics_init_once);
    fputs("{\"counters\":{", stdout);
    int first = 1;
    // Counters
    for (int si = 0; si < NOSTR_METRICS_SHARDS; ++si) {
        metrics_shard *sh = &g_shards[si];
        pthread_mutex_lock(&sh->mu);
        for (metrics_entry *e = sh->head; e; e = e->next) {
            if (e->type != MET_COUNTER) continue;
            unsigned long long v = atomic_load(&e->u.c.counter);
            if (!first) fputc(',', stdout); else first = 0;
            fprintf(stdout, "\"%s\":%llu", e->name, v);
        }
        pthread_mutex_unlock(&sh->mu);
    }
    fputs("},\"histograms\":{", stdout);
    first = 1;
    for (int si = 0; si < NOSTR_METRICS_SHARDS; ++si) {
        metrics_shard *sh = &g_shards[si];
        pthread_mutex_lock(&sh->mu);
        for (metrics_entry *e = sh->head; e; e = e->next) {
            if (e->type != MET_HISTOGRAM) continue;
            const metrics_histogram *h = &e->u.hist;
            if (!first) fputc(',', stdout); else first = 0;
            // sum may exceed 64 bits; cap for printing
            unsigned long long sum64 = (unsigned long long)(h->sum_ns > ((__uint128_t)~0ull) ? ~0ull : h->sum_ns);
            uint64_t p50 = percentile_estimate(h, 0.50);
            uint64_t p90 = percentile_estimate(h, 0.90);
            uint64_t p99 = percentile_estimate(h, 0.99);
            fprintf(stdout, "\"%s\":{\"count\":%llu,\"sum_ns\":%llu,\"min_ns\":%llu,\"max_ns\":%llu,\"p50_ns\":%llu,\"p90_ns\":%llu,\"p99_ns\":%llu,\"bins\":[",
                    e->name,
                    (unsigned long long)h->count,
                    sum64,
                    (unsigned long long)(h->count ? h->min_ns : 0ull),
                    (unsigned long long)h->max_ns,
                    (unsigned long long)p50,
                    (unsigned long long)p90,
                    (unsigned long long)p99);
            for (int bi = 0; bi < NOSTR_HIST_NUM_BINS; ++bi) {
                if (bi) fputc(',', stdout);
                fprintf(stdout, "%llu", (unsigned long long)h->bins[bi]);
            }
            fputs("],\"bounds_ns\":[", stdout);
            for (int bi = 0; bi < NOSTR_HIST_NUM_BINS; ++bi) {
                if (bi) fputc(',', stdout);
                fprintf(stdout, "%llu", (unsigned long long)g_bounds_ns[bi]);
            }
            fputs("]}", stdout);
        }
        pthread_mutex_unlock(&sh->mu);
    }
    fputs("}}\n", stdout);
    fflush(stdout);
}

#else // !NOSTR_ENABLE_METRICS

// No-op implementations
nostr_metric_histogram *nostr_metric_histogram_get(const char *name) { (void)name; return NULL; }
void nostr_metric_timer_start(nostr_metric_timer *t) { if (t) t->t0_ns = 0; }
void nostr_metric_timer_stop(nostr_metric_timer *t, nostr_metric_histogram *h) { (void)t; (void)h; }
void nostr_metric_counter_add(const char *name, uint64_t delta) { (void)name; (void)delta; }
void nostr_metrics_dump(void) { }

#endif // NOSTR_ENABLE_METRICS
