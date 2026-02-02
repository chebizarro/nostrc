#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "select.h"
#include "channel.h"
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>

/* Try to complete one case without blocking.
 * Returns the case index on success, -1 if nothing ready, -2 if all channels invalid. */
static int try_select_once(GoSelectCase *cases, size_t num_cases, size_t start_idx) {
    size_t valid_cases = 0;

    for (size_t i = 0; i < num_cases; i++) {
        size_t idx = (start_idx + i) % num_cases;
        GoSelectCase *c = &cases[idx];

        // Skip cases with NULL or invalid channels to avoid use-after-free
        if (c->chan == NULL) continue;
        // Validate magic number to detect garbage/freed channel pointers
        if (c->chan->magic != GO_CHANNEL_MAGIC) continue;
        // Additional check: ensure buffer is valid (channel not being freed)
        if (c->chan->buffer == NULL) continue;

        valid_cases++;

        if (c->op == GO_SELECT_SEND) {
            if (go_channel_try_send(c->chan, c->value) == 0) {
                return (int)idx;
            }
        } else { // GO_SELECT_RECEIVE
            void *dummy = NULL;
            void **dst = c->recv_buf ? c->recv_buf : &dummy;
            if (go_channel_try_receive(c->chan, dst) == 0) {
                return (int)idx;
            }
            // If the receive channel is closed, also consider it ready
            // CRITICAL: Set *dst to NULL to prevent caller from using garbage pointer
            if (go_channel_is_closed(c->chan)) {
                if (c->recv_buf) *c->recv_buf = NULL;
                return (int)idx;
            }
        }
    }

    // If all channels are invalid/freed, signal error
    if (valid_cases == 0) {
        return -2;
    }

    return -1; // Nothing ready
}

int go_select(GoSelectCase *cases, size_t num_cases) {
    if (num_cases == 0) return -1;

    // Initialize a waiter to be signaled when any channel becomes ready
    GoSelectWaiter waiter;
    go_select_waiter_init(&waiter);

    // Random starting point for fairness
    size_t start_idx = (size_t)(rand() % (int)num_cases);

    // First try without blocking
    int result = try_select_once(cases, num_cases, start_idx);
    if (result >= 0 || result == -2) {
        return result == -2 ? -1 : result;
    }

    // Register with all valid channels
    for (size_t i = 0; i < num_cases; i++) {
        GoSelectCase *c = &cases[i];
        if (c->chan && c->chan->magic == GO_CHANNEL_MAGIC && c->chan->buffer) {
            go_channel_register_select_waiter(c->chan, &waiter);
        }
    }

    // Loop until a case succeeds
    for (;;) {
        // Try all cases again
        result = try_select_once(cases, num_cases, start_idx);
        if (result >= 0 || result == -2) {
            break;
        }

        // Wait for any channel to signal us
        nsync_mu_lock(&waiter.mutex);
        while (!atomic_load_explicit(&waiter.signaled, memory_order_acquire)) {
            nsync_cv_wait(&waiter.cond, &waiter.mutex);
        }
        // Reset signaled flag for next iteration
        atomic_store_explicit(&waiter.signaled, 0, memory_order_release);
        nsync_mu_unlock(&waiter.mutex);

        // Rotate start index for fairness
        start_idx = (start_idx + 1) % num_cases;
    }

    // Unregister from all channels
    for (size_t i = 0; i < num_cases; i++) {
        GoSelectCase *c = &cases[i];
        if (c->chan && c->chan->magic == GO_CHANNEL_MAGIC) {
            go_channel_unregister_select_waiter(c->chan, &waiter);
        }
    }

    return result == -2 ? -1 : result;
}

GoSelectResult go_select_timeout(GoSelectCase *cases, size_t num_cases, uint64_t timeout_ms) {
    GoSelectResult result = { .selected_case = -1, .ok = false };
    if (num_cases == 0) return result;

    // Initialize a waiter to be signaled when any channel becomes ready
    GoSelectWaiter waiter;
    go_select_waiter_init(&waiter);

    // Random starting point for fairness
    size_t start_idx = (size_t)(rand() % (int)num_cases);

    // Calculate deadline
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }

    // First try without blocking
    int try_result = try_select_once(cases, num_cases, start_idx);
    if (try_result >= 0) {
        // Check if this was a closed channel receive
        GoSelectCase *c = &cases[try_result];
        if (c->op == GO_SELECT_RECEIVE && go_channel_is_closed(c->chan)) {
            result.selected_case = try_result;
            result.ok = false;
        } else {
            result.selected_case = try_result;
            result.ok = true;
        }
        return result;
    }
    if (try_result == -2) {
        // All channels invalid
        return result;
    }

    // Register with all valid channels
    for (size_t i = 0; i < num_cases; i++) {
        GoSelectCase *c = &cases[i];
        if (c->chan && c->chan->magic == GO_CHANNEL_MAGIC && c->chan->buffer) {
            go_channel_register_select_waiter(c->chan, &waiter);
        }
    }

    // Loop until a case succeeds or timeout
    for (;;) {
        // Try all cases again
        try_result = try_select_once(cases, num_cases, start_idx);
        if (try_result >= 0 || try_result == -2) {
            break;
        }

        // Wait for signal with timeout
        nsync_mu_lock(&waiter.mutex);
        int wait_result = 0;
        while (!atomic_load_explicit(&waiter.signaled, memory_order_acquire) && wait_result == 0) {
            wait_result = nsync_cv_wait_with_deadline(&waiter.cond, &waiter.mutex,
                                                       deadline, NULL);
        }
        int was_signaled = atomic_load_explicit(&waiter.signaled, memory_order_acquire);
        atomic_store_explicit(&waiter.signaled, 0, memory_order_release);
        nsync_mu_unlock(&waiter.mutex);

        if (!was_signaled && wait_result != 0) {
            // Timeout occurred
            break;
        }

        // Rotate start index for fairness
        start_idx = (start_idx + 1) % num_cases;
    }

    // Unregister from all channels
    for (size_t i = 0; i < num_cases; i++) {
        GoSelectCase *c = &cases[i];
        if (c->chan && c->chan->magic == GO_CHANNEL_MAGIC) {
            go_channel_unregister_select_waiter(c->chan, &waiter);
        }
    }

    if (try_result >= 0) {
        // Check if this was a closed channel receive
        GoSelectCase *c = &cases[try_result];
        if (c->op == GO_SELECT_RECEIVE && go_channel_is_closed(c->chan)) {
            result.selected_case = try_result;
            result.ok = false;
        } else {
            result.selected_case = try_result;
            result.ok = true;
        }
    }
    // else: timeout or all invalid - result is already { -1, false }

    return result;
}
