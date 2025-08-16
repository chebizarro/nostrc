#include <unistd.h>
#include "select.h"
#include "channel.h"
#include <stdlib.h>

int go_select(GoSelectCase *cases, size_t num_cases) {
    // Simple fair-ish polling select using non-blocking ops
    for (;;) {
        size_t start = (size_t)(rand() % (int)num_cases);

        // Try each case once in randomized order
        for (size_t i = 0; i < num_cases; i++) {
            size_t idx = (start + i) % num_cases;
            GoSelectCase *c = &cases[idx];
            if (c->op == GO_SELECT_SEND) {
                if (go_channel_try_send(c->chan, c->value) == 0) return (int)idx;
            } else { // GO_SELECT_RECEIVE
                void *dummy = NULL;
                void **dst = c->recv_buf ? c->recv_buf : &dummy;
                if (go_channel_try_receive(c->chan, dst) == 0) return (int)idx;
                // If the receive channel is closed, also consider it ready
                if (go_channel_is_closed(c->chan)) return (int)idx;
            }
        }

        // Nothing available; small sleep to avoid busy spin
        usleep(1000); // 1ms
    }
}
