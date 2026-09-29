/* Feature test macros must come before any includes */
#define _POSIX_C_SOURCE 200809L  /* clock_gettime, CLOCK_MONOTONIC, usleep */
#define _DEFAULT_SOURCE          /* usleep on glibc 2.19+ */

#include "channel.h"
#include "select.h"
#include "fiber_hooks.h"
#include "nostr/metrics.h"
#include "context.h"
#include <sched.h>     /* sched_yield for CPU_RELAX fallback */
#include <time.h>      /* clock_gettime for deferred free */
#include <pthread.h>   /* graveyard mutex */
#include <unistd.h>    /* usleep for waiter drain */

/* Implemented in select.c — signals select waiters registered on this channel.
 * Must be called while holding chan->mutex. */
extern void go_channel_signal_select_waiters(GoChannel *chan);
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ── Fiber waiter infrastructure ──────────────────────────────────────
 * When a fiber calls go_channel_send/receive and the channel would block,
 * instead of calling nsync_cv_wait (which blocks the OS worker thread and
 * starves the fiber scheduler), we park the fiber cooperatively.
 *
 * A GoFiberWaiter is stack-allocated by the blocking fiber, linked into
 * the channel's fiber_waiters_full or fiber_waiters_empty list, and the
 * fiber is parked. When the channel transitions (send signals empty waiters,
 * receive signals full waiters), the fiber is made runnable again.
 */
typedef struct GoFiberWaiter {
    gof_fiber_handle fiber;          /* The parked fiber handle */
    struct GoFiberWaiter *next;      /* Linked list */
} GoFiberWaiter;

/* Wake one fiber waiter from a list. Returns the removed waiter, or NULL.
 * Must be called while holding chan->mutex. */
/* Used only by the REFINED_SIGNALING single-wakeup macros. */
static __attribute__((unused)) GoFiberWaiter *fiber_waiter_wake_one(GoFiberWaiter **list) {
    GoFiberWaiter *w = *list;
    if (w) {
        *list = w->next;
        w->next = NULL;
        gof_hook_make_runnable(w->fiber);
    }
    return w;
}

/* Wake ALL fiber waiters from a list (used on close).
 * Must be called while holding chan->mutex. */
static void fiber_waiter_wake_all(GoFiberWaiter **list) {
    GoFiberWaiter *w = *list;
    while (w) {
        GoFiberWaiter *next = w->next;
        w->next = NULL;
        gof_hook_make_runnable(w->fiber);
        w = next;
    }
    *list = NULL;
}

/* Add a fiber waiter to a list. Must hold chan->mutex. */
static void fiber_waiter_enqueue(GoFiberWaiter **list, GoFiberWaiter *w) {
    w->next = *list;
    *list = w;
}

/* Remove a specific fiber waiter from a list. Must hold chan->mutex.
 * Returns 1 if found and removed, 0 if not found. */
static int fiber_waiter_remove(GoFiberWaiter **list, GoFiberWaiter *w) {
    GoFiberWaiter **pp = list;
    while (*pp) {
        if (*pp == w) {
            *pp = w->next;
            w->next = NULL;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

/* TSAN-aware mutex/condvar helpers — shared with select.c via go_tsan.h so
 * every file touching the same nsync_mu uses identical TSAN annotations. */
#include "go_tsan.h"

/* ── Fiber-aware CV_WAIT ──────────────────────────────────────────────
 * If called from within a fiber context, parks the fiber cooperatively
 * instead of blocking the OS worker thread. The fiber is registered on
 * the channel's fiber waiter list so it can be woken when the channel
 * state transitions (send/recv/close).
 *
 * chan_ptr:    GoChannel* — needed to access the fiber waiter list
 * cv_ptr:     &chan->cond_full or &chan->cond_empty
 * mu_ptr:     &chan->mutex (must be held on entry, re-held on return)
 * waiter_list: &chan->fiber_waiters_full or &chan->fiber_waiters_empty
 */
#define CV_WAIT_FIBER(chan_ptr, cv_ptr, mu_ptr, waiter_list) do { \
    gof_fiber_handle _cur_fiber = gof_hook_current();              \
    if (_cur_fiber) {                                               \
        /* Fiber path: park cooperatively instead of blocking OS thread */ \
        GoFiberWaiter _fw = { .fiber = _cur_fiber, .next = NULL };  \
        fiber_waiter_enqueue((waiter_list), &_fw);                  \
        NUNLOCK(mu_ptr);                                            \
        gof_hook_block_current(); /* Parks fiber — OS thread is freed */ \
        NLOCK(mu_ptr);                                              \
        /* Remove ourselves from the waiter list to prevent use-after-free. \
         * The waiter may have already been removed by fiber_waiter_wake_one \
         * if we were woken by a signal, but we must ensure removal in case \
         * of spurious wakeup or broadcast. */                      \
        fiber_waiter_remove((waiter_list), &_fw);                   \
    } else {                                                        \
        /* OS thread path: use normal nsync_cv_wait */              \
        CV_WAIT_OS((cv_ptr), (mu_ptr));                             \
    }                                                               \
} while(0)

/* ── Fiber-aware signal/broadcast ──────────────────────────────────────
 * These wrappers wake both OS-thread waiters (via nsync_cv) AND
 * fiber waiters (via the fiber waiter list). Must hold chan->mutex.
 *
 * "signal" wakes ONE waiter; "broadcast" wakes ALL.
 * We always signal both the CV and the fiber list because mixed
 * fiber+thread workloads may have waiters in both paths.
 */
#define CV_SIGNAL_EMPTY(chan) do {                       \
    nsync_cv_signal(&(chan)->cond_empty);                \
    fiber_waiter_wake_one(&(chan)->fiber_waiters_empty); \
} while(0)

#define CV_BROADCAST_EMPTY(chan) do {                    \
    nsync_cv_broadcast(&(chan)->cond_empty);             \
    fiber_waiter_wake_all(&(chan)->fiber_waiters_empty); \
} while(0)

#define CV_SIGNAL_FULL(chan) do {                        \
    nsync_cv_signal(&(chan)->cond_full);                 \
    fiber_waiter_wake_one(&(chan)->fiber_waiters_full);  \
} while(0)

#define CV_BROADCAST_FULL(chan) do {                     \
    nsync_cv_broadcast(&(chan)->cond_full);              \
    fiber_waiter_wake_all(&(chan)->fiber_waiters_full);  \
} while(0)

/* ── Channel Graveyard (nostrc-deferred-free) ─────────────────────────
 *
 * When go_channel_unref drops the last reference, we CANNOT immediately
 * free the GoChannel struct. Woken waiters (broadcast in Phase 1) are
 * still inside nsync_mu_lock trying to reacquire chan->mutex. If we free
 * the struct, nsync writes to freed memory — corrupting glibc's fastbin
 * metadata and causing `malloc_consolidate(): unaligned fastbin chunk`.
 *
 * ASAN doesn't catch this because nsync is compiled without ASAN
 * instrumentation. The old sched_yield × 3 hack was timing-dependent
 * and failed under load.
 *
 * Solution: dead channels go into a graveyard list with a timestamp.
 * They are only freed once 1 second has elapsed, guaranteeing all nsync
 * waiters have long since exited. Reaping is amortized onto channel_create
 * and channel_unref calls.
 */

#define GO_GRAVEYARD_DELAY_NS (5000000000ULL)  /* 5 seconds — increased from 1s to give
                                                * nsync waiters more time to drain after
                                                * channel destruction (nostrc-imgdef). */

typedef struct GoDeadChannel {
    GoChannel            *chan;
    uint64_t              death_ns;
    struct GoDeadChannel *next;
} GoDeadChannel;

static pthread_mutex_t g_graveyard_mu = PTHREAD_MUTEX_INITIALIZER;
static GoDeadChannel  *g_graveyard_head = NULL;

static uint64_t graveyard_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Add a dead channel to the graveyard. Called from go_channel_unref. */
static void go_channel_graveyard_add(GoChannel *chan) {
    GoDeadChannel *d = (GoDeadChannel *)malloc(sizeof(*d));
    if (!d) { /* OOM — leak the channel rather than crash */ return; }
    d->chan = chan;
    d->death_ns = graveyard_now_ns();
    pthread_mutex_lock(&g_graveyard_mu);
    d->next = g_graveyard_head;
    g_graveyard_head = d;
    pthread_mutex_unlock(&g_graveyard_mu);
}

/* Reap channels dead for longer than GO_GRAVEYARD_DELAY_NS.
 * Called opportunistically from channel_create and channel_unref.
 *
 * SAFETY (nostrc-select-refcount): Before freeing a dead channel, check
 * if anyone took a ref on it (e.g., go_select via ref_channels). If
 * refs > 0, skip it — the channel will be freed when the ref is dropped
 * and the graveyard is reaped again. This prevents use-after-free when
 * go_select is operating on a channel that was recently unreffed. */
static void go_channel_graveyard_reap(void) {
    uint64_t cutoff = graveyard_now_ns() - GO_GRAVEYARD_DELAY_NS;

    pthread_mutex_lock(&g_graveyard_mu);
    GoDeadChannel **pp = &g_graveyard_head;
    while (*pp) {
        GoDeadChannel *d = *pp;
        if (d->death_ns <= cutoff) {
            /* Check if someone has taken a ref on the dead channel.
             * This can happen if go_select ref'd it just before
             * go_channel_unref cleared the magic number. */
            int refs = atomic_load_explicit(&d->chan->refs, memory_order_acquire);
            if (refs > 0) {
                /* Still referenced — skip, will retry on next reap */
                pp = &d->next;
                continue;
            }
            /* nostrc-waiter-count: Check if any threads are still blocked
             * inside go_channel_send/receive. These threads are touching
             * the channel's nsync_mu mutex and will corrupt heap if we free. */
            int waiters = atomic_load_explicit(&d->chan->active_waiters, memory_order_acquire);
            if (waiters > 0) {
                /* Still has active waiters — skip, will retry on next reap */
                pp = &d->next;
                continue;
            }
            *pp = d->next;
            pthread_mutex_unlock(&g_graveyard_mu);
            free(d->chan);
            free(d);
            pthread_mutex_lock(&g_graveyard_mu);
            /* Restart scan from head since list may have changed */
            pp = &g_graveyard_head;
        } else {
            pp = &d->next;
        }
    }
    pthread_mutex_unlock(&g_graveyard_mu);
}

// Portable aligned allocation: prefer C11 aligned_alloc, fallback to malloc
static inline void *go_aligned_alloc(size_t alignment, size_t size) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__MINGW32__) && !defined(__MINGW64__)
    // aligned_alloc requires size to be a multiple of alignment
    size_t mask = alignment - 1;
    size_t adj = (size + mask) & ~mask;
    void *p = aligned_alloc(alignment, adj);
    if (!p) return malloc(size);
    return p;
#else
    // Best-effort fallback
    (void)alignment;
    return malloc(size);
#endif
}
// Ensure occupancy is derived from in/out counters in MPMC mode
#if NOSTR_CHANNEL_MPMC_SLOTS
#ifndef NOSTR_CHANNEL_DERIVE_SIZE
#define NOSTR_CHANNEL_DERIVE_SIZE 1
#endif
#endif

// Branch prediction hints
#ifndef NOSTR_LIKELY
#define NOSTR_LIKELY(x)   __builtin_expect(!!(x), 1)
#define NOSTR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

// Spin-then-park tuning (micro-waits before longer parks)
#ifndef NOSTR_SPIN_ITERS
#define NOSTR_SPIN_ITERS 20
#endif
#ifndef NOSTR_SPIN_US
#define NOSTR_SPIN_US 10
#endif

// CPU relax hint for tight spin loops (portable)
#ifndef NOSTR_CPU_RELAX
# if defined(__aarch64__) || defined(__arm__)
#  ifdef NOSTR_ARM_WFE
#   define NOSTR_CPU_RELAX() __asm__ __volatile__("wfe" ::: "memory")
#   define NOSTR_EVENT_SEND() __asm__ __volatile__("sev" ::: "memory")
#  else
#   define NOSTR_CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")
#   define NOSTR_EVENT_SEND() ((void)0)
#  endif
# elif defined(__x86_64__) || defined(__i386__)
#  ifdef NOSTR_X86_TPAUSE
static inline void nostr_tpause_short(void) {
    // Use rdtsc to set a very short TSC deadline and TPAUSE C0.2 state
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t now = ((uint64_t)hi << 32) | lo;
    uint64_t deadline = now + 1024; // ~very short pause; tune if needed
    unsigned int d_lo = (unsigned int)(deadline & 0xFFFFFFFFu);
    unsigned int d_hi = (unsigned int)(deadline >> 32);
    // tpause uses EAX/EDX deadline, ECX hints=0
    __asm__ __volatile__(
        "mov %%eax, %%eax\n\t"
        :
        : "a"(d_lo), "d"(d_hi), "c"(0)
        : "memory");
    __asm__ __volatile__("tpause" ::: "memory");
}
#   define NOSTR_CPU_RELAX() nostr_tpause_short()
#  else
#   define NOSTR_CPU_RELAX() __builtin_ia32_pause()
#  endif
#  define NOSTR_EVENT_SEND() ((void)0)
# else
#  include <sched.h>
#  define NOSTR_CPU_RELAX() sched_yield()
#  define NOSTR_EVENT_SEND() ((void)0)
# endif
#endif

// Optional prefetch distance for ring slots (0 = only current index)
#ifndef NOSTR_PREFETCH_DISTANCE
#define NOSTR_PREFETCH_DISTANCE 1
#endif

// Fast-path index increment helpers with power-of-two wrap
// Cache histogram handles for hot-path timer stops
static _Atomic(nostr_metric_histogram*) h_send_wait_ns = NULL;
static _Atomic(nostr_metric_histogram*) h_recv_wait_ns = NULL;
static _Atomic(nostr_metric_histogram*) h_send_wakeup_to_progress_ns = NULL;
static _Atomic(nostr_metric_histogram*) h_recv_wakeup_to_progress_ns = NULL;

static inline void ensure_histos(void) {
    nostr_metric_histogram *p;
    p = atomic_load_explicit(&h_send_wait_ns, memory_order_acquire);
    if (!p) {
        p = nostr_metric_histogram_get("go_chan_send_wait_ns");
        atomic_store_explicit(&h_send_wait_ns, p, memory_order_release);
    }
    p = atomic_load_explicit(&h_recv_wait_ns, memory_order_acquire);
    if (!p) {
        p = nostr_metric_histogram_get("go_chan_recv_wait_ns");
        atomic_store_explicit(&h_recv_wait_ns, p, memory_order_release);
    }
    p = atomic_load_explicit(&h_send_wakeup_to_progress_ns, memory_order_acquire);
    if (!p) {
        p = nostr_metric_histogram_get("go_chan_send_wakeup_to_progress_ns");
        atomic_store_explicit(&h_send_wakeup_to_progress_ns, p, memory_order_release);
    }
    p = atomic_load_explicit(&h_recv_wakeup_to_progress_ns, memory_order_acquire);
    if (!p) {
        p = nostr_metric_histogram_get("go_chan_recv_wakeup_to_progress_ns");
        atomic_store_explicit(&h_recv_wakeup_to_progress_ns, p, memory_order_release);
    }
}

#if NOSTR_CHANNEL_DISABLE_METRICS
#undef ensure_histos
#define ensure_histos() ((void)0)
#undef nostr_metric_counter_add
#define nostr_metric_counter_add(name, delta) ((void)0)
#undef nostr_metric_timer_start
#define nostr_metric_timer_start(tp) ((void)0)
#undef nostr_metric_timer_stop
#define nostr_metric_timer_stop(tp, h) ((void)0)
#endif
#if !NOSTR_CHANNEL_MPMC_SLOTS
/* Single-lock ring only: these wrap the indices, which would corrupt the
 * monotonic MPMC tickets (nostrc-75rv); MPMC paths use mpmc_push/mpmc_pop. */
static inline void go_channel_inc_in(GoChannel *chan) {
    size_t cur = atomic_load_explicit(&chan->in, memory_order_relaxed);
    size_t i = (cur + 1) & chan->mask;
    atomic_store_explicit(&chan->in, i, memory_order_relaxed);
}

static inline void go_channel_inc_out(GoChannel *chan) {
    size_t cur = atomic_load_explicit(&chan->out, memory_order_relaxed);
    size_t o = (cur + 1) & chan->mask;
    atomic_store_explicit(&chan->out, o, memory_order_relaxed);
}
#endif

typedef struct { GoChannel *c; GoContext *ctx; } channel_wait_arg_t;

#if NOSTR_CHANNEL_DERIVE_SIZE
static inline size_t go_channel_occupancy(const GoChannel *c) {
#if NOSTR_CHANNEL_MPMC_SLOTS
    // In MPMC mode, in/out are absolute monotonic counters. Occupancy is difference.
    // Load out first: out never passes in, so a later in cannot be behind it
    // (loading in first let a concurrent receive underflow this to ~SIZE_MAX).
    size_t out = atomic_load_explicit(&((GoChannel*)c)->out, memory_order_acquire);
    size_t in = atomic_load_explicit(&((GoChannel*)c)->in, memory_order_acquire);
    return in - out;
#else
    size_t in = c->in, out = c->out;
    if (in >= out) return in - out; // 0..capacity-1
    return c->capacity - (out - in);
#endif
}
static inline int go_channel_is_full(const GoChannel *c) {
#if NOSTR_CHANNEL_MPMC_SLOTS
    // Full when occupancy reaches capacity
    return go_channel_occupancy(c) >= c->capacity;
#else
    // One-empty-slot discipline: full if next_in would equal out
    size_t next_in = (c->in + 1) & c->mask;
    return next_in == c->out;
#endif
}
#endif

// Runtime-tunable spin settings (read once from env)
static int g_spin_iters = NOSTR_SPIN_ITERS;
static int g_spin_us = NOSTR_SPIN_US;
static int g_chan_debug = 0; // enable extra logging if env set

/* fp-ieg8: pthread_once, not a plain "if (!inited)" guard.
 *
 * This is on the path of every go_channel_is_closed / select try-pass, so the
 * moment a process has two channel-using threads -- which is every pool, and now
 * also the pool's redial worker -- two threads read and write g_spin_inited,
 * g_spin_iters, g_spin_us and g_chan_debug with no synchronisation at all.
 * ThreadSanitizer reports it, and it is a genuine (if benign-looking) race:
 * nothing orders the flag's write after the values it is supposed to publish.
 * pthread_once gives exactly the once-and-happens-before semantics intended,
 * for the same cost after the first call. */
static pthread_once_t g_spin_env_once = PTHREAD_ONCE_INIT;

static void spin_env_init(void) {
    const char *e1 = getenv("NOSTR_SPIN_ITERS");
    const char *e2 = getenv("NOSTR_SPIN_US");
    const char *ed = getenv("NOSTR_CHAN_DEBUG");
    if (e1) {
        long v = strtol(e1, NULL, 10);
        if (v > 0 && v < 100000) g_spin_iters = (int)v;
    }
    if (e2) {
        long v = strtol(e2, NULL, 10);
        if (v >= 0 && v < 1000000) g_spin_us = (int)v;
    }
    if (ed && *ed && *ed != '0') {
        g_chan_debug = 1;
    }
}

static inline void ensure_spin_env(void) {
    (void)pthread_once(&g_spin_env_once, spin_env_init);
}

// Next index helpers (without mutating channel state)
static inline __attribute__((unused)) size_t go_channel_next_in_idx(const GoChannel *chan) {
    size_t i = chan->in + 1;
    return i & chan->mask;
}
static inline __attribute__((unused)) size_t go_channel_next_out_idx(const GoChannel *chan) {
    size_t o = chan->out + 1;
    return o & chan->mask;
}

/* channel_send_pred removed: predicates must not read atomic state. */

int go_channel_is_closed(GoChannel *chan) {
    if (NOSTR_UNLIKELY(chan == NULL)) {
        return 1; /* Treat NULL channel as closed */
    }
    int closed = 0;
    ensure_spin_env();
    NLOCK(&chan->mutex);
    closed = atomic_load_explicit(&chan->closed, memory_order_acquire);
    NUNLOCK(&chan->mutex);
    return closed;
}

/* channel_recv_pred removed: predicates must not read atomic state. */

/* Condition function to check if the channel has space */
int go_channel_has_space(const void *chan) {
    GoChannel *c = (GoChannel *)chan;
    return (
#if NOSTR_CHANNEL_DERIVE_SIZE
        !go_channel_is_full(c)
#else
        c->size < c->capacity
#endif
    );
}

#if NOSTR_CHANNEL_MPMC_SLOTS
/* nostrc-75rv: the per-slot sequence protocol, shared by EVERY MPMC path.
 *
 * `in` and `out` are monotonic tickets.  Slot (t & mask) is free for ticket t
 * when slot_seq == t and holds ticket t's value when slot_seq == t + 1.  A
 * sender claims ticket t by advancing `in`, then stores the value and
 * publishes slot_seq = t + 1; a receiver claims by advancing `out`, takes the
 * value and releases the slot with slot_seq = t + ring, ring = mask + 1.
 * The physical ring has at least two slots (go_channel_create): with one,
 * "t published" == "free for t + 1" and a sender could overwrite a claimed
 * element.  The logical capacity (in - out <= capacity <= ring) is enforced
 * separately.  Between claim
 * and publish (or release) the slot is BUSY: `in - out` already counts the
 * element, but it cannot be read yet (or its slot cannot be reused yet).
 *
 * The lock-free try paths never take chan->mutex for the transfer, so the
 * blocking paths (which hold it only to wait and signal) must follow the same
 * protocol.  They used not to: after 64 spins on a BUSY slot the try paths
 * fell back to the mutex path, which read buffer[out] without checking
 * slot_seq and masked the monotonic tickets.  A receiver racing a sender that
 * was preempted between claim and publish "received" NULL and skipped the
 * element for good, and once tickets passed the capacity the masking
 * corrupted in - out so the channel reported itself full forever.  Under
 * load that lost subscription EVENTs and relay frames (nostrc-75rv).
 *
 * Ticket differences are compared signed: a stale `in` loaded before a newer
 * `out` must read as "retry", not as a huge unsigned occupancy. */
typedef enum {
    MPMC_OK = 0,     /* transferred */
    MPMC_NONE = -1,  /* push: full; pop: empty */
    MPMC_BUSY = 1,   /* the slot is claimed by a peer that has not finished */
} MpmcResult;

static inline MpmcResult mpmc_push(GoChannel *chan, void *data, size_t *occ_before) {
    for (;;) {
        size_t head = atomic_load_explicit(&chan->in, memory_order_acquire);
        size_t tail = atomic_load_explicit(&chan->out, memory_order_acquire);
        if ((intptr_t)(head - tail) >= (intptr_t)chan->capacity)
            return MPMC_NONE;
        size_t idx = head & chan->mask;
        size_t seq = atomic_load_explicit(&chan->slot_seq[idx], memory_order_acquire);
        intptr_t diff = (intptr_t)(seq - head);
        if (diff == 0) {
            size_t expected = head;
            if (atomic_compare_exchange_weak_explicit(&chan->in, &expected, head + 1,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                atomic_store_explicit(&chan->buffer[idx], data, memory_order_release);
                atomic_store_explicit(&chan->slot_seq[idx], head + 1, memory_order_release);
                if (occ_before)
                    *occ_before = ((intptr_t)(head - tail) > 0) ? head - tail : 0;
                return MPMC_OK;
            }
        } else if (diff < 0) {
            /* The previous lap's element in this slot has been claimed by a
             * receiver (out moved past it) that has not released it yet. */
            return MPMC_BUSY;
        }
        /* Another sender took this ticket, or head was stale: reload. */
    }
}

static inline MpmcResult mpmc_pop(GoChannel *chan, void **data, size_t *occ_before) {
    for (;;) {
        size_t tail = atomic_load_explicit(&chan->out, memory_order_acquire);
        size_t head = atomic_load_explicit(&chan->in, memory_order_acquire);
        size_t idx = tail & chan->mask;
        size_t seq = atomic_load_explicit(&chan->slot_seq[idx], memory_order_acquire);
        intptr_t diff = (intptr_t)(seq - (tail + 1));
        if (diff == 0) {
            size_t expected = tail;
            if (atomic_compare_exchange_weak_explicit(&chan->out, &expected, tail + 1,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                void *value = atomic_load_explicit(&chan->buffer[idx], memory_order_acquire);
                /* nostrc-nft: clear the slot so no stale pointer survives. */
                atomic_store_explicit(&chan->buffer[idx], NULL, memory_order_relaxed);
                atomic_store_explicit(&chan->slot_seq[idx], tail + chan->mask + 1,
                                      memory_order_release);
                if (data) *data = value;
                if (occ_before)
                    *occ_before = ((intptr_t)(head - tail) > 0) ? head - tail : 0;
                return MPMC_OK;
            }
        } else if (diff < 0) {
            /* Ticket `tail` is not published.  Re-read `in`: equal means
             * empty; ahead means a sender has claimed it and is storing. */
            head = atomic_load_explicit(&chan->in, memory_order_acquire);
            return ((intptr_t)(head - tail) > 0) ? MPMC_BUSY : MPMC_NONE;
        }
        /* Another receiver took this ticket, or tail was stale: reload. */
    }
}

/* Bounded spin for the try paths: a BUSY peer is normally two stores away. */
#define MPMC_TRY_SPINS 64

/* Longest a blocking send/receive sleeps on a BUSY slot before re-checking. */
#define MPMC_BUSY_WAIT_NS 1000000ull /* 1 ms */

/* A blocking send/receive found the slot it needs BUSY even after spinning:
 * its peer was descheduled between claiming and publishing (or releasing).
 * Called with chan->mutex held; returns with it held.  Sleep on the
 * condition the peer signals once it finishes -- every transfer signals
 * cond_empty (after a publish) or cond_full (after a release) under
 * chan->mutex -- with a short deadline as insurance when that signal is
 * conditional (REFINED_SIGNALING=OFF, nostrc-v624).  A fiber parks instead
 * of blocking its worker thread.  This replaced an unbounded sched_yield
 * spin that burned a core and never parked (nostrc-75rv review F3). */
static void mpmc_wait_busy(GoChannel *chan, nsync_cv *cv, GoFiberWaiter **fiber_waiters) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t deadline_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec +
                           MPMC_BUSY_WAIT_NS;
    gof_fiber_handle fiber = gof_hook_current();
    if (fiber) {
        GoFiberWaiter fw = { .fiber = fiber, .next = NULL };
        fiber_waiter_enqueue(fiber_waiters, &fw);
        NUNLOCK(&chan->mutex);
        gof_hook_block_current_until(deadline_ns);
        NLOCK(&chan->mutex);
        fiber_waiter_remove(fiber_waiters, &fw);
    } else {
        struct timespec deadline = { .tv_sec = (time_t)(deadline_ns / 1000000000ull),
                                     .tv_nsec = (long)(deadline_ns % 1000000000ull) };
        (void)CV_WAIT_DEADLINE_OS(cv, &chan->mutex, deadline, NULL);
    }
}
#endif /* NOSTR_CHANNEL_MPMC_SLOTS */

/* Non-blocking send: returns 0 on success, -1 if full or closed */
int __attribute__((hot)) go_channel_try_send(GoChannel *chan, void *data) {
    if (NOSTR_UNLIKELY(chan == NULL)) {
        nostr_metric_counter_add("go_chan_try_send_failures", 1);
        return -1;
    }
    /* nostrc-waiter-count: Increment BEFORE magic check to prevent race with graveyard. */
    atomic_fetch_add_explicit(&chan->active_waiters, 1, memory_order_acq_rel);
    // Validate magic number to detect garbage/freed channel pointers
    if (NOSTR_UNLIKELY(chan->magic != GO_CHANNEL_MAGIC)) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_counter_add("go_chan_invalid_magic_send", 1);
        return -1;
    }
    if (atomic_load_explicit(&chan->refs, memory_order_acquire) <= 0) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_counter_add("go_chan_try_send_failures", 1);
        return -1;
    }
#if NOSTR_CHANNEL_MPMC_SLOTS
    // Lock-free MPMC try send (per-slot sequence protocol, see mpmc_push).
    if (atomic_load_explicit(&chan->closed, memory_order_acquire) ||
        NOSTR_UNLIKELY(chan->buffer == NULL)) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_counter_add("go_chan_try_send_failures", 1);
        return -1;
    }
    {
        size_t occ_before = 0;
        MpmcResult pr;
        int spins = 0;
        while ((pr = mpmc_push(chan, data, &occ_before)) == MPMC_BUSY &&
               spins++ < MPMC_TRY_SPINS) {
            NOSTR_CPU_RELAX();
        }
        if (pr != MPMC_OK) {
            /* Full, or the slot is still being released by a receiver: the
             * ring has no free slot right now.  Never fall back to a path
             * that ignores slot_seq (nostrc-75rv). */
            atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
            nostr_metric_counter_add("go_chan_try_send_failures", 1);
            return -1;
        }
        nostr_metric_counter_add("go_chan_send_successes", 1);
        nostr_metric_counter_add("go_chan_send_depth_samples", 1);
        nostr_metric_counter_add("go_chan_send_depth_sum", occ_before + 1);
        // Wake a receiver that may be blocked waiting for data.
        // With REFINED_SIGNALING, we must always signal because only one
        // waiter wakes per signal, leaving others blocked even when data exists.
        int was_empty = (occ_before == 0);
        NLOCK(&chan->mutex);
#if NOSTR_REFINED_SIGNALING
        CV_SIGNAL_EMPTY(chan);
#else
        if (was_empty) {
            CV_BROADCAST_EMPTY(chan);
        }
#endif
        go_channel_signal_select_waiters(chan);
        NUNLOCK(&chan->mutex);
#ifdef NOSTR_ARM_WFE
        NOSTR_EVENT_SEND();
#endif
        if (was_empty) {
            nostr_metric_counter_add("go_chan_signal_empty", 1);
        }
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        return 0;
    }
#else
#if NOSTR_CHANNEL_ATOMIC_TRY && NOSTR_CHANNEL_DERIVE_SIZE
    // Lockless full check to avoid mutex on obvious failure
    size_t in_a = atomic_load_explicit(&chan->in, memory_order_acquire);
    size_t out_a = atomic_load_explicit(&chan->out, memory_order_acquire);
    size_t next_in_a = (in_a + 1) & chan->mask;
    if (next_in_a == out_a) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_counter_add("go_chan_try_send_failures", 1);
        return -1;
    }
#endif
    int rc = -1;
    NLOCK(&chan->mutex);
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        NUNLOCK(&chan->mutex);
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_counter_add("go_chan_try_send_failures", 1);
        return -1;
    }
    if (NOSTR_LIKELY(!chan->closed)
#if NOSTR_CHANNEL_DERIVE_SIZE
        && NOSTR_LIKELY(!go_channel_is_full(chan))
#else
        && NOSTR_LIKELY(chan->size < chan->capacity)
#endif
    ) {
#if NOSTR_CHANNEL_DERIVE_SIZE
        size_t occ = go_channel_occupancy(chan);
        int was_empty = (occ == 0);
#else
        int was_empty = (chan->size == 0);
#endif
        // Prefetch current store and a few producer slots to reduce misses
        // Always mask the index: in MPMC mode in is a monotonic counter
        size_t in_idx = chan->in & chan->mask;
        __builtin_prefetch(&chan->buffer[in_idx], 1, 1);
        for (int d = 1; d <= NOSTR_PREFETCH_DISTANCE; ++d) {
            size_t idx = (chan->in + (size_t)d) & chan->mask;
            __builtin_prefetch(&chan->buffer[idx], 1, 1);
        }
        {
            _Atomic(void*) *p = (_Atomic(void*)*)&chan->buffer[in_idx];
            atomic_store_explicit(p, data, memory_order_release);
        }
        go_channel_inc_in(chan);
#if !NOSTR_CHANNEL_DERIVE_SIZE
        /* The size-counted ring gates on `size`: the try paths must keep it
         * in step with the blocking paths, or try_send never sees the ring
         * full (overwriting it) and blocking receivers wait on size == 0
         * with data queued (a ticker produced no ticks). */
        chan->size++;
#endif
        // success + depth sample (post-increment size)
        nostr_metric_counter_add("go_chan_send_successes", 1);
        nostr_metric_counter_add("go_chan_send_depth_samples", 1);
        {
#if NOSTR_CHANNEL_DERIVE_SIZE
            size_t occ2 = occ + 1; // post-increment occupancy
            nostr_metric_counter_add("go_chan_send_depth_sum", occ2);
#else
            nostr_metric_counter_add("go_chan_send_depth_sum", chan->size);
#endif
        }
        // Signal receiver(s) that data is available.
        // With REFINED_SIGNALING, always signal because only one waiter
        // wakes per signal, leaving others blocked even when data exists.
#if NOSTR_REFINED_SIGNALING
        CV_SIGNAL_EMPTY(chan);
#else
        if (was_empty) {
            CV_BROADCAST_EMPTY(chan);
        }
#endif
        go_channel_signal_select_waiters(chan);
        // On ARM with WFE/SEV, send event to nudge sleeping peers
#ifdef NOSTR_ARM_WFE
        NOSTR_EVENT_SEND();
#endif
        if (was_empty) {
            nostr_metric_counter_add("go_chan_signal_empty", 1);
        }
        rc = 0;
    }
    NUNLOCK(&chan->mutex);
    /* nostrc-waiter-order: Decrement active_waiters AFTER unlocking mutex.
     * If we decrement before unlock, graveyard reap could see waiters==0
     * and free the channel while we're still about to call NUNLOCK. */
    atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
    if (NOSTR_UNLIKELY(rc != 0)) {
        nostr_metric_counter_add("go_chan_try_send_failures", 1);
    }
    return rc;
#endif /* NOSTR_CHANNEL_MPMC_SLOTS */
}

/* Non-blocking receive: returns 0 on success, -1 if empty (or closed and empty) */
int __attribute__((hot)) go_channel_try_receive(GoChannel *chan, void **data) {
    if (NOSTR_UNLIKELY(chan == NULL)) {
        nostr_metric_counter_add("go_chan_try_recv_failures", 1);
        return -1;
    }
    /* nostrc-waiter-count: Increment BEFORE magic check to prevent race with graveyard. */
    atomic_fetch_add_explicit(&chan->active_waiters, 1, memory_order_acq_rel);
    // Validate magic number to detect garbage/freed channel pointers
    if (NOSTR_UNLIKELY(chan->magic != GO_CHANNEL_MAGIC)) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_counter_add("go_chan_invalid_magic_recv", 1);
        return -1;
    }
    // Early buffer NULL check to catch channels being freed concurrently
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_counter_add("go_chan_try_recv_failures", 1);
        return -1;
    }
    if (atomic_load_explicit(&chan->refs, memory_order_acquire) <= 0) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_counter_add("go_chan_try_recv_failures", 1);
        return -1;
    }
#if NOSTR_CHANNEL_MPMC_SLOTS
    // Lock-free MPMC try receive (per-slot sequence protocol, see mpmc_pop).
    {
        size_t occ_before = 0;
        void *tmp = NULL;
        MpmcResult pr;
        int spins = 0;
        while ((pr = mpmc_pop(chan, &tmp, &occ_before)) == MPMC_BUSY &&
               spins++ < MPMC_TRY_SPINS) {
            NOSTR_CPU_RELAX();
        }
        if (pr != MPMC_OK) {
            /* Empty, or the head element is claimed by a sender that has not
             * published it yet.  Report "nothing yet": that sender signals
             * the channel (select waiters, cond_empty) after it publishes,
             * so no waiter misses it.  Consuming the unpublished slot here
             * returned NULL and lost the element (nostrc-75rv). */
            atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
            nostr_metric_counter_add("go_chan_try_recv_failures", 1);
            return -1;
        }
        if (data) *data = tmp;
        nostr_metric_counter_add("go_chan_recv_successes", 1);
        nostr_metric_counter_add("go_chan_recv_depth_samples", 1);
        nostr_metric_counter_add("go_chan_recv_depth_sum", occ_before ? occ_before - 1 : 0);
        // Wake a sender that may be blocked waiting for space.
        // With REFINED_SIGNALING, we must always signal because only one
        // waiter wakes per signal, leaving others blocked even when space exists.
        int was_full = (occ_before >= chan->capacity);
        // Signal under mutex to avoid lost wakeups
        NLOCK(&chan->mutex);
#if NOSTR_REFINED_SIGNALING
        CV_SIGNAL_FULL(chan);
#else
        if (was_full) {
            CV_BROADCAST_FULL(chan);
        }
#endif
        go_channel_signal_select_waiters(chan);
        NUNLOCK(&chan->mutex);
#ifdef NOSTR_ARM_WFE
        NOSTR_EVENT_SEND();
#endif
        if (was_full) {
            nostr_metric_counter_add("go_chan_signal_full", 1);
        }
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        return 0;
    }
#else
#if NOSTR_CHANNEL_ATOMIC_TRY && NOSTR_CHANNEL_DERIVE_SIZE
    // Lockless empty check to avoid mutex on obvious failure
    size_t in_a = atomic_load_explicit(&chan->in, memory_order_acquire);
    size_t out_a = atomic_load_explicit(&chan->out, memory_order_acquire);
    if (in_a == out_a) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_counter_add("go_chan_try_recv_failures", 1);
        return -1;
    }
#endif
    // Check freed flag before taking mutex to avoid use-after-free
    if (atomic_load_explicit(&chan->refs, memory_order_acquire) <= 0) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_counter_add("go_chan_try_recv_failures", 1);
        return -1;
    }
    int rc = -1;
    NLOCK(&chan->mutex);
    // Re-check freed flag under mutex in case of race
    if (NOSTR_UNLIKELY(atomic_load_explicit(&chan->refs, memory_order_acquire) <= 0)) {
        NUNLOCK(&chan->mutex);
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_counter_add("go_chan_try_recv_failures", 1);
        return -1;
    }
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        NUNLOCK(&chan->mutex);
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_counter_add("go_chan_try_recv_failures", 1);
        return -1;
    }
    if (
#if NOSTR_CHANNEL_DERIVE_SIZE
        NOSTR_LIKELY(go_channel_occupancy(chan) > 0)
#else
        NOSTR_LIKELY(chan->size > 0)
#endif
    ) {
#if NOSTR_CHANNEL_DERIVE_SIZE
        int was_full = go_channel_is_full(chan);
#else
        int was_full = (chan->size == chan->capacity);
#endif
        // Prefetch current load and a few consumer slots
        // Always mask the index: in MPMC mode out is a monotonic counter
        size_t out_idx = chan->out & chan->mask;
        __builtin_prefetch(&chan->buffer[out_idx], 0, 1);
        for (int d = 1; d <= NOSTR_PREFETCH_DISTANCE; ++d) {
            size_t idx = (chan->out + (size_t)d) & chan->mask;
            __builtin_prefetch(&chan->buffer[idx], 0, 1);
        }
        void *tmp = chan->buffer[out_idx];
        // nostrc-nft: Clear slot after receive to prevent stale pointer reads
        chan->buffer[out_idx] = NULL;
        if (data) *data = tmp;
        go_channel_inc_out(chan);
#if !NOSTR_CHANNEL_DERIVE_SIZE
        chan->size--; /* see go_channel_try_send */
#endif
        // success + depth sample (post-decrement size)
        nostr_metric_counter_add("go_chan_recv_successes", 1);
        nostr_metric_counter_add("go_chan_recv_depth_samples", 1);
        {
#if NOSTR_CHANNEL_DERIVE_SIZE
            size_t occ2 = go_channel_occupancy(chan); // post-decrement
            nostr_metric_counter_add("go_chan_recv_depth_sum", occ2);
#else
            nostr_metric_counter_add("go_chan_recv_depth_sum", chan->size);
#endif
        }
        // Signal sender(s) that space is available.
        // With REFINED_SIGNALING, always signal because only one waiter
        // wakes per signal, leaving others blocked even when space exists.
#if NOSTR_REFINED_SIGNALING
        CV_SIGNAL_FULL(chan);
#else
        if (was_full) {
            CV_BROADCAST_FULL(chan);
        }
#endif
        go_channel_signal_select_waiters(chan);
        // On ARM with WFE/SEV, send event to nudge sleeping peers
#ifdef NOSTR_ARM_WFE
        NOSTR_EVENT_SEND();
#endif
        if (was_full) {
            nostr_metric_counter_add("go_chan_signal_full", 1);
        }
        rc = 0;
    }
    NUNLOCK(&chan->mutex);
    /* nostrc-waiter-order: Decrement active_waiters AFTER unlocking mutex.
     * If we decrement before unlock, graveyard reap could see waiters==0
     * and free the channel while we're still about to call NUNLOCK. */
    atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
    if (NOSTR_UNLIKELY(rc != 0)) {
        nostr_metric_counter_add("go_chan_try_recv_failures", 1);
    }
    return rc;
#endif /* NOSTR_CHANNEL_MPMC_SLOTS */
}

/* Condition function to check if the channel has data */
int go_channel_has_data(const void *chan) {
    GoChannel *c = (GoChannel *)chan;
    return (
#if NOSTR_CHANNEL_DERIVE_SIZE
        go_channel_occupancy(c) > 0
#else
        c->size > 0
#endif
    );
}

/* Create a new channel with the given capacity */
GoChannel *go_channel_create(size_t capacity) {
    /* Opportunistically reap dead channels from the graveyard */
    go_channel_graveyard_reap();

    GoChannel *chan = malloc(sizeof(GoChannel));
    if (!chan) return NULL;
    // Set magic number for validation
    chan->magic = GO_CHANNEL_MAGIC;
    /* libgo has no unbuffered (rendezvous) channels: capacity 0 means 1.
     * A zero-slot ring had mask SIZE_MAX over a zero-byte buffer, so every
     * access was out of bounds (nostrc-75rv review F4). */
    size_t cap = capacity ? capacity : 1;
    // Optionally round capacity up to next power of two for faster masking
#if NOSTR_CHANNEL_ENFORCE_POW2_CAP
    if (cap > 1) {
        size_t v = cap - 1;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
#if SIZE_MAX > 0xFFFFFFFFu
        v |= v >> 32;
#endif
        cap = v + 1;
    }
#endif
#if NOSTR_CHANNEL_MPMC_SLOTS
    /* The slot protocol needs a physical ring of at least two slots (see
     * mpmc_push).  With one slot, "ticket t published" and "slot free for
     * ticket t + 1" are the same sequence value, so a sender could claim the
     * slot a receiver had claimed but not yet read: the element was lost,
     * slot_seq went backwards and the channel wedged (nostrc-75rv review F1).
     * The logical capacity -- how many elements the channel holds, and so
     * how a capacity-1 wake/ticker channel coalesces -- is unchanged. */
    size_t ring = 2;
    while (ring < cap) ring <<= 1;
#else
    size_t ring = cap;
#endif
    // Align the ring buffer to cache line size to reduce cross-line traffic
    _Atomic(void*) *buf = NULL;
    size_t bytes = sizeof(void *) * ring;
    buf = (_Atomic(void*)*)go_aligned_alloc(64, bytes);
    if (buf) memset(buf, 0, bytes);  // Zero the buffer to avoid garbage pointers
    chan->buffer = buf;
    chan->capacity = cap;
    // The ring is a power of two; compute mask for fast wrap
    chan->mask = ring - 1;
    chan->size = 0; // unused when NOSTR_CHANNEL_DERIVE_SIZE enabled
    atomic_store_explicit(&chan->in, 0, memory_order_relaxed);
    atomic_store_explicit(&chan->out, 0, memory_order_relaxed);
#if NOSTR_CHANNEL_MPMC_SLOTS
    // Allocate per-slot sequence numbers (aligned to cacheline)
    _Atomic size_t *seq = NULL;
    size_t sbytes = sizeof(_Atomic size_t) * ring;
    seq = (_Atomic size_t *)go_aligned_alloc(64, sbytes);
    chan->slot_seq = seq;
    for (size_t i = 0; i < ring; ++i) {
        atomic_store_explicit(&chan->slot_seq[i], i, memory_order_relaxed);
    }
#else
    chan->slot_seq = NULL;
#endif
    atomic_store_explicit(&chan->closed, 0, memory_order_relaxed);
    nsync_mu_init(&chan->mutex);
    nsync_cv_init(&chan->cond_full);
    nsync_cv_init(&chan->cond_empty);
    atomic_store_explicit(&chan->refs, 1, memory_order_relaxed);
    atomic_store_explicit(&chan->active_waiters, 0, memory_order_relaxed);
    chan->select_waiters = NULL;
    chan->fiber_waiters_full = NULL;
    chan->fiber_waiters_empty = NULL;
    return chan;
}

/* Increment reference count (hq-e3ach).
 * Returns chan on success, NULL if channel is invalid/destroyed.
 * CRITICAL (nostrc-ref-race): Must check magic BEFORE incrementing refs.
 * Without this check, a channel being destroyed (magic cleared in unref)
 * could have its refs incremented, causing the graveyard to skip freeing
 * it while the caller uses an invalid channel. */
GoChannel *go_channel_ref(GoChannel *chan) {
    if (chan == NULL) return NULL;
    /* Check magic to reject destroyed/invalid channels */
    if (chan->magic != GO_CHANNEL_MAGIC) {
        nostr_metric_counter_add("go_chan_ref_invalid_magic", 1);
        return NULL;
    }
    atomic_fetch_add_explicit(&chan->refs, 1, memory_order_relaxed);
    return chan;
}

/* Decrement reference count; destroy when it reaches zero (hq-e3ach). */
void go_channel_unref(GoChannel *chan) {
    if (chan == NULL) return;

    // Magic number validation: detect garbage/invalid pointers
    if (chan->magic != GO_CHANNEL_MAGIC) {
        nostr_metric_counter_add("go_chan_invalid_magic_free", 1);
        return;
    }

    // Decrement refs. If previous value was 1 we are the last owner.
    int prev = atomic_fetch_sub_explicit(&chan->refs, 1, memory_order_acq_rel);
    if (prev > 1) {
        return; // Other owners remain
    }
    if (NOSTR_UNLIKELY(prev <= 0)) {
        // Double-free or over-unref guard
        nostr_metric_counter_add("go_chan_double_free_guard", 1);
        return;
    }

    // prev == 1: we are the last reference — perform actual cleanup.
    //
    // TWO-PHASE DESTRUCTION (nostrc-uaf-unref):
    //
    // Phase 1: Mark closed, wake all waiters, free internal buffers, then
    // release the mutex. Woken waiters will reacquire the mutex, see
    // closed=1 / buffer=NULL, and exit gracefully.
    //
    // Phase 2: After releasing the mutex, we must NOT immediately free the
    // GoChannel struct because woken waiters may still be trying to
    // reacquire the embedded mutex. Instead, we yield to give them time
    // to wake, observe the closed state, and release the mutex themselves.
    // This is a best-effort approach — the definitive safety comes from
    // the protocol: callers MUST ensure all users have stopped (via
    // go_channel_close + join) before the last unref.

    NLOCK(&chan->mutex);
    // Mark closed and wake all waiters to prevent further use
    atomic_store_explicit(&chan->closed, 1, memory_order_release);
    CV_BROADCAST_FULL(chan);
    CV_BROADCAST_EMPTY(chan);
    // Wake all select waiters too
    go_channel_signal_select_waiters(chan);
    // Clean up all select waiter registrations (nostrc-select-refcount).
    // This frees GoSelectWaiterNode entries and drops their refs on waiters.
    // Previously, nodes were leaked when unregister_waiter_all skipped dead
    // channels (magic=0), leaving orphaned nodes with dangling waiter pointers.
    // Must happen AFTER signaling (so waiters get woken) but BEFORE clearing
    // magic (so we're still under the mutex with a valid channel state).
    go_channel_cleanup_select_waiters(chan);
    // Clear magic to help detect use-after-free
    chan->magic = 0;
    // Free internal buffers (under mutex so concurrent access sees NULL)
    _Atomic(void*) *buf = chan->buffer;
    _Atomic size_t *seq = chan->slot_seq;
    chan->buffer = NULL;
    chan->slot_seq = NULL;
    NUNLOCK(&chan->mutex);

    // Free the internal buffers outside the mutex (no one can access them
    // since we NULLed the pointers under the mutex)
    if (buf) free(buf);
    if (seq) free(seq);

    // nostrc-waiter-count: Wait for all active waiters to exit before adding
    // to graveyard. Waiters woken by the broadcast above will see closed=1,
    // decrement active_waiters, and exit. We use a two-phase wait:
    // 1. Spin with sched_yield for quick draining
    // 2. If still waiting, use usleep to avoid busy-waiting
    for (int i = 0; i < 100; ++i) {
        int waiters = atomic_load_explicit(&chan->active_waiters, memory_order_acquire);
        if (waiters <= 0) break;
        sched_yield();
    }
    // Phase 2: If waiters still present, sleep briefly to let them finish
    for (int i = 0; i < 50; ++i) {
        int waiters = atomic_load_explicit(&chan->active_waiters, memory_order_acquire);
        if (waiters <= 0) break;
        usleep(1000);  // 1ms per iteration, max 50ms total
    }

    // Phase 2: Defer the struct free via graveyard (nostrc-deferred-free).
    //
    // CRITICAL: We cannot free(chan) here — woken waiters blocked in
    // nsync_mu_lock are still touching chan->mutex AFTER we unlocked.
    // nsync is not ASAN-instrumented, so writes to freed memory corrupt
    // glibc fastbin metadata silently (malloc_consolidate crash).
    //
    // The previous sched_yield × 3 was a timing hack that failed under
    // load. Instead, defer the free for 1 second via a graveyard list,
    // guaranteeing all waiters have long since exited.
    go_channel_graveyard_add(chan);

    // Opportunistically reap old entries (amortized cost)
    go_channel_graveyard_reap();
}

/* Free the channel resources — backward compatible wrapper (hq-e3ach). */
void go_channel_free(GoChannel *chan) {
    go_channel_unref(chan);
}

/* Send data to the channel */
int __attribute__((hot)) go_channel_send(GoChannel *chan, void *data) {
    if (NOSTR_UNLIKELY(chan == NULL)) {
        return -1;
    }
    /* nostrc-waiter-count: Increment active_waiters BEFORE acquiring mutex.
     * This ensures graveyard reap sees us even if we haven't locked yet.
     * The sequence is: increment waiters → check magic → acquire mutex.
     * If magic is invalid, we decrement and bail out. */
    atomic_fetch_add_explicit(&chan->active_waiters, 1, memory_order_acq_rel);
    /* nostrc-magic-check: Reject dead channels. If magic is cleared, the
     * channel is in the graveyard. Decrement waiters and bail out. */
    if (NOSTR_UNLIKELY(chan->magic != GO_CHANNEL_MAGIC)) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        return -1;
    }
    /* nostrc-refs-check: Reject channels with refs <= 0. This prevents a race
     * where go_channel_unref decrements refs to 0 and proceeds with cleanup
     * while we're about to acquire the mutex. */
    if (NOSTR_UNLIKELY(atomic_load_explicit(&chan->refs, memory_order_acquire) <= 0)) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        return -1;
    }
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    int have_tw = 0; // whether we started wake->progress timer
    nostr_metric_timer tw; // wake->progress timer
    ensure_histos();
    NLOCK(&chan->mutex);
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        NUNLOCK(&chan->mutex);
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_timer_stop(&t, h_send_wait_ns);
        return -1;
    }

    /* removed wa_send: no predicate waits */
#if NOSTR_CHANNEL_MPMC_SLOTS
send_again: /* BUSY/NONE retry (mpmc_push/mpmc_pop below) */
#endif
    while (
#if NOSTR_CHANNEL_DERIVE_SIZE
        NOSTR_UNLIKELY(go_channel_is_full(chan))
#else
        NOSTR_UNLIKELY(chan->size == chan->capacity)
#endif
        && NOSTR_LIKELY(!chan->closed)) {
        if (!blocked) { nostr_metric_counter_add("go_chan_block_sends", 1); blocked = 1; }
        // Short spin of micro-deadline waits to avoid long parks on short contention
        for (int i = 0; i < g_spin_iters
#if NOSTR_CHANNEL_DERIVE_SIZE
             && NOSTR_UNLIKELY(go_channel_is_full(chan))
#else
             && NOSTR_UNLIKELY(chan->size == chan->capacity)
#endif
             && NOSTR_LIKELY(!chan->closed); ++i) {
            NOSTR_CPU_RELAX();
            CV_WAIT_FIBER(chan, &chan->cond_full, &chan->mutex, &chan->fiber_waiters_full);
            // woke up
            nostr_metric_counter_add("go_chan_send_wait_wakeups", 1);
            if (
#if NOSTR_CHANNEL_DERIVE_SIZE
                NOSTR_UNLIKELY(go_channel_is_full(chan))
#else
                NOSTR_UNLIKELY(chan->size == chan->capacity)
#endif
                && NOSTR_LIKELY(!chan->closed)) {
                nostr_metric_counter_add("go_chan_send_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_send_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
        if (
#if NOSTR_CHANNEL_DERIVE_SIZE
            NOSTR_UNLIKELY(go_channel_is_full(chan))
#else
            NOSTR_UNLIKELY(chan->size == chan->capacity)
#endif
            && NOSTR_LIKELY(!chan->closed)) {
            CV_WAIT_FIBER(chan, &chan->cond_full, &chan->mutex, &chan->fiber_waiters_full);
            // woke up
            nostr_metric_counter_add("go_chan_send_wait_wakeups", 1);
            if (
#if NOSTR_CHANNEL_DERIVE_SIZE
                NOSTR_UNLIKELY(go_channel_is_full(chan))
#else
                NOSTR_UNLIKELY(chan->size == chan->capacity)
#endif
                && NOSTR_LIKELY(!chan->closed)) {
                nostr_metric_counter_add("go_chan_send_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_send_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
    }

    // Guard: channel may have been freed while we were waiting
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        NUNLOCK(&chan->mutex);
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_timer_stop(&t, h_send_wait_ns);
        return -1;
    }
    if (NOSTR_UNLIKELY(chan->closed)) {
        NUNLOCK(&chan->mutex);
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_timer_stop(&t, h_send_wait_ns);
        return -1; // Cannot send to a closed channel
    }

    // Add data to the buffer
    int was_empty = (
#if NOSTR_CHANNEL_MPMC_SLOTS
        (chan->in - chan->out) == 0
#elif NOSTR_CHANNEL_DERIVE_SIZE
        go_channel_occupancy(chan) == 0
#else
        chan->size == 0
#endif
    );
#if NOSTR_CHANNEL_MPMC_SLOTS
    /* The mutex does not exclude lock-free try_send/try_receive, so claim a
     * ticket with the slot protocol (nostrc-75rv).  NONE: lock-free senders
     * refilled the ring since the wait; BUSY: a receiver has claimed the
     * slot but not released it yet (spin briefly, then mpmc_wait_busy).
     * Either way, re-evaluate. */
    {
        MpmcResult pr;
        int spins = 0;
        while ((pr = mpmc_push(chan, data, NULL)) == MPMC_BUSY && spins++ < MPMC_TRY_SPINS)
            NOSTR_CPU_RELAX();
        if (NOSTR_UNLIKELY(pr != MPMC_OK)) {
            if (pr == MPMC_BUSY)
                mpmc_wait_busy(chan, &chan->cond_full, &chan->fiber_waiters_full);
            goto send_again;
        }
    }
#else
    // Prefetch store and a few producer slots
    __builtin_prefetch(&chan->buffer[chan->in], 1, 1);
    for (int d = 1; d <= NOSTR_PREFETCH_DISTANCE; ++d) {
        size_t idx2 = (chan->in + (size_t)d) & chan->mask;
        __builtin_prefetch(&chan->buffer[idx2], 1, 1);
    }
    {
        _Atomic(void*) *p = (_Atomic(void*)*)&chan->buffer[chan->in];
        atomic_store_explicit(p, data, memory_order_release);
    }
    go_channel_inc_in(chan);
#endif
#if !NOSTR_CHANNEL_DERIVE_SIZE
    chan->size++;
#endif
    // success + depth sample (post-increment size)
    nostr_metric_counter_add("go_chan_send_successes", 1);
    nostr_metric_counter_add("go_chan_send_depth_samples", 1);
#if NOSTR_CHANNEL_DERIVE_SIZE
    {
        size_t occ2 = go_channel_occupancy(chan); // post-increment occupancy
        nostr_metric_counter_add("go_chan_send_depth_sum", occ2);
    }
#else
    nostr_metric_counter_add("go_chan_send_depth_sum", chan->size);
#endif

    // Signal receiver(s) that data is available.
    // With REFINED_SIGNALING, always signal because only one waiter
    // wakes per signal, leaving others blocked even when data exists.
#if NOSTR_REFINED_SIGNALING
    CV_SIGNAL_EMPTY(chan);
#else
    if (was_empty) {
        CV_BROADCAST_EMPTY(chan);
    }
#endif
    go_channel_signal_select_waiters(chan);
    (void)was_empty; // suppress unused warning when REFINED_SIGNALING
    if (was_empty) {
        // On ARM with WFE/SEV, send event to nudge sleeping peers
#ifdef NOSTR_ARM_WFE
        NOSTR_EVENT_SEND();
#endif
        nostr_metric_counter_add("go_chan_signal_empty", 1);
    }

    NUNLOCK(&chan->mutex);
    atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
    nostr_metric_timer_stop(&t, h_send_wait_ns);
    if (have_tw) {
        nostr_metric_timer_stop(&tw, h_send_wakeup_to_progress_ns);
    }
    return 0;
}

/* Receive data from the channel */
int __attribute__((hot)) go_channel_receive(GoChannel *chan, void **data) {
    if (NOSTR_UNLIKELY(chan == NULL)) {
        return -1;
    }
    /* nostrc-waiter-count: Increment active_waiters BEFORE acquiring mutex. */
    atomic_fetch_add_explicit(&chan->active_waiters, 1, memory_order_acq_rel);
    /* nostrc-magic-check: Reject dead channels. */
    if (NOSTR_UNLIKELY(chan->magic != GO_CHANNEL_MAGIC)) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        return -1;
    }
    /* nostrc-refs-check: Reject channels with refs <= 0. This prevents a race
     * where go_channel_unref decrements refs to 0 and proceeds with cleanup
     * while we're about to acquire the mutex. */
    if (NOSTR_UNLIKELY(atomic_load_explicit(&chan->refs, memory_order_acquire) <= 0)) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        return -1;
    }
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    int have_tw = 0; // whether we started wake->progress timer
    nostr_metric_timer tw; // wake->progress timer
    ensure_histos();
    NLOCK(&chan->mutex);
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        NUNLOCK(&chan->mutex);
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_timer_stop(&t, h_recv_wait_ns);
        return -1;
    }

    /* removed wa_recv: no predicate waits */
#if NOSTR_CHANNEL_MPMC_SLOTS
recv_again: /* BUSY/NONE retry (mpmc_push/mpmc_pop below) */
#endif
    while ((
#if NOSTR_CHANNEL_DERIVE_SIZE
        NOSTR_UNLIKELY(go_channel_occupancy(chan) == 0)
#else
        NOSTR_UNLIKELY(chan->size == 0)
#endif
        ) && NOSTR_LIKELY(!chan->closed)) {
        if (!blocked) { nostr_metric_counter_add("go_chan_block_recvs", 1); blocked = 1; }
        // Short spin of micro-deadline waits to avoid long parks on short contention
        for (int i = 0; i < g_spin_iters
#if NOSTR_CHANNEL_DERIVE_SIZE
             && NOSTR_UNLIKELY(go_channel_occupancy(chan) == 0)
#else
             && NOSTR_UNLIKELY(chan->size == 0)
#endif
             && NOSTR_LIKELY(!chan->closed); ++i) {
            NOSTR_CPU_RELAX();
            CV_WAIT_FIBER(chan, &chan->cond_empty, &chan->mutex, &chan->fiber_waiters_empty);
            // woke up
            nostr_metric_counter_add("go_chan_recv_wait_wakeups", 1);
            if ((
#if NOSTR_CHANNEL_DERIVE_SIZE
                NOSTR_UNLIKELY(go_channel_occupancy(chan) == 0)
#else
                NOSTR_UNLIKELY(chan->size == 0)
#endif
                ) && NOSTR_LIKELY(!chan->closed)) {
                nostr_metric_counter_add("go_chan_recv_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_recv_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
        if ((
#if NOSTR_CHANNEL_DERIVE_SIZE
            NOSTR_UNLIKELY(go_channel_occupancy(chan) == 0)
#else
            NOSTR_UNLIKELY(chan->size == 0)
#endif
            ) && NOSTR_LIKELY(!chan->closed)) {
            CV_WAIT_FIBER(chan, &chan->cond_empty, &chan->mutex, &chan->fiber_waiters_empty);
            // woke up
            nostr_metric_counter_add("go_chan_recv_wait_wakeups", 1);
            if ((
#if NOSTR_CHANNEL_DERIVE_SIZE
                NOSTR_UNLIKELY(go_channel_occupancy(chan) == 0)
#else
                NOSTR_UNLIKELY(chan->size == 0)
#endif
                ) && NOSTR_LIKELY(!chan->closed)) {
                nostr_metric_counter_add("go_chan_recv_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_recv_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
    }

    {
        int closed_empty_cond;
#if NOSTR_CHANNEL_DERIVE_SIZE
        closed_empty_cond = (go_channel_occupancy(chan) == 0);
#else
        closed_empty_cond = (chan->size == 0);
#endif
        if (NOSTR_UNLIKELY(chan->closed && closed_empty_cond)) {
        size_t in_dbg = atomic_load_explicit(&chan->in, memory_order_acquire);
        size_t out_dbg = atomic_load_explicit(&chan->out, memory_order_acquire);
#if NOSTR_CHANNEL_DERIVE_SIZE
        size_t occ_dbg = go_channel_occupancy(chan);
#else
        size_t occ_dbg = chan->size;
#endif
        NUNLOCK(&chan->mutex);
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_timer_stop(&t, h_recv_wait_ns);
        nostr_metric_counter_add("go_chan_recv_closed_empty", 1);
        if (g_chan_debug) {
            fprintf(stderr, "[chan] receive: closed+empty, in=%zu out=%zu occ=%zu\n", in_dbg, out_dbg, occ_dbg);
        }
        return -1; // Channel is closed and empty
        }
    }

    // Guard: channel may have been freed while we were waiting
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        NUNLOCK(&chan->mutex);
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_timer_stop(&t, h_recv_wait_ns);
        return -1;
    }

    // Get data from the buffer
    int was_full = (
#if NOSTR_CHANNEL_MPMC_SLOTS
        (chan->in - chan->out) == chan->capacity
#elif NOSTR_CHANNEL_DERIVE_SIZE
        go_channel_is_full(chan)
#else
        chan->size == chan->capacity
#endif
    );
#if NOSTR_CHANNEL_MPMC_SLOTS
    /* A non-empty ring does not mean the head is readable: a lock-free
     * sender may have claimed it and not published it yet, and lock-free
     * receivers do not take this mutex.  Reading buffer[out] regardless
     * returned NULL and skipped the element (nostrc-75rv).  NONE: a lock-free
     * receiver took the element since the wait; BUSY: wait for the sender to
     * publish (spin briefly, then mpmc_wait_busy).  Either way, re-evaluate. */
    void *tmp = NULL;
    {
        MpmcResult pr;
        int spins = 0;
        while ((pr = mpmc_pop(chan, &tmp, NULL)) == MPMC_BUSY && spins++ < MPMC_TRY_SPINS)
            NOSTR_CPU_RELAX();
        if (NOSTR_UNLIKELY(pr != MPMC_OK)) {
            if (pr == MPMC_BUSY)
                mpmc_wait_busy(chan, &chan->cond_empty, &chan->fiber_waiters_empty);
            goto recv_again;
        }
    }
    if (data) *data = tmp;
#else
    __builtin_prefetch(&chan->buffer[chan->out], 0, 1);
    void *tmp = NULL;
    {
        _Atomic(void*) *p = (_Atomic(void*)*)&chan->buffer[chan->out];
        tmp = atomic_load_explicit(p, memory_order_acquire);
        // nostrc-nft: Clear slot after receive to prevent stale pointer reads
        atomic_store_explicit(p, NULL, memory_order_release);
    }
    if (data) *data = tmp;
    go_channel_inc_out(chan);
#endif
#if !NOSTR_CHANNEL_DERIVE_SIZE
    chan->size--;
#endif
    // success + depth sample (post-decrement size)
    nostr_metric_counter_add("go_chan_recv_successes", 1);
    nostr_metric_counter_add("go_chan_recv_depth_samples", 1);
#if NOSTR_CHANNEL_DERIVE_SIZE
    {
        size_t occ2 = go_channel_occupancy(chan); // post-decrement occupancy
        nostr_metric_counter_add("go_chan_recv_depth_sum", occ2);
    }
#else
    nostr_metric_counter_add("go_chan_recv_depth_sum", chan->size);
#endif

    // Signal sender(s) that space is available.
    // Always signal when space exists - a blocked sender may be waiting
    // even if we weren't at full capacity (due to REFINED_SIGNALING only
    // waking one waiter, leaving others blocked).
#if NOSTR_REFINED_SIGNALING
    // With refined signaling, always signal to wake one blocked sender
    CV_SIGNAL_FULL(chan);
#else
    // Broadcast only needed when transitioning from full
    if (was_full) {
        CV_BROADCAST_FULL(chan);
    }
#endif
    go_channel_signal_select_waiters(chan);
    // On ARM with WFE/SEV, send event to nudge sleeping peers
#ifdef NOSTR_ARM_WFE
    NOSTR_EVENT_SEND();
#endif
    if (was_full) {
        nostr_metric_counter_add("go_chan_signal_full", 1);
    }

    NUNLOCK(&chan->mutex);
    atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
    nostr_metric_timer_stop(&t, h_recv_wait_ns);
    if (have_tw) {
        nostr_metric_timer_stop(&tw, h_recv_wakeup_to_progress_ns);
    }
    return 0;
}

/* Send data to the channel with cancellation context */
int __attribute__((hot)) go_channel_send_with_context(GoChannel *chan, void *data, GoContext *ctx) {
    if (NOSTR_UNLIKELY(chan == NULL)) {
        return -1;
    }
    /* nostrc-waiter-count: Increment active_waiters BEFORE acquiring mutex. */
    atomic_fetch_add_explicit(&chan->active_waiters, 1, memory_order_acq_rel);
    /* nostrc-magic-check: Reject dead channels. */
    if (NOSTR_UNLIKELY(chan->magic != GO_CHANNEL_MAGIC)) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        return -1;
    }
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    int have_tw = 0; (void)have_tw;
    nostr_metric_timer tw;
    ensure_histos();
    if (atomic_load_explicit(&chan->refs, memory_order_acquire) <= 0) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        ensure_histos();
        return -1;
    }
    NLOCK(&chan->mutex);
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        NUNLOCK(&chan->mutex);
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_timer_stop(&t, h_send_wait_ns);
        return -1;
    }

    /* no predicate waits: correctness-first */
#if NOSTR_CHANNEL_MPMC_SLOTS
send_ctx_again: /* BUSY/NONE retry (mpmc_push/mpmc_pop below) */
#endif
    while (
        /* avoid preprocessor inside macro args */
        (/* full? */ (
#if NOSTR_CHANNEL_DERIVE_SIZE
            NOSTR_UNLIKELY(go_channel_is_full(chan))
#else
            NOSTR_UNLIKELY(chan->size == chan->capacity)
#endif
        )) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx))) {
        if (!blocked) { nostr_metric_counter_add("go_chan_block_sends", 1); blocked = 1; }
        // Spin-then-park: a few micro waits first
        for (int i = 0; i < g_spin_iters
#if NOSTR_CHANNEL_DERIVE_SIZE
             && NOSTR_UNLIKELY(go_channel_is_full(chan))
#else
             && NOSTR_UNLIKELY(chan->size == chan->capacity)
#endif
             && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx)); ++i) {
            NOSTR_CPU_RELAX();
            CV_WAIT_FIBER(chan, &chan->cond_full, &chan->mutex, &chan->fiber_waiters_full);
            // woke up (deadline or condition)
            nostr_metric_counter_add("go_chan_send_wait_wakeups", 1);
            if ((
#if NOSTR_CHANNEL_DERIVE_SIZE
                NOSTR_UNLIKELY(go_channel_is_full(chan))
#else
                NOSTR_UNLIKELY(chan->size == chan->capacity)
#endif
                ) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx))) {
                nostr_metric_counter_add("go_chan_send_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_send_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
        if ((
#if NOSTR_CHANNEL_DERIVE_SIZE
            NOSTR_UNLIKELY(go_channel_is_full(chan))
#else
            NOSTR_UNLIKELY(chan->size == chan->capacity)
#endif
            ) && !chan->closed && !(ctx && go_context_is_canceled(ctx))) {
            // Correctness-first: wait under mutex until space is available
            CV_WAIT_FIBER(chan, &chan->cond_full, &chan->mutex, &chan->fiber_waiters_full);
            nostr_metric_counter_add("go_chan_send_wait_wakeups", 1);
            if ((
#if NOSTR_CHANNEL_DERIVE_SIZE
                NOSTR_UNLIKELY(go_channel_is_full(chan))
#else
                NOSTR_UNLIKELY(chan->size == chan->capacity)
#endif
                ) && !chan->closed && !(ctx && go_context_is_canceled(ctx))) {
                nostr_metric_counter_add("go_chan_send_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_send_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
    }
    /* silence unused warning on toolchains where timers compile out */
    if (have_tw) { (void)0; }

    if (NOSTR_UNLIKELY(chan->closed || (ctx && go_context_is_canceled(ctx)))) {
        NUNLOCK(&chan->mutex);
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        ensure_histos();
        nostr_metric_timer_stop(&t, h_send_wait_ns);
        return -1; // Channel closed or canceled
    }

    // Add data to the buffer
    int was_empty2;
#if NOSTR_CHANNEL_DERIVE_SIZE
    was_empty2 = (go_channel_occupancy(chan) == 0);
#else
    was_empty2 = (chan->size == 0);
#endif
#if NOSTR_CHANNEL_MPMC_SLOTS
    /* Same slot protocol as go_channel_send (nostrc-75rv).  This path used
     * the masked single-lock ring increment in MPMC mode and never published
     * slot_seq, so a ring filled to capacity read as empty and every
     * lock-free receive of its elements depended on the broken fallback. */
    {
        MpmcResult pr;
        int spins = 0;
        while ((pr = mpmc_push(chan, data, NULL)) == MPMC_BUSY && spins++ < MPMC_TRY_SPINS)
            NOSTR_CPU_RELAX();
        if (NOSTR_UNLIKELY(pr != MPMC_OK)) {
            if (pr == MPMC_BUSY)
                mpmc_wait_busy(chan, &chan->cond_full, &chan->fiber_waiters_full);
            goto send_ctx_again;
        }
    }
#else
    size_t in_idx2 = chan->in & chan->mask;
    __builtin_prefetch(&chan->buffer[in_idx2], 1, 1);
    {
        _Atomic(void*) *p = (_Atomic(void*)*)&chan->buffer[in_idx2];
        atomic_store_explicit(p, data, memory_order_release);
    }
    go_channel_inc_in(chan);
#endif
#if !NOSTR_CHANNEL_DERIVE_SIZE
    chan->size++;
#endif
    // success + depth sample (post-increment size)
    nostr_metric_counter_add("go_chan_send_successes", 1);
    nostr_metric_counter_add("go_chan_send_depth_samples", 1);
#if NOSTR_CHANNEL_DERIVE_SIZE
    {
        size_t occ2s = go_channel_occupancy(chan); // post-increment occupancy
        nostr_metric_counter_add("go_chan_send_depth_sum", occ2s);
    }
#else
    nostr_metric_counter_add("go_chan_send_depth_sum", chan->size);
#endif

    // Signal receiver(s) that data is available.
    // With REFINED_SIGNALING, always signal because only one waiter
    // wakes per signal, leaving others blocked even when data exists.
#if NOSTR_REFINED_SIGNALING
    CV_SIGNAL_EMPTY(chan);
#else
    if (was_empty2) {
        CV_BROADCAST_EMPTY(chan);
    }
#endif
    go_channel_signal_select_waiters(chan);
    // On ARM with WFE/SEV, send event to nudge sleeping peers
#ifdef NOSTR_ARM_WFE
    NOSTR_EVENT_SEND();
#endif
    if (was_empty2) {
        nostr_metric_counter_add("go_chan_signal_empty", 1);
    }

    NUNLOCK(&chan->mutex);
    atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
    return 0;
}

/* Receive data from the channel with cancellation context */
int __attribute__((hot)) go_channel_receive_with_context(GoChannel *chan, void **data, GoContext *ctx) {
    if (NOSTR_UNLIKELY(chan == NULL)) {
        return -1;
    }
    /* nostrc-waiter-count: Increment active_waiters BEFORE acquiring mutex. */
    atomic_fetch_add_explicit(&chan->active_waiters, 1, memory_order_acq_rel);
    /* nostrc-magic-check: Reject dead channels. */
    if (NOSTR_UNLIKELY(chan->magic != GO_CHANNEL_MAGIC)) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        return -1;
    }
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    int have_tw = 0; (void)have_tw;
    nostr_metric_timer tw;
    ensure_histos();
    if (atomic_load_explicit(&chan->refs, memory_order_acquire) <= 0) {
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        ensure_histos();
        return -1;
    }
    NLOCK(&chan->mutex);
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        NUNLOCK(&chan->mutex);
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        nostr_metric_timer_stop(&t, h_recv_wait_ns);
        return -1;
    }

    /* no predicate waits: correctness-first */
#if NOSTR_CHANNEL_MPMC_SLOTS
recv_ctx_again: /* BUSY/NONE retry (mpmc_push/mpmc_pop below) */
#endif
    while ((
#if NOSTR_CHANNEL_DERIVE_SIZE
        NOSTR_UNLIKELY(go_channel_occupancy(chan) == 0)
#else
        NOSTR_UNLIKELY(chan->size == 0)
#endif
        ) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx))) {
        if (!blocked) { nostr_metric_counter_add("go_chan_block_recvs", 1); blocked = 1; }
        // Spin-then-park: a few micro waits first
        for (int i = 0; i < g_spin_iters
#if NOSTR_CHANNEL_DERIVE_SIZE
             && NOSTR_UNLIKELY(go_channel_occupancy(chan) == 0)
#else
             && NOSTR_UNLIKELY(chan->size == 0)
#endif
             && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx)); ++i) {
            NOSTR_CPU_RELAX();
            CV_WAIT_FIBER(chan, &chan->cond_empty, &chan->mutex, &chan->fiber_waiters_empty);
            // woke up
            nostr_metric_counter_add("go_chan_recv_wait_wakeups", 1);
            if ((
#if NOSTR_CHANNEL_DERIVE_SIZE
                NOSTR_UNLIKELY(go_channel_occupancy(chan) == 0)
#else
                NOSTR_UNLIKELY(chan->size == 0)
#endif
                ) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx))) {
                nostr_metric_counter_add("go_chan_recv_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_recv_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
        if ((
#if NOSTR_CHANNEL_DERIVE_SIZE
            NOSTR_UNLIKELY(go_channel_occupancy(chan) == 0)
#else
            NOSTR_UNLIKELY(chan->size == 0)
#endif
            ) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx))) {
            // Correctness-first: wait under mutex until data is available
            CV_WAIT_FIBER(chan, &chan->cond_empty, &chan->mutex, &chan->fiber_waiters_empty);
            nostr_metric_counter_add("go_chan_recv_wait_wakeups", 1);
            if ((
#if NOSTR_CHANNEL_DERIVE_SIZE
                NOSTR_UNLIKELY(go_channel_occupancy(chan) == 0)
#else
                NOSTR_UNLIKELY(chan->size == 0)
#endif
                ) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx))) {
                nostr_metric_counter_add("go_chan_recv_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_recv_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
    }
    /* silence unused warning on toolchains where timers compile out */
    (void)have_tw;

    {
        int is_empty;
#if NOSTR_CHANNEL_DERIVE_SIZE
        is_empty = (go_channel_occupancy(chan) == 0);
#else
        is_empty = (chan->size == 0);
#endif
        int canceled = (ctx && go_context_is_canceled(ctx));
        int closed_empty = (chan->closed && is_empty);
        if (NOSTR_UNLIKELY(closed_empty || canceled)) {
            size_t in_dbg = atomic_load_explicit(&chan->in, memory_order_acquire);
            size_t out_dbg = atomic_load_explicit(&chan->out, memory_order_acquire);
#if NOSTR_CHANNEL_DERIVE_SIZE
            size_t occ_dbg = go_channel_occupancy(chan);
#else
            size_t occ_dbg = chan->size;
#endif
            NUNLOCK(&chan->mutex);
            atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
            ensure_histos();
            nostr_metric_timer_stop(&t, h_recv_wait_ns);
            if (closed_empty) {
                nostr_metric_counter_add("go_chan_recv_closed_empty", 1);
                if (g_chan_debug) {
                    fprintf(stderr, "[chan] receive_ctx: closed+empty, in=%zu out=%zu occ=%zu\n", in_dbg, out_dbg, occ_dbg);
                }
            }
            if (canceled) {
                nostr_metric_counter_add("go_chan_recv_ctx_canceled", 1);
                if (g_chan_debug) {
                    fprintf(stderr, "[chan] receive_ctx: canceled, in=%zu out=%zu occ=%zu\n", in_dbg, out_dbg, occ_dbg);
                }
            }
            return -1; // Channel is closed and empty or canceled
        }
    }

    // Guard: channel may have been freed while we were waiting
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        NUNLOCK(&chan->mutex);
        atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
        return -1;
    }

    // Get data from the buffer
    int was_full2 = (
#if NOSTR_CHANNEL_MPMC_SLOTS
        (chan->in - chan->out) == chan->capacity
#elif NOSTR_CHANNEL_DERIVE_SIZE
        go_channel_is_full(chan)
#else
        chan->size == chan->capacity
#endif
    );
#if NOSTR_CHANNEL_MPMC_SLOTS
    /* Same slot protocol as go_channel_receive (nostrc-75rv). */
    void *tmp2 = NULL;
    {
        MpmcResult pr;
        int spins = 0;
        while ((pr = mpmc_pop(chan, &tmp2, NULL)) == MPMC_BUSY && spins++ < MPMC_TRY_SPINS)
            NOSTR_CPU_RELAX();
        if (NOSTR_UNLIKELY(pr != MPMC_OK)) {
            if (pr == MPMC_BUSY)
                mpmc_wait_busy(chan, &chan->cond_empty, &chan->fiber_waiters_empty);
            goto recv_ctx_again;
        }
    }
    if (data) *data = tmp2;
#else
    __builtin_prefetch(&chan->buffer[chan->out], 0, 1);
    void *tmp2 = NULL;
    {
        _Atomic(void*) *p2 = (_Atomic(void*)*)&chan->buffer[chan->out];
        tmp2 = atomic_load_explicit(p2, memory_order_acquire);
    }
    if (data) *data = tmp2;
    go_channel_inc_out(chan);
#if !NOSTR_CHANNEL_DERIVE_SIZE
    chan->size--;
#endif
#endif
    // success + depth sample (post-decrement size)
    nostr_metric_counter_add("go_chan_recv_successes", 1);
    nostr_metric_counter_add("go_chan_recv_depth_samples", 1);
#if NOSTR_CHANNEL_DERIVE_SIZE
    {
        size_t occ2r = go_channel_occupancy(chan); // post-decrement occupancy
        nostr_metric_counter_add("go_chan_recv_depth_sum", occ2r);
    }
#else
    nostr_metric_counter_add("go_chan_recv_depth_sum", chan->size);
#endif

    // Signal sender(s) that space is available.
    // Always signal when space exists - a blocked sender may be waiting
    // even if we weren't at full capacity (due to REFINED_SIGNALING only
    // waking one waiter, leaving others blocked).
#if NOSTR_REFINED_SIGNALING
    // With refined signaling, always signal to wake one blocked sender
    CV_SIGNAL_FULL(chan);
#else
    // Broadcast only needed when transitioning from full
    if (was_full2) {
        CV_BROADCAST_FULL(chan);
    }
#endif
    go_channel_signal_select_waiters(chan);
    // On ARM with WFE/SEV, send event to nudge sleeping peers
#ifdef NOSTR_ARM_WFE
    NOSTR_EVENT_SEND();
#endif
    if (was_full2) {
        nostr_metric_counter_add("go_chan_signal_full", 1);
    }

    NUNLOCK(&chan->mutex);
    atomic_fetch_sub_explicit(&chan->active_waiters, 1, memory_order_release);
    return 0;
}

/* Close the channel (non-destructive): mark closed and wake waiters. */
void go_channel_close(GoChannel *chan) {
    NLOCK(&chan->mutex);

    if (!atomic_load_explicit(&chan->closed, memory_order_acquire)) {
        atomic_store_explicit(&chan->closed, 1, memory_order_release); // Mark the channel as closed
        // Wake up all potential waiters so they can observe closed state
        CV_BROADCAST_FULL(chan);
        CV_BROADCAST_EMPTY(chan);
        // Wake all select waiters — they need to re-evaluate their cases
        go_channel_signal_select_waiters(chan);
        // Nudge ARM WFE sleepers
#ifdef NOSTR_ARM_WFE
        NOSTR_EVENT_SEND();
#endif
        nostr_metric_counter_add("go_chan_close_broadcasts", 1);
    }

    NUNLOCK(&chan->mutex);
}

/* Get current channel depth (number of items in buffer) */
size_t go_channel_get_depth(GoChannel *chan) {
    if (NOSTR_UNLIKELY(chan == NULL)) {
        return 0;
    }
    if (NOSTR_UNLIKELY(chan->magic != GO_CHANNEL_MAGIC)) {
        return 0;
    }
#if NOSTR_CHANNEL_MPMC_SLOTS
    // In MPMC mode, in/out are absolute monotonic counters.  out first, as in
    // go_channel_occupancy(), so a concurrent receive cannot underflow this.
    size_t out = atomic_load_explicit(&chan->out, memory_order_acquire);
    size_t in = atomic_load_explicit(&chan->in, memory_order_acquire);
    return in - out;
#elif NOSTR_CHANNEL_DERIVE_SIZE
    return go_channel_occupancy(chan);
#else
    return chan->size;
#endif
}
