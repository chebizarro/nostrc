#include "channel.h"
#include "nostr/metrics.h"
#include "context.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

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
static nostr_metric_histogram *h_send_wait_ns = NULL;
static nostr_metric_histogram *h_recv_wait_ns = NULL;
static nostr_metric_histogram *h_send_wakeup_to_progress_ns = NULL;
static nostr_metric_histogram *h_recv_wakeup_to_progress_ns = NULL;

static inline void ensure_histos(void) {
    if (!h_send_wait_ns) {
        h_send_wait_ns = nostr_metric_histogram_get("go_chan_send_wait_ns");
    }
    if (!h_recv_wait_ns) {
        h_recv_wait_ns = nostr_metric_histogram_get("go_chan_recv_wait_ns");
    }
    if (!h_send_wakeup_to_progress_ns) {
        h_send_wakeup_to_progress_ns = nostr_metric_histogram_get("go_chan_send_wakeup_to_progress_ns");
    }
    if (!h_recv_wakeup_to_progress_ns) {
        h_recv_wakeup_to_progress_ns = nostr_metric_histogram_get("go_chan_recv_wakeup_to_progress_ns");
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

typedef struct { GoChannel *c; GoContext *ctx; } channel_wait_arg_t;

#if NOSTR_CHANNEL_DERIVE_SIZE
static inline size_t go_channel_occupancy(const GoChannel *c) {
#if NOSTR_CHANNEL_MPMC_SLOTS
    // In MPMC mode, in/out are absolute monotonic counters. Occupancy is difference.
    size_t in = atomic_load_explicit(&((GoChannel*)c)->in, memory_order_acquire);
    size_t out = atomic_load_explicit(&((GoChannel*)c)->out, memory_order_acquire);
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
static int g_spin_inited = 0;
static int g_chan_debug = 0; // enable extra logging if env set
static inline void ensure_spin_env(void) {
    if (NOSTR_UNLIKELY(!g_spin_inited)) {
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
        g_spin_inited = 1;
    }
}

// Next index helpers (without mutating channel state)
static inline size_t go_channel_next_in_idx(const GoChannel *chan) {
    size_t i = chan->in + 1;
    return i & chan->mask;
}
static inline size_t go_channel_next_out_idx(const GoChannel *chan) {
    size_t o = chan->out + 1;
    return o & chan->mask;
}

static int channel_send_pred(const void *arg) {
    const channel_wait_arg_t *wa = (const channel_wait_arg_t *)arg;
    const GoChannel *c = wa->c;
    return (
        c->closed
#if NOSTR_CHANNEL_DERIVE_SIZE
        || !go_channel_is_full(c)
#else
        || (c->size < c->capacity)
#endif
        || (wa->ctx && go_context_is_canceled(wa->ctx))
    );
}

int go_channel_is_closed(GoChannel *chan) {
    int closed = 0;
    ensure_spin_env();
    nsync_mu_lock(&chan->mutex);
    closed = chan->closed;
    nsync_mu_unlock(&chan->mutex);
    return closed;
}

static int channel_recv_pred(const void *arg) {
    const channel_wait_arg_t *wa = (const channel_wait_arg_t *)arg;
    const GoChannel *c = wa->c;
    return (
        c->closed
#if NOSTR_CHANNEL_DERIVE_SIZE
        || (go_channel_occupancy(c) > 0)
#else
        || (c->size > 0)
#endif
        || (wa->ctx && go_context_is_canceled(wa->ctx))
    );
}

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

/* Non-blocking send: returns 0 on success, -1 if full or closed */
int __attribute__((hot)) go_channel_try_send(GoChannel *chan, void *data) {
#if NOSTR_CHANNEL_MPMC_SLOTS && NOSTR_CHANNEL_ATOMIC_TRY
    // Lock-free MPMC try send using per-slot sequence protocol with bounded retries.
    for (int attempts = 0; attempts < 64; ++attempts) {
        size_t head = atomic_load_explicit(&chan->in, memory_order_acquire);
        size_t tail = atomic_load_explicit(&chan->out, memory_order_acquire);
        if (head - tail >= chan->capacity || chan->closed || NOSTR_UNLIKELY(chan->buffer == NULL)) {
            nostr_metric_counter_add("go_chan_try_send_failures", 1);
            return -1;
        }
        size_t idx = head & chan->mask;
        size_t seq = atomic_load_explicit(&chan->slot_seq[idx], memory_order_acquire);
        if (seq != head) {
            // Slot not free yet, transient contention
            NOSTR_CPU_RELAX();
            continue;
        }
        size_t expected = head;
        if (!atomic_compare_exchange_strong_explicit(&chan->in, &expected, head + 1, memory_order_acq_rel, memory_order_acquire)) {
            // Lost race to another producer; retry
            NOSTR_CPU_RELAX();
            continue;
        }
        // We own the slot at idx for sequence 'head'
        chan->buffer[idx] = data;
        atomic_store_explicit(&chan->slot_seq[idx], head + 1, memory_order_release);
        nostr_metric_counter_add("go_chan_send_successes", 1);
        nostr_metric_counter_add("go_chan_send_depth_samples", 1);
        {
            size_t occ2 = (head + 1) - tail; // post-increment occupancy
            nostr_metric_counter_add("go_chan_send_depth_sum", occ2);
        }
        // Wake a receiver if transition from empty (head==tail before increment)
        if (head == tail) {
#if NOSTR_REFINED_SIGNALING
            nsync_cv_signal(&chan->cond_empty);
#else
            nsync_cv_broadcast(&chan->cond_empty);
#endif
#ifdef NOSTR_ARM_WFE
            NOSTR_EVENT_SEND();
#endif
            nostr_metric_counter_add("go_chan_signal_empty", 1);
        }
        return 0;
    }
    nostr_metric_counter_add("go_chan_try_send_failures", 1);
    return -1;
#elif NOSTR_CHANNEL_ATOMIC_TRY && NOSTR_CHANNEL_DERIVE_SIZE
    // Lockless full check to avoid mutex on obvious failure
    size_t in_a = atomic_load_explicit(&chan->in, memory_order_acquire);
    size_t out_a = atomic_load_explicit(&chan->out, memory_order_acquire);
    size_t next_in_a = (in_a + 1) & chan->mask;
    if (next_in_a == out_a) {
        nostr_metric_counter_add("go_chan_try_send_failures", 1);
        return -1;
    }
#endif
    int rc = -1;
    nsync_mu_lock(&chan->mutex);
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        nsync_mu_unlock(&chan->mutex);
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
        __builtin_prefetch(&chan->buffer[chan->in], 1, 1);
        for (int d = 1; d <= NOSTR_PREFETCH_DISTANCE; ++d) {
            size_t idx = (chan->in + (size_t)d) & chan->mask;
            __builtin_prefetch(&chan->buffer[idx], 1, 1);
        }
        chan->buffer[chan->in] = data;
        go_channel_inc_in(chan);
        // size derived: no counter update
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
        // Signal receiver(s) that data is available
        if (was_empty) {
#if NOSTR_REFINED_SIGNALING
            nsync_cv_signal(&chan->cond_empty);
#else
            nsync_cv_broadcast(&chan->cond_empty);
#endif
            // On ARM with WFE/SEV, send event to nudge sleeping peers
#ifdef NOSTR_ARM_WFE
            NOSTR_EVENT_SEND();
#endif
            nostr_metric_counter_add("go_chan_signal_empty", 1);
        }
        rc = 0;
    }
    nsync_mu_unlock(&chan->mutex);
    if (NOSTR_UNLIKELY(rc != 0)) {
        nostr_metric_counter_add("go_chan_try_send_failures", 1);
    }
    return rc;
}

/* Non-blocking receive: returns 0 on success, -1 if empty (or closed and empty) */
int __attribute__((hot)) go_channel_try_receive(GoChannel *chan, void **data) {
#if NOSTR_CHANNEL_MPMC_SLOTS && NOSTR_CHANNEL_ATOMIC_TRY
    // Lock-free MPMC try receive using per-slot sequence protocol with bounded retries.
    for (int attempts = 0; attempts < 64; ++attempts) {
        size_t tail = atomic_load_explicit(&chan->out, memory_order_acquire);
        size_t head = atomic_load_explicit(&chan->in, memory_order_acquire);
        if (head == tail || NOSTR_UNLIKELY(chan->buffer == NULL)) {
            nostr_metric_counter_add("go_chan_try_recv_failures", 1);
            return -1;
        }
        size_t idx = tail & chan->mask;
        size_t seq = atomic_load_explicit(&chan->slot_seq[idx], memory_order_acquire);
        if (seq != tail + 1) {
            // Slot not ready yet; transient
            NOSTR_CPU_RELAX();
            continue;
        }
        size_t expected = tail;
        if (!atomic_compare_exchange_strong_explicit(&chan->out, &expected, tail + 1, memory_order_acq_rel, memory_order_acquire)) {
            // Lost race to another consumer
            NOSTR_CPU_RELAX();
            continue;
        }
        void *tmp = chan->buffer[idx];
        if (data) *data = tmp;
        atomic_store_explicit(&chan->slot_seq[idx], tail + chan->capacity, memory_order_release);
        nostr_metric_counter_add("go_chan_recv_successes", 1);
        nostr_metric_counter_add("go_chan_recv_depth_samples", 1);
        {
            size_t occ2 = head - (tail + 1); // post-decrement occupancy
            nostr_metric_counter_add("go_chan_recv_depth_sum", occ2);
        }
        // Wake a sender if transition from full (head - tail == capacity before dec)
        if ((head - tail) == chan->capacity) {
#if NOSTR_REFINED_SIGNALING
            nsync_cv_signal(&chan->cond_full);
#else
            nsync_cv_broadcast(&chan->cond_full);
#endif
#ifdef NOSTR_ARM_WFE
            NOSTR_EVENT_SEND();
#endif
            nostr_metric_counter_add("go_chan_signal_full", 1);
        }
        return 0;
    }
#elif NOSTR_CHANNEL_ATOMIC_TRY && NOSTR_CHANNEL_DERIVE_SIZE
    // Lockless empty check to avoid mutex on obvious failure
    size_t in_a = atomic_load_explicit(&chan->in, memory_order_acquire);
    size_t out_a = atomic_load_explicit(&chan->out, memory_order_acquire);
    if (in_a == out_a) {
        nostr_metric_counter_add("go_chan_try_recv_failures", 1);
        return -1;
    }
#endif
    int rc = -1;
    nsync_mu_lock(&chan->mutex);
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        nsync_mu_unlock(&chan->mutex);
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
        __builtin_prefetch(&chan->buffer[chan->out], 0, 1);
        for (int d = 1; d <= NOSTR_PREFETCH_DISTANCE; ++d) {
            size_t idx = (chan->out + (size_t)d) & chan->mask;
            __builtin_prefetch(&chan->buffer[idx], 0, 1);
        }
        void *tmp = chan->buffer[chan->out];
        if (data) *data = tmp;
        go_channel_inc_out(chan);
        // size derived: no counter update
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
        // Signal sender(s) that space is available
        if (was_full) {
#if NOSTR_REFINED_SIGNALING
            nsync_cv_signal(&chan->cond_full);
#else
            nsync_cv_broadcast(&chan->cond_full);
#endif
            // On ARM with WFE/SEV, send event to nudge sleeping peers
#ifdef NOSTR_ARM_WFE
            NOSTR_EVENT_SEND();
#endif
            nostr_metric_counter_add("go_chan_signal_full", 1);
        }
        rc = 0;
    }
    nsync_mu_unlock(&chan->mutex);
    if (NOSTR_UNLIKELY(rc != 0)) {
        nostr_metric_counter_add("go_chan_try_recv_failures", 1);
    }
    return rc;
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
    GoChannel *chan = malloc(sizeof(GoChannel));
    // Optionally round capacity up to next power of two for faster masking
    size_t cap = capacity;
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
    // Align the ring buffer to cache line size to reduce cross-line traffic
    void **buf = NULL;
    size_t bytes = sizeof(void *) * cap;
    if (posix_memalign((void **)&buf, 64, bytes) != 0) {
        buf = malloc(bytes);
    }
    chan->buffer = buf;
    chan->capacity = cap;
    // Capacity is enforced to a power of two; compute mask for fast wrap
    chan->mask = chan->capacity - 1;
    chan->size = 0; // unused when NOSTR_CHANNEL_DERIVE_SIZE enabled
    atomic_store_explicit(&chan->in, 0, memory_order_relaxed);
    atomic_store_explicit(&chan->out, 0, memory_order_relaxed);
#if NOSTR_CHANNEL_MPMC_SLOTS
    // Allocate per-slot sequence numbers (aligned to cacheline)
    _Atomic size_t *seq = NULL;
    size_t sbytes = sizeof(_Atomic size_t) * cap;
    if (posix_memalign((void **)&seq, 64, sbytes) != 0) {
        seq = (_Atomic size_t *)malloc(sbytes);
    }
    chan->slot_seq = seq;
    for (size_t i = 0; i < cap; ++i) {
        atomic_store_explicit(&chan->slot_seq[i], i, memory_order_relaxed);
    }
#else
    chan->slot_seq = NULL;
#endif
    chan->closed = false;
    nsync_mu_init(&chan->mutex);
    nsync_cv_init(&chan->cond_full);
    nsync_cv_init(&chan->cond_empty);
    atomic_store_explicit(&chan->freed, 0, memory_order_relaxed);
    return chan;
}

/* Free the channel resources */
void go_channel_free(GoChannel *chan) {
    if (chan == NULL) {
        return;
    }

    // Double-free guard: if already freed, return immediately
    int was = atomic_exchange_explicit(&chan->freed, 1, memory_order_acq_rel);
    if (NOSTR_UNLIKELY(was)) {
        // Optional: emit a metric or stderr to help track erroneous frees
        nostr_metric_counter_add("go_chan_double_free_guard", 1);
        return;
    }

    nsync_mu_lock(&chan->mutex);
    if (chan->buffer) {
        free(chan->buffer);
        chan->buffer = NULL;
    }
    if (chan->slot_seq) {
        free(chan->slot_seq);
        chan->slot_seq = NULL;
    }
    nsync_mu_unlock(&chan->mutex);
    free(chan);
}

/* Send data to the channel */
int __attribute__((hot)) go_channel_send(GoChannel *chan, void *data) {
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    int have_tw = 0; // whether we started wake->progress timer
    nostr_metric_timer tw; // wake->progress timer
    ensure_histos();
    nsync_mu_lock(&chan->mutex);
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, h_send_wait_ns);
        return -1;
    }

    channel_wait_arg_t wa_send = { .c = chan, .ctx = NULL };
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
            nsync_time dl_spin = nsync_time_add(nsync_time_now(), nsync_time_us(g_spin_us));
            nsync_mu_wait_with_deadline(&chan->mutex, channel_send_pred, &wa_send, NULL, dl_spin, NULL);
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
            nsync_cv_wait(&chan->cond_full, &chan->mutex);
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

    if (NOSTR_UNLIKELY(chan->closed)) {
        nsync_mu_unlock(&chan->mutex);
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
    size_t head = atomic_load_explicit(&chan->in, memory_order_relaxed);
    size_t idx = head & chan->mask;
    __builtin_prefetch(&chan->buffer[idx], 1, 1);
    // Slot must be free under the lock
    atomic_store_explicit(&chan->slot_seq[idx], head, memory_order_relaxed); // ensure expected value
    chan->buffer[idx] = data;
    atomic_store_explicit(&chan->slot_seq[idx], head + 1, memory_order_release);
    atomic_store_explicit(&chan->in, head + 1, memory_order_release);
#else
    // Prefetch store and a few producer slots
    __builtin_prefetch(&chan->buffer[chan->in], 1, 1);
    for (int d = 1; d <= NOSTR_PREFETCH_DISTANCE; ++d) {
        size_t idx2 = (chan->in + (size_t)d) & chan->mask;
        __builtin_prefetch(&chan->buffer[idx2], 1, 1);
    }
    chan->buffer[chan->in] = data;
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

    // Signal receiver(s) that data is available
    if (was_empty) {
#if NOSTR_REFINED_SIGNALING
        nsync_cv_signal(&chan->cond_empty);
#else
        nsync_cv_broadcast(&chan->cond_empty);
#endif
        // On ARM with WFE/SEV, send event to nudge sleeping peers
#ifdef NOSTR_ARM_WFE
        NOSTR_EVENT_SEND();
#endif
        nostr_metric_counter_add("go_chan_signal_empty", 1);
    }

    nsync_mu_unlock(&chan->mutex);
    nostr_metric_timer_stop(&t, h_send_wait_ns);
    if (have_tw) {
        nostr_metric_timer_stop(&tw, h_send_wakeup_to_progress_ns);
    }
    return 0;
}

/* Receive data from the channel */
int __attribute__((hot)) go_channel_receive(GoChannel *chan, void **data) {
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    int have_tw = 0; // whether we started wake->progress timer
    nostr_metric_timer tw; // wake->progress timer
    ensure_histos();
    nsync_mu_lock(&chan->mutex);
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, h_recv_wait_ns);
        return -1;
    }

    channel_wait_arg_t wa_recv = { .c = chan, .ctx = NULL };
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
            nsync_time dl_spin = nsync_time_add(nsync_time_now(), nsync_time_us(g_spin_us));
            nsync_mu_wait_with_deadline(&chan->mutex, channel_recv_pred, &wa_recv, NULL, dl_spin, NULL);
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
            nsync_cv_wait(&chan->cond_empty, &chan->mutex);
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
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, h_recv_wait_ns);
        nostr_metric_counter_add("go_chan_recv_closed_empty", 1);
        if (g_chan_debug) {
            fprintf(stderr, "[chan] receive: closed+empty, in=%zu out=%zu occ=%zu\n", in_dbg, out_dbg, occ_dbg);
        }
        return -1; // Channel is closed and empty
        }
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
    size_t tail = atomic_load_explicit(&chan->out, memory_order_relaxed);
    size_t idx = tail & chan->mask;
    __builtin_prefetch(&chan->buffer[idx], 0, 1);
    // Slot must be ready under the lock
    void *tmp = chan->buffer[idx];
    if (data) *data = tmp;
    atomic_store_explicit(&chan->slot_seq[idx], tail + chan->capacity, memory_order_release);
    atomic_store_explicit(&chan->out, tail + 1, memory_order_release);
#else
    __builtin_prefetch(&chan->buffer[chan->out], 0, 1);
    void *tmp = chan->buffer[chan->out];
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

    // Signal sender(s) that space is available
    if (was_full) {
#if NOSTR_REFINED_SIGNALING
        nsync_cv_signal(&chan->cond_full);
#else
        nsync_cv_broadcast(&chan->cond_full);
#endif
        // On ARM with WFE/SEV, send event to nudge sleeping peers
#ifdef NOSTR_ARM_WFE
        NOSTR_EVENT_SEND();
#endif
        nostr_metric_counter_add("go_chan_signal_full", 1);
    }

    nsync_mu_unlock(&chan->mutex);
    nostr_metric_timer_stop(&t, h_recv_wait_ns);
    if (have_tw) {
        nostr_metric_timer_stop(&tw, h_recv_wakeup_to_progress_ns);
    }
    return 0;
}

/* Send data to the channel with cancellation context */
int __attribute__((hot)) go_channel_send_with_context(GoChannel *chan, void *data, GoContext *ctx) {
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    int have_tw = 0;
    nostr_metric_timer tw;
    ensure_histos();
    nsync_mu_lock(&chan->mutex);
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, h_send_wait_ns);
        return -1;
    }

    channel_wait_arg_t wa = { .c = chan, .ctx = ctx };
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
            nsync_time dl_spin = nsync_time_add(nsync_time_now(), nsync_time_us(g_spin_us));
            nsync_mu_wait_with_deadline(&chan->mutex, channel_send_pred, &wa, NULL, dl_spin, NULL);
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
            // Fallback to a longer park to avoid busy waiting when contention persists
            nsync_time dl = nsync_time_add(nsync_time_now(), nsync_time_ms(50));
            nsync_mu_wait_with_deadline(&chan->mutex, channel_send_pred, &wa, NULL, dl, NULL);
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
    (void)have_tw;

    if (NOSTR_UNLIKELY(chan->closed || (ctx && go_context_is_canceled(ctx)))) {
        nsync_mu_unlock(&chan->mutex);
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
    __builtin_prefetch(&chan->buffer[chan->in], 1, 1);
    chan->buffer[chan->in] = data;
    go_channel_inc_in(chan);
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

    // Signal receiver(s) that data is available
    if (was_empty2) {
#if NOSTR_REFINED_SIGNALING
        nsync_cv_signal(&chan->cond_empty);
#else
        nsync_cv_broadcast(&chan->cond_empty);
#endif
        // On ARM with WFE/SEV, send event to nudge sleeping peers
#ifdef NOSTR_ARM_WFE
        NOSTR_EVENT_SEND();
#endif
        nostr_metric_counter_add("go_chan_signal_empty", 1);
    }

    nsync_mu_unlock(&chan->mutex);
    return 0;
}

/* Receive data from the channel with cancellation context */
int __attribute__((hot)) go_channel_receive_with_context(GoChannel *chan, void **data, GoContext *ctx) {
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    int have_tw = 0;
    nostr_metric_timer tw;
    ensure_histos();
    nsync_mu_lock(&chan->mutex);
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, h_recv_wait_ns);
        return -1;
    }

    channel_wait_arg_t wa = { .c = chan, .ctx = ctx };
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
            nsync_time dl_spin = nsync_time_add(nsync_time_now(), nsync_time_us(g_spin_us));
            nsync_mu_wait_with_deadline(&chan->mutex, channel_recv_pred, &wa, NULL, dl_spin, NULL);
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
            // Fallback to a longer park to avoid busy waiting when contention persists
            nsync_time dl = nsync_time_add(nsync_time_now(), nsync_time_ms(50));
            nsync_mu_wait_with_deadline(&chan->mutex, channel_recv_pred, &wa, NULL, dl, NULL);
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

    {
        int closed_empty;
#if NOSTR_CHANNEL_DERIVE_SIZE
        closed_empty = (go_channel_occupancy(chan) == 0);
#else
        closed_empty = (chan->size == 0);
#endif
        int canceled = (ctx && go_context_is_canceled(ctx));
        if (NOSTR_UNLIKELY(((chan->closed && closed_empty) || canceled))) {
            int canceled = (ctx && go_context_is_canceled(ctx));
            int closed_empty = (chan->closed
#if NOSTR_CHANNEL_DERIVE_SIZE
                && go_channel_occupancy(chan) == 0
#else
                && chan->size == 0
#endif
                );
            size_t in_dbg = atomic_load_explicit(&chan->in, memory_order_acquire);
            size_t out_dbg = atomic_load_explicit(&chan->out, memory_order_acquire);
#if NOSTR_CHANNEL_DERIVE_SIZE
            size_t occ_dbg = go_channel_occupancy(chan);
#else
            size_t occ_dbg = chan->size;
#endif
            nsync_mu_unlock(&chan->mutex);
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
    size_t tail2 = atomic_load_explicit(&chan->out, memory_order_relaxed);
    size_t idx2 = tail2 & chan->mask;
    __builtin_prefetch(&chan->buffer[idx2], 0, 1);
    void *tmp2 = chan->buffer[idx2];
    if (data) *data = tmp2;
    atomic_store_explicit(&chan->slot_seq[idx2], tail2 + chan->capacity, memory_order_release);
    atomic_store_explicit(&chan->out, tail2 + 1, memory_order_release);
#else
    __builtin_prefetch(&chan->buffer[chan->out], 0, 1);
    void *tmp2 = chan->buffer[chan->out];
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

    // Signal sender(s) that space is available
    if (was_full2) {
#if NOSTR_REFINED_SIGNALING
        nsync_cv_signal(&chan->cond_full);
#else
        nsync_cv_broadcast(&chan->cond_full);
#endif
        // On ARM with WFE/SEV, send event to nudge sleeping peers
#ifdef NOSTR_ARM_WFE
        NOSTR_EVENT_SEND();
#endif
        nostr_metric_counter_add("go_chan_signal_full", 1);
    }

    nsync_mu_unlock(&chan->mutex);
    return 0;
}

/* Close the channel (non-destructive): mark closed and wake waiters. */
void go_channel_close(GoChannel *chan) {
    nsync_mu_lock(&chan->mutex);

    if (!chan->closed) {
        chan->closed = true; // Mark the channel as closed
        // Wake up all potential waiters so they can observe closed state
        nsync_cv_broadcast(&chan->cond_full);
        nsync_cv_broadcast(&chan->cond_empty);
        // Nudge ARM WFE sleepers
#ifdef NOSTR_ARM_WFE
        NOSTR_EVENT_SEND();
#endif
        nostr_metric_counter_add("go_chan_close_broadcasts", 1);
    }

    nsync_mu_unlock(&chan->mutex);
}
