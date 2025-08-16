#include "channel.h"
#include "nostr/metrics.h"
#include "context.h"
#include <stdio.h>
#include <stdlib.h>

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
# if defined(__x86_64__) || defined(__i386__)
#  define NOSTR_CPU_RELAX() __builtin_ia32_pause()
# elif defined(__aarch64__) || defined(__arm__)
#  define NOSTR_CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")
# else
#  include <sched.h>
#  define NOSTR_CPU_RELAX() sched_yield()
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
static inline void go_channel_inc_in(GoChannel *chan) {
    size_t i = chan->in + 1;
    if (NOSTR_LIKELY(chan->is_pow2)) {
        chan->in = i & chan->mask;
    } else if (NOSTR_UNLIKELY(i == chan->capacity)) {
        chan->in = 0;
    } else {
        chan->in = i;
    }
}

static inline void go_channel_inc_out(GoChannel *chan) {
    size_t o = chan->out + 1;
    if (NOSTR_LIKELY(chan->is_pow2)) {
        chan->out = o & chan->mask;
    } else if (NOSTR_UNLIKELY(o == chan->capacity)) {
        chan->out = 0;
    } else {
        chan->out = o;
    }
}

typedef struct { GoChannel *c; GoContext *ctx; } channel_wait_arg_t;

// Runtime-tunable spin settings (read once from env)
static int g_spin_iters = NOSTR_SPIN_ITERS;
static int g_spin_us = NOSTR_SPIN_US;
static int g_spin_inited = 0;
static inline void ensure_spin_env(void) {
    if (NOSTR_UNLIKELY(!g_spin_inited)) {
        const char *e1 = getenv("NOSTR_SPIN_ITERS");
        const char *e2 = getenv("NOSTR_SPIN_US");
        if (e1) {
            long v = strtol(e1, NULL, 10);
            if (v > 0 && v < 100000) g_spin_iters = (int)v;
        }
        if (e2) {
            long v = strtol(e2, NULL, 10);
            if (v >= 0 && v < 1000000) g_spin_us = (int)v;
        }
        g_spin_inited = 1;
    }
}

// Next index helpers (without mutating channel state)
static inline size_t go_channel_next_in_idx(const GoChannel *chan) {
    size_t i = chan->in + 1;
    if (NOSTR_LIKELY(chan->is_pow2)) return i & chan->mask;
    return (NOSTR_UNLIKELY(i == chan->capacity)) ? 0 : i;
}
static inline size_t go_channel_next_out_idx(const GoChannel *chan) {
    size_t o = chan->out + 1;
    if (NOSTR_LIKELY(chan->is_pow2)) return o & chan->mask;
    return (NOSTR_UNLIKELY(o == chan->capacity)) ? 0 : o;
}

static int channel_send_pred(const void *arg) {
    const channel_wait_arg_t *wa = (const channel_wait_arg_t *)arg;
    const GoChannel *c = wa->c;
    return (c->closed || (c->size < c->capacity) || (wa->ctx && go_context_is_canceled(wa->ctx)));
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
    return (c->closed || (c->size > 0) || (wa->ctx && go_context_is_canceled(wa->ctx)));
}

/* Condition function to check if the channel has space */
int go_channel_has_space(const void *chan) {
    GoChannel *c = (GoChannel *)chan;
    return c->size < c->capacity;
}

/* Non-blocking send: returns 0 on success, -1 if full or closed */
int __attribute__((hot)) go_channel_try_send(GoChannel *chan, void *data) {
    int rc = -1;
    nsync_mu_lock(&chan->mutex);
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_counter_add("go_chan_try_send_failures", 1);
        return -1;
    }
    if (NOSTR_LIKELY(!chan->closed) && NOSTR_LIKELY(chan->size < chan->capacity)) {
        int was_empty = (chan->size == 0);
        // Prefetch current store and a few producer slots to reduce misses
        __builtin_prefetch(&chan->buffer[chan->in], 1, 1);
        for (int d = 1; d <= NOSTR_PREFETCH_DISTANCE; ++d) {
            size_t idx = chan->is_pow2 ? ((chan->in + (size_t)d) & chan->mask)
                                       : ((chan->in + (size_t)d) % chan->capacity);
            __builtin_prefetch(&chan->buffer[idx], 1, 1);
        }
        chan->buffer[chan->in] = data;
        go_channel_inc_in(chan);
        chan->size++;
        // success + depth sample (post-increment size)
        nostr_metric_counter_add("go_chan_send_successes", 1);
        nostr_metric_counter_add("go_chan_send_depth_samples", 1);
        nostr_metric_counter_add("go_chan_send_depth_sum", chan->size);
        // Signal one receiver only on 0->1 transition
        if (was_empty) {
            nsync_cv_signal(&chan->cond_empty);
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
    int rc = -1;
    nsync_mu_lock(&chan->mutex);
    if (NOSTR_UNLIKELY(chan->buffer == NULL)) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_counter_add("go_chan_try_recv_failures", 1);
        return -1;
    }
    if (NOSTR_LIKELY(chan->size > 0)) {
        int was_full = (chan->size == chan->capacity);
        // Prefetch current load and a few consumer slots
        __builtin_prefetch(&chan->buffer[chan->out], 0, 1);
        for (int d = 1; d <= NOSTR_PREFETCH_DISTANCE; ++d) {
            size_t idx = chan->is_pow2 ? ((chan->out + (size_t)d) & chan->mask)
                                       : ((chan->out + (size_t)d) % chan->capacity);
            __builtin_prefetch(&chan->buffer[idx], 0, 1);
        }
        void *tmp = chan->buffer[chan->out];
        if (data) *data = tmp;
        go_channel_inc_out(chan);
        chan->size--;
        // success + depth sample (post-decrement size)
        nostr_metric_counter_add("go_chan_recv_successes", 1);
        nostr_metric_counter_add("go_chan_recv_depth_samples", 1);
        nostr_metric_counter_add("go_chan_recv_depth_sum", chan->size);
        // Signal one sender only on full->not-full transition
        if (was_full) {
            nsync_cv_signal(&chan->cond_full);
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
    return c->size > 0;
}

/* Create a new channel with the given capacity */
GoChannel *go_channel_create(size_t capacity) {
    GoChannel *chan = malloc(sizeof(GoChannel));
    // Align the ring buffer to cache line size to reduce cross-line traffic
    void **buf = NULL;
    size_t bytes = sizeof(void *) * capacity;
    if (posix_memalign((void **)&buf, 64, bytes) != 0) {
        buf = malloc(bytes);
    }
    chan->buffer = buf;
    chan->capacity = capacity;
    // If capacity is a power of two, we use mask for fast wrap
    chan->is_pow2 = (capacity != 0) && ((capacity & (capacity - 1)) == 0);
    chan->mask = chan->is_pow2 ? (capacity - 1) : 0;
    chan->size = 0;
    chan->in = 0;
    chan->out = 0;
    chan->closed = false;
    nsync_mu_init(&chan->mutex);
    nsync_cv_init(&chan->cond_full);
    nsync_cv_init(&chan->cond_empty);
    return chan;
}

/* Free the channel resources */
void go_channel_free(GoChannel *chan) {
    if (chan == NULL) {
        return;
    }

    nsync_mu_lock(&chan->mutex);
    if (chan->buffer) {
        free(chan->buffer);
        chan->buffer = NULL;
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
    while (NOSTR_UNLIKELY(chan->size == chan->capacity) && NOSTR_LIKELY(!chan->closed)) {
        if (!blocked) { nostr_metric_counter_add("go_chan_block_sends", 1); blocked = 1; }
        // Short spin of micro-deadline waits to avoid long parks on short contention
        for (int i = 0; i < g_spin_iters && NOSTR_UNLIKELY(chan->size == chan->capacity) && NOSTR_LIKELY(!chan->closed); ++i) {
            NOSTR_CPU_RELAX();
            nsync_time dl_spin = nsync_time_add(nsync_time_now(), nsync_time_us(g_spin_us));
            nsync_mu_wait_with_deadline(&chan->mutex, channel_send_pred, &wa_send, NULL, dl_spin, NULL);
            // woke up
            nostr_metric_counter_add("go_chan_send_wait_wakeups", 1);
            if (NOSTR_UNLIKELY(chan->size == chan->capacity) && NOSTR_LIKELY(!chan->closed)) {
                nostr_metric_counter_add("go_chan_send_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_send_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
        if (NOSTR_UNLIKELY(chan->size == chan->capacity) && NOSTR_LIKELY(!chan->closed)) {
            nsync_cv_wait(&chan->cond_full, &chan->mutex);
            // woke up
            nostr_metric_counter_add("go_chan_send_wait_wakeups", 1);
            if (NOSTR_UNLIKELY(chan->size == chan->capacity) && NOSTR_LIKELY(!chan->closed)) {
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
    int was_empty = (chan->size == 0);
    // Prefetch store and a few producer slots
    __builtin_prefetch(&chan->buffer[chan->in], 1, 1);
    for (int d = 1; d <= NOSTR_PREFETCH_DISTANCE; ++d) {
        size_t idx = chan->is_pow2 ? ((chan->in + (size_t)d) & chan->mask)
                                   : ((chan->in + (size_t)d) % chan->capacity);
        __builtin_prefetch(&chan->buffer[idx], 1, 1);
    }
    chan->buffer[chan->in] = data;
    go_channel_inc_in(chan);
    chan->size++;
    // success + depth sample (post-increment size)
    nostr_metric_counter_add("go_chan_send_successes", 1);
    nostr_metric_counter_add("go_chan_send_depth_samples", 1);
    nostr_metric_counter_add("go_chan_send_depth_sum", chan->size);

    // Signal one receiver that data is available only on 0->1
    if (was_empty) {
        nsync_cv_signal(&chan->cond_empty);
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
    while (NOSTR_UNLIKELY(chan->size == 0) && NOSTR_LIKELY(!chan->closed)) {
        if (!blocked) { nostr_metric_counter_add("go_chan_block_recvs", 1); blocked = 1; }
        // Short spin of micro-deadline waits to avoid long parks on short contention
        for (int i = 0; i < g_spin_iters && NOSTR_UNLIKELY(chan->size == 0) && NOSTR_LIKELY(!chan->closed); ++i) {
            NOSTR_CPU_RELAX();
            nsync_time dl_spin = nsync_time_add(nsync_time_now(), nsync_time_us(g_spin_us));
            nsync_mu_wait_with_deadline(&chan->mutex, channel_recv_pred, &wa_recv, NULL, dl_spin, NULL);
            // woke up
            nostr_metric_counter_add("go_chan_recv_wait_wakeups", 1);
            if (NOSTR_UNLIKELY(chan->size == 0) && NOSTR_LIKELY(!chan->closed)) {
                nostr_metric_counter_add("go_chan_recv_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_recv_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
        if (NOSTR_UNLIKELY(chan->size == 0) && NOSTR_LIKELY(!chan->closed)) {
            nsync_cv_wait(&chan->cond_empty, &chan->mutex);
            // woke up
            nostr_metric_counter_add("go_chan_recv_wait_wakeups", 1);
            if (NOSTR_UNLIKELY(chan->size == 0) && NOSTR_LIKELY(!chan->closed)) {
                nostr_metric_counter_add("go_chan_recv_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_recv_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
    }

    if (NOSTR_UNLIKELY(chan->closed && chan->size == 0)) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, h_recv_wait_ns);
        return -1; // Channel is closed and empty
    }

    // Get data from the buffer
    int was_full = (chan->size == chan->capacity);
    __builtin_prefetch(&chan->buffer[chan->out], 0, 1);
    void *tmp = chan->buffer[chan->out];
    if (data) *data = tmp;
    go_channel_inc_out(chan);
    chan->size--;
    // success + depth sample (post-decrement size)
    nostr_metric_counter_add("go_chan_recv_successes", 1);
    nostr_metric_counter_add("go_chan_recv_depth_samples", 1);
    nostr_metric_counter_add("go_chan_recv_depth_sum", chan->size);

    // Signal one sender that space is available only on full->not-full
    if (was_full) {
        nsync_cv_signal(&chan->cond_full);
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
    while (NOSTR_UNLIKELY(chan->size == chan->capacity) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx))) {
        if (!blocked) { nostr_metric_counter_add("go_chan_block_sends", 1); blocked = 1; }
        // Spin-then-park: a few micro waits first
        for (int i = 0; i < g_spin_iters && NOSTR_UNLIKELY(chan->size == chan->capacity) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx)); ++i) {
            NOSTR_CPU_RELAX();
            nsync_time dl_spin = nsync_time_add(nsync_time_now(), nsync_time_us(g_spin_us));
            nsync_mu_wait_with_deadline(&chan->mutex, channel_send_pred, &wa, NULL, dl_spin, NULL);
            // woke up (deadline or condition)
            nostr_metric_counter_add("go_chan_send_wait_wakeups", 1);
            if (NOSTR_UNLIKELY(chan->size == chan->capacity) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx))) {
                nostr_metric_counter_add("go_chan_send_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_send_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
        if (NOSTR_UNLIKELY(chan->size == chan->capacity) && !chan->closed && !(ctx && go_context_is_canceled(ctx))) {
            // Fallback to a longer park to avoid busy waiting when contention persists
            nsync_time dl = nsync_time_add(nsync_time_now(), nsync_time_ms(50));
            nsync_mu_wait_with_deadline(&chan->mutex, channel_send_pred, &wa, NULL, dl, NULL);
            nostr_metric_counter_add("go_chan_send_wait_wakeups", 1);
            if (NOSTR_UNLIKELY(chan->size == chan->capacity) && !chan->closed && !(ctx && go_context_is_canceled(ctx))) {
                nostr_metric_counter_add("go_chan_send_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_send_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
    }

    if (NOSTR_UNLIKELY(chan->closed || (ctx && go_context_is_canceled(ctx)))) {
        nsync_mu_unlock(&chan->mutex);
        ensure_histos();
        nostr_metric_timer_stop(&t, h_send_wait_ns);
        return -1; // Channel closed or canceled
    }

    // Add data to the buffer
    int was_empty2 = (chan->size == 0);
    __builtin_prefetch(&chan->buffer[chan->in], 1, 1);
    chan->buffer[chan->in] = data;
    go_channel_inc_in(chan);
    chan->size++;
    // success + depth sample (post-increment size)
    nostr_metric_counter_add("go_chan_send_successes", 1);
    nostr_metric_counter_add("go_chan_send_depth_samples", 1);
    nostr_metric_counter_add("go_chan_send_depth_sum", chan->size);

    // Signal one receiver that data is available only on 0->1
    if (was_empty2) {
        nsync_cv_signal(&chan->cond_empty);
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
    while (NOSTR_UNLIKELY(chan->size == 0) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx))) {
        if (!blocked) { nostr_metric_counter_add("go_chan_block_recvs", 1); blocked = 1; }
        // Spin-then-park: a few micro waits first
        for (int i = 0; i < g_spin_iters && NOSTR_UNLIKELY(chan->size == 0) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx)); ++i) {
            NOSTR_CPU_RELAX();
            nsync_time dl_spin = nsync_time_add(nsync_time_now(), nsync_time_us(g_spin_us));
            nsync_mu_wait_with_deadline(&chan->mutex, channel_recv_pred, &wa, NULL, dl_spin, NULL);
            // woke up
            nostr_metric_counter_add("go_chan_recv_wait_wakeups", 1);
            if (NOSTR_UNLIKELY(chan->size == 0) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx))) {
                nostr_metric_counter_add("go_chan_recv_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_recv_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
        if (NOSTR_UNLIKELY(chan->size == 0) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx))) {
            // Fallback to a longer park to avoid busy waiting when contention persists
            nsync_time dl = nsync_time_add(nsync_time_now(), nsync_time_ms(50));
            nsync_mu_wait_with_deadline(&chan->mutex, channel_recv_pred, &wa, NULL, dl, NULL);
            nostr_metric_counter_add("go_chan_recv_wait_wakeups", 1);
            if (NOSTR_UNLIKELY(chan->size == 0) && NOSTR_LIKELY(!chan->closed) && !(ctx && go_context_is_canceled(ctx))) {
                nostr_metric_counter_add("go_chan_recv_wait_spurious", 1);
            } else {
                nostr_metric_counter_add("go_chan_recv_wait_productive", 1);
                nostr_metric_timer_start(&tw);
                have_tw = 1;
            }
        }
    }

    if (NOSTR_UNLIKELY((chan->closed && chan->size == 0) || (ctx && go_context_is_canceled(ctx)))) {
        nsync_mu_unlock(&chan->mutex);
        ensure_histos();
        nostr_metric_timer_stop(&t, h_recv_wait_ns);
        return -1; // Channel is closed and empty or canceled
    }

    // Get data from the buffer
    int was_full2 = (chan->size == chan->capacity);
    __builtin_prefetch(&chan->buffer[chan->out], 0, 1);
    void *tmp2 = chan->buffer[chan->out];
    if (data) *data = tmp2;
    go_channel_inc_out(chan);
    chan->size--;
    // success + depth sample (post-decrement size)
    nostr_metric_counter_add("go_chan_recv_successes", 1);
    nostr_metric_counter_add("go_chan_recv_depth_samples", 1);
    nostr_metric_counter_add("go_chan_recv_depth_sum", chan->size);

    // Signal one sender that space is available only on full->not-full
    if (was_full2) {
        nsync_cv_signal(&chan->cond_full);
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
        nostr_metric_counter_add("go_chan_close_broadcasts", 1);
    }

    nsync_mu_unlock(&chan->mutex);
}
