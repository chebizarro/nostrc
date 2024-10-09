#include "select.h"
#include "nsync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int go_select(GoSelectCase *cases, size_t num_cases) {
    while (1) {
        for (size_t i = 0; i < num_cases; i++) {
            GoSelectCase *c = &cases[i];
            if (c->op == GO_SELECT_SEND && go_channel_send(c->chan, c->value)) {
                return i; // Send successful, return index of the case
            }
            if (c->op == GO_SELECT_RECEIVE && go_channel_receive(c->chan, c->recv_buf)) {
                return i; // Receive successful, return index of the case
            }
        }
        // Randomization to avoid priority bias
        size_t start = rand() % num_cases;
        for (size_t i = 0; i < num_cases; i++) {
            size_t idx = (start + i) % num_cases;
            GoSelectCase *c = &cases[idx];
            nsync_mu_lock(&c->chan->mutex);
            if (c->op == GO_SELECT_SEND) {
                nsync_cv_wait(&c->chan->cond_full, &c->chan->mutex); // Wait for space
            } else {
                nsync_cv_wait(&c->chan->cond_empty, &c->chan->mutex); // Wait for data
            }
            nsync_mu_unlock(&c->chan->mutex);
        }
    }
}