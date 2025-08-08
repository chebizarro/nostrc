#include "select.h"
#include <stdlib.h>
#include <unistd.h>

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
                if (go_channel_try_receive(c->chan, c->recv_buf) == 0) return (int)idx;
            }
        }

        // Nothing available; small sleep to avoid busy spin
        usleep(1000); // 1ms
    }
}
