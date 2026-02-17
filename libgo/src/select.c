/**
 * @file select.c
 * @brief Event-driven select implementation using GoSelectWaiter registration.
 *
 * REPLACES the previous polling-loop implementation that used nanosleep(1ms)
 * between try_send/try_receive attempts. The new implementation:
 *
 *   1. Tries each case once (fast path — no syscall overhead)
 *   2. If no case is ready, registers a GoSelectWaiter with ALL channels
 *   3. Blocks on the waiter's condition variable (efficient OS-level sleep)
 *   4. When any channel transitions (send/recv/close), it signals the waiter
 *   5. Wakes up, unregisters, retries from step 1
 *
 * This eliminates the O(n_threads × 1ms) CPU waste from polling and reduces
 * channel operation latency from 0–1ms to microseconds.
 */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "select.h"
#include "channel.h"
#include "fiber_hooks.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

/* ── GoSelectWaiter implementation ────────────────────────────────────── */

void go_select_waiter_init(GoSelectWaiter *w) {
    if (!w) return;
    nsync_mu_init(&w->mutex);
    nsync_cv_init(&w->cond);
    atomic_store_explicit(&w->signaled, 0, memory_order_relaxed);
    w->fiber_handle = NULL;
    w->next = NULL;
}

void go_channel_register_select_waiter(GoChannel *chan, GoSelectWaiter *w) {
    if (!chan || !w) return;
    if (chan->magic != GO_CHANNEL_MAGIC) return;
    /* Must hold chan->mutex — caller is responsible (we take it here for safety) */
    nsync_mu_lock(&chan->mutex);
    /* Avoid double-registration: check if w is already in the list */
    GoSelectWaiter *cur = (GoSelectWaiter *)chan->select_waiters;
    while (cur) {
        if (cur == w) {
            nsync_mu_unlock(&chan->mutex);
            return; /* already registered */
        }
        cur = cur->next;
    }
    /* Prepend to linked list (O(1)) */
    w->next = (GoSelectWaiter *)chan->select_waiters;
    chan->select_waiters = (struct GoSelectWaiter *)w;
    nsync_mu_unlock(&chan->mutex);
}

void go_channel_unregister_select_waiter(GoChannel *chan, GoSelectWaiter *w) {
    if (!chan || !w) return;
    if (chan->magic != GO_CHANNEL_MAGIC) return;
    nsync_mu_lock(&chan->mutex);
    GoSelectWaiter **pp = (GoSelectWaiter **)&chan->select_waiters;
    while (*pp) {
        if (*pp == w) {
            *pp = w->next;
            w->next = NULL;
            break;
        }
        pp = &(*pp)->next;
    }
    nsync_mu_unlock(&chan->mutex);
}

/**
 * Signal all select waiters registered on a channel.
 * Called from channel.c send/recv/close paths when state transitions occur.
 * Must be called while holding chan->mutex.
 *
 * To avoid deadlock (ABBA lock ordering with waiter->mutex), we:
 * 1. Collect waiter pointers while holding chan->mutex
 * 2. Set the atomic signaled flag (lock-free)
 * 3. Signal condition variables AFTER releasing chan->mutex
 *
 * The caller is responsible for calling this function, then releasing
 * chan->mutex, then calling go_channel_signal_select_waiters_finish()
 * to actually wake the waiters.
 *
 * For simplicity, we use a two-phase approach:
 * - Phase 1 (this function): Set signaled flags atomically (safe under chan->mutex)
 * - Phase 2: Caller releases chan->mutex, then signals CVs
 *
 * However, to maintain the simple single-call API, we now signal CVs
 * without holding waiter->mutex since nsync_cv_signal is safe to call
 * without the mutex (it just wakes a waiter; the waiter re-checks the
 * predicate under its own mutex).
 */
void go_channel_signal_select_waiters(GoChannel *chan) {
    if (!chan) return;
    /* Walk the select_waiters list and signal each one.
     * The list is protected by chan->mutex which the caller holds.
     * Cache w->next before signaling to prevent use-after-free if the
     * waiter wakes and unregisters before we advance. */
    GoSelectWaiter *w = (GoSelectWaiter *)chan->select_waiters;
    while (w) {
        GoSelectWaiter *next = w->next;  /* Cache before signaling */
        /* Set signaled flag atomically — waiter checks this in cv_wait loop */
        atomic_store_explicit(&w->signaled, 1, memory_order_release);
        /* Wake the waiter — either fiber or OS thread */
        if (w->fiber_handle) {
            /* Fiber path: make the parked fiber runnable again */
            gof_hook_make_runnable(w->fiber_handle);
        } else {
            /* OS thread path: signal the CV without holding waiter->mutex
             * to avoid ABBA deadlock. nsync_cv_signal is safe to call
             * without the mutex — it just wakes a thread which will then
             * acquire the mutex and re-check the predicate. */
            nsync_cv_signal(&w->cond);
        }
        w = next;
    }
}

/* ── Helper: validate a channel pointer ──────────────────────────────── */

static inline int chan_valid(GoChannel *c) {
    return c != NULL
        && c->magic == GO_CHANNEL_MAGIC
        && c->buffer != NULL;
}

/* ── Helper: try all cases once (randomized start for fairness) ──────── */

typedef struct {
    int      selected;     /* index of ready case, or -1 */
    int      ok;           /* 1 if op succeeded, 0 if channel closed */
    size_t   valid_count;  /* how many cases had valid channels */
} TryResult;

static TryResult try_cases_once(GoSelectCase *cases, size_t num_cases) {
    TryResult r = { .selected = -1, .ok = 0, .valid_count = 0 };
    size_t start = (num_cases > 0) ? (size_t)(rand() % (int)num_cases) : 0;

    for (size_t i = 0; i < num_cases; i++) {
        size_t idx = (start + i) % num_cases;
        GoSelectCase *c = &cases[idx];
        if (!chan_valid(c->chan)) continue;
        r.valid_count++;

        if (c->op == GO_SELECT_SEND) {
            if (go_channel_try_send(c->chan, c->value) == 0) {
                r.selected = (int)idx;
                r.ok = 1;
                return r;
            }
        } else { /* GO_SELECT_RECEIVE */
            void *dummy = NULL;
            void **dst = c->recv_buf ? c->recv_buf : &dummy;
            if (go_channel_try_receive(c->chan, dst) == 0) {
                r.selected = (int)idx;
                r.ok = 1;
                return r;
            }
            /* Closed channel is also "ready" (returns immediately) */
            if (go_channel_is_closed(c->chan)) {
                if (c->recv_buf) *c->recv_buf = NULL;
                r.selected = (int)idx;
                r.ok = 0; /* closed */
                return r;
            }
        }
    }
    return r;
}

/* ── Helper: register waiter with all valid channels ─────────────────── */

static void register_waiter_all(GoSelectCase *cases, size_t num_cases,
                                GoSelectWaiter *w) {
    for (size_t i = 0; i < num_cases; i++) {
        if (chan_valid(cases[i].chan)) {
            go_channel_register_select_waiter(cases[i].chan, w);
        }
    }
}

static void unregister_waiter_all(GoSelectCase *cases, size_t num_cases,
                                  GoSelectWaiter *w) {
    for (size_t i = 0; i < num_cases; i++) {
        if (chan_valid(cases[i].chan)) {
            go_channel_unregister_select_waiter(cases[i].chan, w);
        }
    }
}

/* ── go_select: event-driven blocking select ─────────────────────────── */

int go_select(GoSelectCase *cases, size_t num_cases) {
    if (num_cases == 0) return -1;

    /* Fast path: try once without any registration overhead */
    TryResult r = try_cases_once(cases, num_cases);
    if (r.selected >= 0) return r.selected;
    if (r.valid_count == 0) return -1;

    /* Slow path: register waiter, block, retry */
    GoSelectWaiter waiter;
    go_select_waiter_init(&waiter);

    for (;;) {
        /* Reset signaled flag before registering */
        atomic_store_explicit(&waiter.signaled, 0, memory_order_relaxed);

        /* Register with all channels so any state change wakes us */
        register_waiter_all(cases, num_cases, &waiter);

        /* Double-check after registration to avoid lost wakeups:
         * A channel may have transitioned between our try and registration. */
        r = try_cases_once(cases, num_cases);
        if (r.selected >= 0) {
            unregister_waiter_all(cases, num_cases, &waiter);
            return r.selected;
        }
        if (r.valid_count == 0) {
            unregister_waiter_all(cases, num_cases, &waiter);
            return -1;
        }

        /* Block efficiently — either by parking the fiber (cooperative)
         * or by nsync_cv_wait (OS thread blocking). */
        gof_fiber_handle _sel_fiber = gof_hook_current();
        if (_sel_fiber) {
            /* Fiber path: park cooperatively.
             * The select waiter's signaled flag is set atomically by
             * go_channel_signal_select_waiters(), and the CV is signaled
             * which would wake an OS thread. For fibers, we store our
             * fiber handle and park. The CV signal handler in
             * go_channel_signal_select_waiters already sets signaled=1
             * and signals the CV. We need to also be woken by fiber wake.
             * Since we registered as a select waiter, we'll be woken
             * when any channel transitions. We just need to check signaled. */
            if (!atomic_load_explicit(&waiter.signaled, memory_order_acquire)) {
                /* Not signaled yet — park the fiber. We'll be woken by
                 * go_channel_signal_select_waiters which calls nsync_cv_signal
                 * on the waiter's CV. For fiber awareness, we piggyback on
                 * the signaled flag: since we've already registered as a
                 * select waiter, when any channel transitions, our signaled
                 * flag will be set. We just need a way to be woken.
                 *
                 * Store our fiber handle in the waiter for channel code to wake us. */
                waiter.fiber_handle = _sel_fiber;
                gof_hook_block_current(); /* Parks fiber — OS thread freed */
                waiter.fiber_handle = NULL;
            }
        } else {
            /* OS thread path: use nsync_cv_wait */
            nsync_mu_lock(&waiter.mutex);
            while (!atomic_load_explicit(&waiter.signaled, memory_order_acquire)) {
                nsync_cv_wait(&waiter.cond, &waiter.mutex);
            }
            nsync_mu_unlock(&waiter.mutex);
        }

        /* Unregister before retrying (avoids stale registrations) */
        unregister_waiter_all(cases, num_cases, &waiter);

        /* Something changed — retry all cases */
        r = try_cases_once(cases, num_cases);
        if (r.selected >= 0) return r.selected;
        if (r.valid_count == 0) return -1;

        /* Spurious wakeup or race — loop and re-register */
    }
}

/* ── go_select_timeout: event-driven select with timeout ─────────────── */

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

GoSelectResult go_select_timeout(GoSelectCase *cases, size_t num_cases,
                                 uint64_t timeout_ms) {
    GoSelectResult result = { .selected_case = -1, .ok = false };
    if (num_cases == 0) return result;

    /* Fast path: try once without registration */
    TryResult r = try_cases_once(cases, num_cases);
    if (r.selected >= 0) {
        result.selected_case = r.selected;
        result.ok = (r.ok != 0);
        return result;
    }
    if (r.valid_count == 0) return result;

    /* Compute absolute deadline */
    uint64_t start = now_us();
    uint64_t deadline_us = start + timeout_ms * 1000ull;

    /* Slow path: register waiter, block with timeout, retry */
    GoSelectWaiter waiter;
    go_select_waiter_init(&waiter);

    for (;;) {
        /* Check timeout before blocking */
        uint64_t now = now_us();
        if (now >= deadline_us) {
            result.selected_case = -1;
            result.ok = false;
            return result;
        }

        /* Reset signaled flag */
        atomic_store_explicit(&waiter.signaled, 0, memory_order_relaxed);

        /* Register with all channels */
        register_waiter_all(cases, num_cases, &waiter);

        /* Double-check after registration */
        r = try_cases_once(cases, num_cases);
        if (r.selected >= 0) {
            unregister_waiter_all(cases, num_cases, &waiter);
            result.selected_case = r.selected;
            result.ok = (r.ok != 0);
            return result;
        }
        if (r.valid_count == 0) {
            unregister_waiter_all(cases, num_cases, &waiter);
            return result;
        }

        /* Block with timeout using nsync_cv_wait_with_deadline.
         * Convert remaining microseconds to absolute timespec. */
        uint64_t remaining_us = deadline_us - now_us();
        if (remaining_us == 0) {
            unregister_waiter_all(cases, num_cases, &waiter);
            return result; /* timeout */
        }

        /* nsync_cv_wait_with_deadline needs an absolute CLOCK_REALTIME time */
        struct timespec abs_deadline;
        {
            struct timespec cur;
            clock_gettime(CLOCK_REALTIME, &cur);
            uint64_t cur_ns = (uint64_t)cur.tv_sec * 1000000000ull
                            + (uint64_t)cur.tv_nsec;
            uint64_t deadline_ns = cur_ns + remaining_us * 1000ull;
            abs_deadline.tv_sec  = (time_t)(deadline_ns / 1000000000ull);
            abs_deadline.tv_nsec = (long)(deadline_ns % 1000000000ull);
        }

        gof_fiber_handle _sel_fiber_t = gof_hook_current();
        if (_sel_fiber_t) {
            /* Fiber path: park with timeout awareness.
             * Use gof_hook_block_current_until() which parks the fiber with a
             * deadline. The fiber will be woken either by:
             *   (a) channel transition → go_channel_signal_select_waiters → make_runnable
             *   (b) scheduler sleeper timeout → automatic wake when deadline expires
             * This avoids indefinite parking when no channel activity occurs. */
            if (!atomic_load_explicit(&waiter.signaled, memory_order_acquire)) {
                waiter.fiber_handle = _sel_fiber_t;
                /* Convert remaining_us to absolute nanosecond deadline for the scheduler */
                struct timespec _ts_now;
                clock_gettime(CLOCK_REALTIME, &_ts_now);
                uint64_t _abs_deadline_ns = (uint64_t)_ts_now.tv_sec * 1000000000ull
                                          + (uint64_t)_ts_now.tv_nsec
                                          + remaining_us * 1000ull;
                gof_hook_block_current_until(_abs_deadline_ns);
                waiter.fiber_handle = NULL;
            }
        } else {
            /* OS thread path: use nsync_cv_wait_with_deadline */
            nsync_mu_lock(&waiter.mutex);
            while (!atomic_load_explicit(&waiter.signaled, memory_order_acquire)) {
                int wait_rc = nsync_cv_wait_with_deadline(
                    &waiter.cond, &waiter.mutex, abs_deadline, NULL);
                if (wait_rc != 0) {
                    /* Timeout expired */
                    break;
                }
            }
            nsync_mu_unlock(&waiter.mutex);
        }

        /* Unregister before retrying */
        unregister_waiter_all(cases, num_cases, &waiter);

        /* Retry all cases */
        r = try_cases_once(cases, num_cases);
        if (r.selected >= 0) {
            result.selected_case = r.selected;
            result.ok = (r.ok != 0);
            return result;
        }
        if (r.valid_count == 0) return result;

        /* Check timeout after retry */
        if (now_us() >= deadline_us) {
            result.selected_case = -1;
            result.ok = false;
            return result;
        }

        /* Spurious wakeup or race — loop and re-register */
    }
}
