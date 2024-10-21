#include "select.h"
#include "nsync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int go_select(GoSelectCase *cases, size_t num_cases) {
    while (1) {
        // First check for immediate availability in the channels
        for (size_t i = 0; i < num_cases; i++) {
            GoSelectCase *c = &cases[i];

            if (c->op == GO_SELECT_SEND) {
                int result = go_channel_send(c->chan, c->value);
                if (result == 0) {
                    return i; // Send successful, return index of the case
                }
            } else if (c->op == GO_SELECT_RECEIVE) {
                int result = go_channel_receive(c->chan, c->recv_buf);
                if (result == 0) {
                    return i; // Receive successful, return index of the case
                }
            }
        }

        // If no immediate operation was successful, we need to wait
        size_t start = rand() % num_cases; // Randomize starting point to avoid bias

        for (size_t i = 0; i < num_cases; i++) {
            size_t idx = (start + i) % num_cases;
            GoSelectCase *c = &cases[idx];

            nsync_mu_lock(&c->chan->mutex);

            if (c->op == GO_SELECT_SEND) {
                // Check if there is space in the channel to send
                if (c->chan->size < c->chan->capacity && !c->chan->closed) {
                    nsync_mu_unlock(&c->chan->mutex);
                    continue;  // Skip waiting, go back and retry send
                }
                nsync_cv_wait(&c->chan->cond_full, &c->chan->mutex); // Wait for space

            } else if (c->op == GO_SELECT_RECEIVE) {
                // Check if there is data available in the channel to receive
                if (c->chan->size > 0 || c->chan->closed) {
                    nsync_mu_unlock(&c->chan->mutex);
                    continue;  // Skip waiting, go back and retry receive
                }
                nsync_cv_wait(&c->chan->cond_empty, &c->chan->mutex); // Wait for data
            }

            nsync_mu_unlock(&c->chan->mutex);
        }
    }
}
