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
 *
 * LIFECYCLE SAFETY (nostrc-select-refcount):
 *   GoSelectWaiter is heap-allocated with reference counting. Each channel
 *   registration node holds a ref, and go_select holds one ref. The embedded
 *   nsync_mu/nsync_cv are guaranteed to be alive as long as any ref exists.
 *   Additionally, go_select takes refs on all channels in the cases array
 *   to prevent channels from being freed during the select operation.
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
    atomic_store_explicit(&w->refcount, 1, memory_order_relaxed);
}

GoSelectWaiter *go_select_waiter_create(void) {
    GoSelectWaiter *w = (GoSelectWaiter *)calloc(1, sizeof(GoSelectWaiter));
    if (!w) return NULL;
    go_select_waiter_init(w);
    return w;
}

GoSelectWaiter *go_select_waiter_ref(GoSelectWaiter *w) {
    if (!w) return NULL;
    atomic_fetch_add_explicit(&w->refcount, 1, memory_order_relaxed);
    return w;
}

void go_select_waiter_unref(GoSelectWaiter *w) {
    if (!w) return;
    int prev = atomic_fetch_sub_explicit(&w->refcount, 1, memory_order_acq_rel);
    if (prev == 1) {
        /* Last ref — safe to free. All channel registrations have been
         * removed (or cleaned up by channel destruction), so no thread
         * can access w->mutex or w->cond after this point. */
        free(w);
    } else if (prev <= 0) {
        /* Double-unref guard — restore and log. This should never happen. */
        atomic_fetch_add_explicit(&w->refcount, 1, memory_order_relaxed);
    }
}

void go_channel_register_select_waiter(GoChannel *chan, GoSelectWaiter *w) {
    if (!chan || !w) return;
    if (chan->magic != GO_CHANNEL_MAGIC) return;
    /* Must hold chan->mutex — caller is responsible (we take it here for safety) */
    nsync_mu_lock(&chan->mutex);
    /* Avoid double-registration: check if w is already in the list */
    GoSelectWaiterNode *cur = (GoSelectWaiterNode *)chan->select_waiters;
    while (cur) {
        if (cur->waiter == w) {
            nsync_mu_unlock(&chan->mutex);
            return; /* already registered */
        }
        cur = cur->next;
    }

    /* Prepend per-channel registration node (O(1)).
     * Do NOT store linkage in GoSelectWaiter itself: one waiter can be
     * registered on multiple channels concurrently. */
    GoSelectWaiterNode *node = (GoSelectWaiterNode *)malloc(sizeof(GoSelectWaiterNode));
    if (!node) {
        nsync_mu_unlock(&chan->mutex);
        return;
    }
    node->waiter = go_select_waiter_ref(w);  /* Node takes a ref on the waiter */
    node->next = (GoSelectWaiterNode *)chan->select_waiters;
    chan->select_waiters = node;
    nsync_mu_unlock(&chan->mutex);
}

void go_channel_unregister_select_waiter(GoChannel *chan, GoSelectWaiter *w) {
    if (!chan || !w) return;
    if (chan->magic != GO_CHANNEL_MAGIC) return;
    nsync_mu_lock(&chan->mutex);
    GoSelectWaiterNode **pp = (GoSelectWaiterNode **)&chan->select_waiters;
    while (*pp) {
        GoSelectWaiterNode *node = *pp;
        if (node->waiter == w) {
            *pp = node->next;
            go_select_waiter_unref(node->waiter);  /* Drop node's ref */
            free(node);
            break;
        }
        pp = &(*pp)->next;
    }
    nsync_mu_unlock(&chan->mutex);
}

/**
 * Clean up all select waiter registrations on a channel.
 * Called from go_channel_unref when the channel is being destroyed.
 * Must be called while holding chan->mutex.
 *
 * This prevents leaked GoSelectWaiterNode entries (and their dangling
 * waiter pointers) when a channel dies while select waiters are registered.
 * Previously, unregister_waiter_all would skip dead channels (magic=0),
 * leaving orphaned nodes.
 */
void go_channel_cleanup_select_waiters(GoChannel *chan) {
    GoSelectWaiterNode *node = (GoSelectWaiterNode *)chan->select_waiters;
    chan->select_waiters = NULL;
    while (node) {
        GoSelectWaiterNode *next = node->next;
        go_select_waiter_unref(node->waiter);  /* Drop node's ref */
        free(node);
        node = next;
    }
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
 *
 * SAFETY: Each node holds a ref on its waiter, so the waiter is guaranteed
 * to be alive when we access w->cond/w->signaled here. The ref is only
 * dropped when the node is freed (in unregister or cleanup).
 */
void go_channel_signal_select_waiters(GoChannel *chan) {
    if (!chan) return;
    /* Walk the select_waiters list and signal each one.
     * The list is protected by chan->mutex which the caller holds.
     * Cache node->next before signaling to prevent use-after-free if the
     * waiter wakes and unregisters before we advance. */
    GoSelectWaiterNode *node = (GoSelectWaiterNode *)chan->select_waiters;
    while (node) {
        GoSelectWaiterNode *next = node->next;  /* Cache before signaling */
        GoSelectWaiter *w = node->waiter;
        if (!w) {
            node = next;
            continue;
        }
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
        node = next;
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

/* ── Helper: take/release refs on all valid channels ─────────────────
 *
 * go_select must hold refs on all channels for the duration of the call
 * to prevent channels from being freed (and reaped from the graveyard)
 * while select is operating on them. Without this, a dangling channel
 * pointer in the GoSelectCase array could cause reads from freed memory
 * in chan_valid(), register_waiter_all(), or unregister_waiter_all().
 */

static void ref_channels(GoSelectCase *cases, size_t num_cases) {
    for (size_t i = 0; i < num_cases; i++) {
        if (cases[i].chan && cases[i].chan->magic == GO_CHANNEL_MAGIC) {
            go_channel_ref(cases[i].chan);
        }
    }
}

static void unref_channels(GoSelectCase *cases, size_t num_cases) {
    for (size_t i = 0; i < num_cases; i++) {
        /* Only unref channels with valid magic. Channels we ref'd will
         * have valid magic because our ref prevents destruction. Channels
         * that were already dead when we started are safely skipped. */
        if (cases[i].chan && cases[i].chan->magic == GO_CHANNEL_MAGIC) {
            go_channel_unref(cases[i].chan);
        }
    }
}

/* ── go_select: event-driven blocking select ─────────────────────────── */

int go_select(GoSelectCase *cases, size_t num_cases) {
    if (num_cases == 0) return -1;

    /* Take refs on all valid channels to prevent destruction during select.
     * This is the primary defense against dangling channel pointers. */
    ref_channels(cases, num_cases);

    /* Fast path: try once without any registration overhead */
    TryResult r = try_cases_once(cases, num_cases);
    if (r.selected >= 0) {
        unref_channels(cases, num_cases);
        return r.selected;
    }
    if (r.valid_count == 0) {
        unref_channels(cases, num_cases);
        return -1;
    }

    /* Slow path: heap-allocated waiter with refcounting.
     * Previously the waiter was stack-allocated, which meant its embedded
     * nsync_mu/nsync_cv could be accessed after the function returned if
     * a channel signaled the waiter in a narrow race window. Heap allocation
     * with refcounting ensures the waiter lives until all references are gone. */
    GoSelectWaiter *waiter = go_select_waiter_create();
    if (!waiter) {
        unref_channels(cases, num_cases);
        return -1;
    }

    for (;;) {
        /* Reset signaled flag before registering */
        atomic_store_explicit(&waiter->signaled, 0, memory_order_relaxed);

        /* Register with all channels so any state change wakes us */
        register_waiter_all(cases, num_cases, waiter);

        /* Double-check after registration to avoid lost wakeups:
         * A channel may have transitioned between our try and registration. */
        r = try_cases_once(cases, num_cases);
        if (r.selected >= 0) {
            unregister_waiter_all(cases, num_cases, waiter);
            go_select_waiter_unref(waiter);
            unref_channels(cases, num_cases);
            return r.selected;
        }
        if (r.valid_count == 0) {
            unregister_waiter_all(cases, num_cases, waiter);
            go_select_waiter_unref(waiter);
            unref_channels(cases, num_cases);
            return -1;
        }

        /* Block efficiently — either by parking the fiber (cooperative)
         * or by nsync_cv_wait (OS thread blocking). */
        gof_fiber_handle _sel_fiber = gof_hook_current();
        if (_sel_fiber) {
            /* Fiber path: park cooperatively.
             *
             * RACE FIX: We must set fiber_handle BEFORE checking signaled,
             * otherwise a signal arriving between check and set will call
             * gof_hook_make_runnable(NULL). Use the waiter's mutex to make
             * the set-handle + check-signaled + park sequence atomic with
             * respect to the signaler. */
            nsync_mu_lock(&waiter->mutex);
            waiter->fiber_handle = _sel_fiber;
            if (!atomic_load_explicit(&waiter->signaled, memory_order_acquire)) {
                /* Not signaled yet — park the fiber. Release mutex before
                 * parking so the signaler can acquire it. */
                nsync_mu_unlock(&waiter->mutex);
                gof_hook_block_current(); /* Parks fiber — OS thread freed */
            } else {
                /* Already signaled — don't park, just continue */
                nsync_mu_unlock(&waiter->mutex);
            }
            waiter->fiber_handle = NULL;
        } else {
            /* OS thread path: use nsync_cv_wait */
            nsync_mu_lock(&waiter->mutex);
            while (!atomic_load_explicit(&waiter->signaled, memory_order_acquire)) {
                nsync_cv_wait(&waiter->cond, &waiter->mutex);
            }
            nsync_mu_unlock(&waiter->mutex);
        }

        /* Unregister before retrying (avoids stale registrations) */
        unregister_waiter_all(cases, num_cases, waiter);

        /* Something changed — retry all cases */
        r = try_cases_once(cases, num_cases);
        if (r.selected >= 0) {
            go_select_waiter_unref(waiter);
            unref_channels(cases, num_cases);
            return r.selected;
        }
        if (r.valid_count == 0) {
            go_select_waiter_unref(waiter);
            unref_channels(cases, num_cases);
            return -1;
        }

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

    /* Take refs on all valid channels to prevent destruction during select */
    ref_channels(cases, num_cases);

    /* Fast path: try once without registration */
    TryResult r = try_cases_once(cases, num_cases);
    if (r.selected >= 0) {
        result.selected_case = r.selected;
        result.ok = (r.ok != 0);
        unref_channels(cases, num_cases);
        return result;
    }
    if (r.valid_count == 0) {
        unref_channels(cases, num_cases);
        return result;
    }

    /* Compute absolute deadline */
    uint64_t start = now_us();
    uint64_t deadline_us = start + timeout_ms * 1000ull;

    /* Slow path: heap-allocated waiter with refcounting */
    GoSelectWaiter *waiter = go_select_waiter_create();
    if (!waiter) {
        unref_channels(cases, num_cases);
        return result;
    }

    for (;;) {
        /* Check timeout before blocking */
        uint64_t now = now_us();
        if (now >= deadline_us) {
            result.selected_case = -1;
            result.ok = false;
            go_select_waiter_unref(waiter);
            unref_channels(cases, num_cases);
            return result;
        }

        /* Reset signaled flag */
        atomic_store_explicit(&waiter->signaled, 0, memory_order_relaxed);

        /* Register with all channels */
        register_waiter_all(cases, num_cases, waiter);

        /* Double-check after registration */
        r = try_cases_once(cases, num_cases);
        if (r.selected >= 0) {
            unregister_waiter_all(cases, num_cases, waiter);
            result.selected_case = r.selected;
            result.ok = (r.ok != 0);
            go_select_waiter_unref(waiter);
            unref_channels(cases, num_cases);
            return result;
        }
        if (r.valid_count == 0) {
            unregister_waiter_all(cases, num_cases, waiter);
            go_select_waiter_unref(waiter);
            unref_channels(cases, num_cases);
            return result;
        }

        /* Block with timeout using nsync_cv_wait_with_deadline.
         * Convert remaining microseconds to absolute timespec. */
        uint64_t remaining_us = deadline_us - now_us();
        if (remaining_us == 0) {
            unregister_waiter_all(cases, num_cases, waiter);
            go_select_waiter_unref(waiter);
            unref_channels(cases, num_cases);
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
             *
             * RACE FIX: Same as go_select — set fiber_handle BEFORE checking
             * signaled, using mutex to make the sequence atomic. */
            nsync_mu_lock(&waiter->mutex);
            waiter->fiber_handle = _sel_fiber_t;
            if (!atomic_load_explicit(&waiter->signaled, memory_order_acquire)) {
                /* Not signaled yet — compute deadline and park */
                struct timespec _ts_now;
                clock_gettime(CLOCK_REALTIME, &_ts_now);
                uint64_t _abs_deadline_ns = (uint64_t)_ts_now.tv_sec * 1000000000ull
                                          + (uint64_t)_ts_now.tv_nsec
                                          + remaining_us * 1000ull;
                nsync_mu_unlock(&waiter->mutex);
                gof_hook_block_current_until(_abs_deadline_ns);
            } else {
                /* Already signaled — don't park */
                nsync_mu_unlock(&waiter->mutex);
            }
            waiter->fiber_handle = NULL;
        } else {
            /* OS thread path: use nsync_cv_wait_with_deadline */
            nsync_mu_lock(&waiter->mutex);
            while (!atomic_load_explicit(&waiter->signaled, memory_order_acquire)) {
                int wait_rc = nsync_cv_wait_with_deadline(
                    &waiter->cond, &waiter->mutex, abs_deadline, NULL);
                if (wait_rc != 0) {
                    /* Timeout expired */
                    break;
                }
            }
            nsync_mu_unlock(&waiter->mutex);
        }

        /* Unregister before retrying */
        unregister_waiter_all(cases, num_cases, waiter);

        /* Retry all cases */
        r = try_cases_once(cases, num_cases);
        if (r.selected >= 0) {
            result.selected_case = r.selected;
            result.ok = (r.ok != 0);
            go_select_waiter_unref(waiter);
            unref_channels(cases, num_cases);
            return result;
        }
        if (r.valid_count == 0) {
            go_select_waiter_unref(waiter);
            unref_channels(cases, num_cases);
            return result;
        }

        /* Check timeout after retry */
        if (now_us() >= deadline_us) {
            result.selected_case = -1;
            result.ok = false;
            go_select_waiter_unref(waiter);
            unref_channels(cases, num_cases);
            return result;
        }

        /* Spurious wakeup or race — loop and re-register */
    }
}
