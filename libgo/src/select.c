#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "select.h"
#include "channel.h"
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>

int go_select(GoSelectCase *cases, size_t num_cases) {
    // Simple fair-ish polling select using non-blocking ops
    for (;;) {
        size_t start = (size_t)(rand() % (int)num_cases);

        // Try each case once in randomized order
        for (size_t i = 0; i < num_cases; i++) {
            size_t idx = (start + i) % num_cases;
            GoSelectCase *c = &cases[idx];
            // Skip cases with NULL or invalid channels to avoid use-after-free
            if (c->chan == NULL) continue;
            // Validate magic number to detect garbage/freed channel pointers
            if (c->chan->magic != GO_CHANNEL_MAGIC) continue;
            if (c->op == GO_SELECT_SEND) {
                if (go_channel_try_send(c->chan, c->value) == 0) return (int)idx;
            } else { // GO_SELECT_RECEIVE
                void *dummy = NULL;
                void **dst = c->recv_buf ? c->recv_buf : &dummy;
                if (go_channel_try_receive(c->chan, dst) == 0) return (int)idx;
                // If the receive channel is closed, also consider it ready
                // CRITICAL: Set *dst to NULL to prevent caller from using garbage pointer
                if (go_channel_is_closed(c->chan)) {
                    if (c->recv_buf) *c->recv_buf = NULL;
                    return (int)idx;
                }
            }
        }

        // Nothing available; small sleep to avoid busy spin
        struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 1000 * 1000; // 1ms
        nanosleep(&ts, NULL);
    }
}

GoSelectResult go_select_timeout(GoSelectCase *cases, size_t num_cases, uint64_t timeout_ms) {
    GoSelectResult result = { .selected_case = -1, .ok = false };
    
    // Get start time
    struct timeval start_tv, now_tv;
    gettimeofday(&start_tv, NULL);
    uint64_t start_us = (uint64_t)start_tv.tv_sec * 1000000 + (uint64_t)start_tv.tv_usec;
    uint64_t timeout_us = timeout_ms * 1000;
    
    // Poll with timeout
    for (;;) {
        size_t start_idx = (size_t)(rand() % (int)num_cases);

        // Try each case once in randomized order
        for (size_t i = 0; i < num_cases; i++) {
            size_t idx = (start_idx + i) % num_cases;
            GoSelectCase *c = &cases[idx];
            // Skip cases with NULL or invalid channels to avoid use-after-free
            if (c->chan == NULL) continue;
            // Validate magic number to detect garbage/freed channel pointers
            if (c->chan->magic != GO_CHANNEL_MAGIC) continue;
            if (c->op == GO_SELECT_SEND) {
                if (go_channel_try_send(c->chan, c->value) == 0) {
                    result.selected_case = (int)idx;
                    result.ok = true;
                    return result;
                }
            } else { // GO_SELECT_RECEIVE
                void *dummy = NULL;
                void **dst = c->recv_buf ? c->recv_buf : &dummy;
                int recv_result = go_channel_try_receive(c->chan, dst);
                if (recv_result == 0) {
                    result.selected_case = (int)idx;
                    result.ok = true;
                    return result;
                }
                // If the receive channel is closed, also consider it ready
                // CRITICAL: Set recv_buf to NULL to prevent caller from using garbage pointer
                if (go_channel_is_closed(c->chan)) {
                    if (c->recv_buf) *c->recv_buf = NULL;
                    result.selected_case = (int)idx;
                    result.ok = false; // Channel closed
                    return result;
                }
            }
        }

        // Check timeout
        gettimeofday(&now_tv, NULL);
        uint64_t now_us = (uint64_t)now_tv.tv_sec * 1000000 + (uint64_t)now_tv.tv_usec;
        uint64_t elapsed_us = now_us - start_us;
        
        if (elapsed_us >= timeout_us) {
            // Timeout occurred
            result.selected_case = -1;
            result.ok = false;
            return result;
        }

        // Small sleep to avoid busy spin (but shorter than remaining timeout)
        uint64_t remaining_us = timeout_us - elapsed_us;
        uint64_t sleep_us = remaining_us < 1000 ? remaining_us : 1000; // Max 1ms sleep
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = (long)(sleep_us * 1000);
        nanosleep(&ts, NULL);
    }
}
