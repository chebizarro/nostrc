#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <nsync.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
#if defined(__linux__)
#include <sched.h>
#endif

#include "channel.h"
#include "context.h"

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef struct {
    GoChannel *ch;
    size_t messages;
    int pin;
    int cpu;
} producer_arg_t;

typedef struct {
    GoChannel *ch;
    int pin;
    int cpu;
} consumer_arg_t;

// Global progress ticker (across all consumers)
static _Atomic size_t g_progress = 0;
static size_t g_progress_step = 50000; // default; updated per run
static _Atomic int g_done = 0;
static _Atomic size_t g_target_total = 0; // optional early-stop signal for monitor

// Watchdog control
static _Atomic int g_watch_stop = 0;
static _Atomic int g_timed_out = 0;
static _Atomic int g_abort = 0; // set to 1 to ask threads to exit ASAP

static void *watchdog_thread(void *arg) {
    GoChannel *ch = (GoChannel *)arg;
    size_t last_in = 0, last_out = 0;
    int idle_ticks = 0;
    while (!atomic_load_explicit(&g_watch_stop, memory_order_acquire)) {
        // Read internal state non-destructively
        size_t in = atomic_load_explicit(&ch->in, memory_order_acquire);
        size_t out = atomic_load_explicit(&ch->out, memory_order_acquire);
        int closed = ch->closed; // benign racy read for diagnostics
        size_t occ = in - out;
        // If we appear stalled, print a line
        if (in == last_in && out == last_out) {
            if (++idle_ticks % 10 == 0) {
                fprintf(stderr, "[wdog] stalled? closed=%d in=%zu out=%zu occ=%zu\n", closed, in, out, occ);
                fflush(stderr);
            }
        } else {
            idle_ticks = 0;
            fprintf(stderr, "[wdog] state closed=%d in=%zu out=%zu occ=%zu\n", closed, in, out, occ);
            fflush(stderr);
        }
        last_in = in; last_out = out;
        usleep(200000); // 200ms cadence
    }
    // Final snapshot
    size_t in_f = atomic_load_explicit(&ch->in, memory_order_acquire);
    size_t out_f = atomic_load_explicit(&ch->out, memory_order_acquire);
    int closed_f = ch->closed;
    size_t occ_f = in_f - out_f;
    fprintf(stderr, "[wdog] stop closed=%d in=%zu out=%zu occ=%zu\n", closed_f, in_f, out_f, occ_f);
    fflush(stderr);
    return NULL;
}

typedef struct {
    GoChannel *ch;
    unsigned timeout_secs;
} timer_arg_t;

static void *timeout_thread(void *arg) {
    timer_arg_t *ta = (timer_arg_t *)arg;
    if (!ta || ta->timeout_secs == 0) return NULL;
    unsigned left = ta->timeout_secs;
    while (left--) {
        // If finished before timeout, exit early
        if (atomic_load_explicit(&g_done, memory_order_acquire)) return NULL;
        sleep(1);
    }
    // Timed out: request abort, close channel to unblock producers/consumers and stop watchdog/monitor
    fprintf(stderr, "[timeout] benchmark exceeded %u seconds; aborting run\n", ta->timeout_secs);
    fflush(stderr);
    atomic_store_explicit(&g_timed_out, 1, memory_order_release);
    atomic_store_explicit(&g_abort, 1, memory_order_release);
    if (ta->ch) {
        go_channel_close(ta->ch);
    }
    atomic_store_explicit(&g_watch_stop, 1, memory_order_release);
    atomic_store_explicit(&g_done, 1, memory_order_release);
    return NULL;
}

static void *monitor_thread(void *arg) {
    (void)arg;
    uint64_t last = 0;
    while (!atomic_load_explicit(&g_done, memory_order_acquire)) {
        size_t n = atomic_load_explicit(&g_progress, memory_order_relaxed);
        if (n != last) {
            fprintf(stdout, "[mon] progress=%zu\n", n);
            fflush(stdout);
            last = n;
        }
        size_t tgt = atomic_load_explicit(&g_target_total, memory_order_relaxed);
        if (tgt && n >= tgt) {
            break;
        }
        usleep(200000); // 200ms
    }
    size_t n = atomic_load_explicit(&g_progress, memory_order_relaxed);
    fprintf(stdout, "[mon] done progress=%zu\n", n);
    fflush(stdout);
    return NULL;
}

static inline int get_env_int(const char *name, int def) {
    const char *e = getenv(name);
    if (!e || !*e) return def;
    return atoi(e);
}

static inline long get_ncpu_portable(void) {
#if defined(__APPLE__)
    int mib[2] = { CTL_HW, HW_NCPU };
    int ncpu = 1; size_t len = sizeof(ncpu);
    if (sysctl(mib, 2, &ncpu, &len, NULL, 0) == 0 && ncpu > 0) return ncpu;
    return 1;
#elif defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0 ? n : 1);
#else
    return 1;
#endif
}

static inline void maybe_pin_thread(int enable, int cpu) {
    if (!enable) return;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (cpu < 0) return;
    CPU_SET((unsigned)cpu, &set);
    pthread_t th = pthread_self();
    (void)pthread_setaffinity_np(th, sizeof(set), &set);
#else
    (void)enable; (void)cpu; // no-op on non-Linux
#endif
}

static void *producer_thread(void *arg) {
    producer_arg_t *pa = (producer_arg_t *)arg;
    maybe_pin_thread(pa->pin, pa->cpu);
    for (size_t i = 0; i < pa->messages; ++i) {
        if (atomic_load_explicit(&g_abort, memory_order_acquire)) {
            break;
        }
        // Try-send loop; exit early if abort requested
        while (go_channel_try_send(pa->ch, (void*)(uintptr_t)(i + 1)) != 0) {
            if (atomic_load_explicit(&g_abort, memory_order_acquire)) {
                return NULL;
            }
            // brief backoff to avoid hot spinning in try loop
            sched_yield();
        }
        if (atomic_load_explicit(&g_abort, memory_order_acquire)) {
            break;
        }
    }
    return NULL;
}

static void *consumer_thread(void *arg) {
    consumer_arg_t *ca = (consumer_arg_t *)arg;
    maybe_pin_thread(ca->pin, ca->cpu);
    for (;;) {
        if (atomic_load_explicit(&g_abort, memory_order_acquire)) {
            break;
        }
        void *v = NULL;
        if (go_channel_try_receive(ca->ch, &v) == 0) {
            size_t n = atomic_fetch_add(&g_progress, 1) + 1;
            if (g_progress_step && (n % g_progress_step) == 0) {
                printf(".. progress: %zu messages\n", n);
                fflush(stdout);
            }
            // Do not exit early on target here; only exit on closed+empty below.
            if (atomic_load_explicit(&g_abort, memory_order_acquire)) {
                break;
            }
        } else {
            // Respect abort before blocking
            if (atomic_load_explicit(&g_abort, memory_order_acquire)) {
                break;
            }
            // blocking receive to ensure progress (will return non-zero on closed+empty)
            if (go_channel_receive(ca->ch, &v) != 0) {
                // closed and empty
                break;
            }
            size_t n = atomic_fetch_add(&g_progress, 1) + 1;
            if (g_progress_step && (n % g_progress_step) == 0) {
                printf(".. progress: %zu messages\n", n);
                fflush(stdout);
            }
            // Do not exit early on target here; only exit on closed+empty above.
            if (atomic_load_explicit(&g_abort, memory_order_acquire)) {
                break;
            }
        }
    }
    return NULL;
}

static void run_bench(size_t capacity, int prod, int cons, size_t total_msgs) {
    GoChannel *ch = go_channel_create(capacity);
    if (!ch) { fprintf(stderr, "failed to create channel\n"); exit(1); }

    // Distribute total messages exactly across producers (handle remainder)
    size_t base_msgs = total_msgs / (size_t)prod;
    size_t rem_msgs = total_msgs % (size_t)prod;

    pthread_t *pt = (pthread_t *)calloc((size_t)prod, sizeof(pthread_t));
    pthread_t *ct = (pthread_t *)calloc((size_t)cons, sizeof(pthread_t));
    producer_arg_t *pargs = (producer_arg_t *)calloc((size_t)prod, sizeof(producer_arg_t));
    consumer_arg_t *cargs = (consumer_arg_t *)calloc((size_t)cons, sizeof(consumer_arg_t));

    int pin = get_env_int("CHAN_BENCH_PIN", 0);
    int base = get_env_int("CHAN_BENCH_BASE_CPU", 0);
    long ncpu = get_ncpu_portable();
    for (int i = 0; i < prod; ++i) {
        size_t count = base_msgs + ((size_t)i < rem_msgs ? 1 : 0);
        pargs[i].ch = ch; pargs[i].messages = count; pargs[i].pin = pin;
        pargs[i].cpu = base + i;
        if (pargs[i].cpu >= ncpu) pargs[i].cpu %= (int)ncpu;
    }
    for (int i = 0; i < cons; ++i) {
        cargs[i].ch = ch; cargs[i].pin = pin;
        cargs[i].cpu = base + prod + i;
        if (cargs[i].cpu >= ncpu) cargs[i].cpu %= (int)ncpu;
    }

    // Initialize progress ticker
    atomic_store(&g_progress, 0);
    atomic_store(&g_target_total, total_msgs);
    const char *step_env = getenv("CHAN_BENCH_PROGRESS_STEP");
    if (step_env && *step_env) {
        size_t step = (size_t)strtoull(step_env, NULL, 10);
        g_progress_step = (step == 0 ? 0 : step);
    } else {
        size_t step = total_msgs / 10; // ~10 ticks
        if (step < 50000) step = (total_msgs >= 50000 ? 50000 : total_msgs);
        g_progress_step = (step == 0 ? 0 : step);
    }

    uint64_t t0 = now_ns();
    pthread_t mon;
    atomic_store_explicit(&g_done, 0, memory_order_release);
    atomic_store_explicit(&g_abort, 0, memory_order_release);
    pthread_create(&mon, NULL, monitor_thread, NULL);

    // Optional timeout controller
    unsigned timeout_secs = (unsigned)get_env_int("CHAN_BENCH_TIMEOUT_SECS", 0);
    pthread_t timer;
    timer_arg_t ta = { .ch = ch, .timeout_secs = timeout_secs };
    int timer_started = 0;
    if (timeout_secs > 0) {
        if (pthread_create(&timer, NULL, timeout_thread, &ta) == 0) {
            timer_started = 1;
        }
    }
    for (int i = 0; i < cons; ++i) pthread_create(&ct[i], NULL, consumer_thread, &cargs[i]);
    for (int i = 0; i < prod; ++i) pthread_create(&pt[i], NULL, producer_thread, &pargs[i]);

    for (int i = 0; i < prod; ++i) pthread_join(pt[i], NULL);
    // No more sends; close channel so consumers can exit when empty
    go_channel_close(ch);
    // Start watchdog after close to observe drain/exit
    pthread_t wdog;
    atomic_store_explicit(&g_watch_stop, 0, memory_order_release);
    pthread_create(&wdog, NULL, watchdog_thread, ch);
    for (int i = 0; i < cons; ++i) pthread_join(ct[i], NULL);
    // Stop watchdog once consumers are done
    atomic_store_explicit(&g_watch_stop, 1, memory_order_release);
    pthread_join(wdog, NULL);
    uint64_t t1 = now_ns();
    atomic_store_explicit(&g_done, 1, memory_order_release);
    pthread_join(mon, NULL);
    if (timer_started) {
        // timer thread may still be sleeping; ensure it exits
        pthread_join(timer, NULL);
    }

    uint64_t dtns = (t1 > t0) ? (t1 - t0) : 1; // clamp to avoid zero
    double secs = (double)dtns / 1e9;
    size_t consumed_total = atomic_load(&g_progress);

    double mps = (double)consumed_total / secs;
    int timed_out = atomic_load_explicit(&g_timed_out, memory_order_acquire);
    printf("capacity=%zu prod=%d cons=%d total=%zu time=%.3fs rate=%.0f msgs/s%s\n",
           capacity, prod, cons, consumed_total, secs, mps,
           timed_out ? " (TIMEOUT)" : "");
    fflush(stdout);

    // Channel already closed above. Just free it now.
    go_channel_free(ch);

    free(pt); free(ct); free(pargs); free(cargs);
}

static size_t parse_size(const char *s, size_t def) {
    if (!s || !*s) return def;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (end && (*end == 'k' || *end == 'K')) v *= 1000ull;
    if (end && (*end == 'm' || *end == 'M')) v *= 1000ull * 1000ull;
    if (v == 0) return def;
    return (size_t)v;
}

int main(int argc, char **argv) {
    // Env or args:
    // ARGS: capacity prod cons total_msgs
    size_t capacity = (argc > 1) ? (size_t)strtoull(argv[1], NULL, 10) : 1024;
    int prod = (argc > 2) ? atoi(argv[2]) : 1;
    int cons = (argc > 3) ? atoi(argv[3]) : 1;
    size_t total = (argc > 4) ? parse_size(argv[4], 1000000) : 1000000; // default 1M msgs
    // Optional 5th arg: timeout seconds (overrides env)
    if (argc > 5 && argv[5] && *argv[5]) {
        setenv("CHAN_BENCH_TIMEOUT_SECS", argv[5], 1);
    }

    const char *iters = getenv("NOSTR_SPIN_ITERS");
    const char *us = getenv("NOSTR_SPIN_US");
    int sweep = get_env_int("CHAN_BENCH_SWEEP", 0);
    int pin = get_env_int("CHAN_BENCH_PIN", 0);
    int base = get_env_int("CHAN_BENCH_BASE_CPU", 0);
    const char *to = getenv("CHAN_BENCH_TIMEOUT_SECS");
    printf("NOSTR_SPIN_ITERS=%s NOSTR_SPIN_US=%s PIN=%d BASE_CPU=%d SWEEP=%d TIMEOUT=%s\n",
           iters ? iters : "(default)", us ? us : "(default)", pin, base, sweep,
           to && *to ? to : "(none)");
    fflush(stdout);

    if (!sweep) {
        run_bench(capacity, prod, cons, total);
    } else {
        // Simple predefined sweep
        const size_t caps[] = {64, 256, 1024, 4096, 16384};
        const int prods[] = {1, 2, 4};
        const int conss[] = {1, 2, 4};
        const size_t totals[] = { total };
        for (size_t ic = 0; ic < sizeof(caps)/sizeof(caps[0]); ++ic) {
            for (size_t ip = 0; ip < sizeof(prods)/sizeof(prods[0]); ++ip) {
                for (size_t ik = 0; ik < sizeof(conss)/sizeof(conss[0]); ++ik) {
                    for (size_t it = 0; it < sizeof(totals)/sizeof(totals[0]); ++it) {
                        run_bench(caps[ic], prods[ip], conss[ik], totals[it]);
                    }
                }
            }
        }
    }
    return 0;
}
