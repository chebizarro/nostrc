#include "select.h"
#include "channel.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    GoChannel *channel;
    int selected;
} SelectThreadArgs;

static void *select_until_closed(void *userdata) {
    SelectThreadArgs *args = (SelectThreadArgs *)userdata;
    GoSelectCase cases[] = {
        (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = args->channel, .value = NULL, .recv_buf = NULL },
    };
    args->selected = go_select(cases, 1);
    return NULL;
}

int main(void) {
    GoChannel *c = go_channel_create(1);
    if (!c) {
        fprintf(stderr, "failed to create channel\n");
        return 1;
    }
    // Close immediately; select on receive should return promptly
    go_channel_close(c);

    GoSelectCase cases[] = {
        (GoSelectCase){ .op = GO_SELECT_RECEIVE, .chan = c, .value = NULL, .recv_buf = NULL },
    };
    int idx = go_select(cases, 1);
    if (idx != 0) {
        fprintf(stderr, "expected idx 0 for closed receive, got %d\n", idx);
        go_channel_free(c);
        return 2;
    }

    /* Regression for nostrc-480: shutdown can close a channel between
     * go_select's fast-path probe and waiter registration. Registering on an
     * already-closed channel must publish a terminal wakeup immediately. */
    GoSelectWaiter *waiter = go_select_waiter_create();
    if (!waiter) {
        go_channel_free(c);
        return 3;
    }
    go_channel_register_select_waiter(c, waiter);
    if (!atomic_load_explicit(&waiter->signaled, memory_order_acquire)) {
        fprintf(stderr, "closed channel did not signal late select waiter\n");
        go_select_waiter_unref(waiter);
        go_channel_free(c);
        return 4;
    }
    go_channel_unregister_select_waiter(c, waiter);
    go_select_waiter_unref(waiter);

    go_channel_free(c);

    /* Exercise both close-before-registration and close-after-registration.
     * Before nostrc-480, the latter could lose the CV wake between the
     * waiter's predicate check and nsync_cv_wait, hanging shutdown forever. */
    for (int iteration = 0; iteration < 500; ++iteration) {
        GoChannel *race_channel = go_channel_create(1);
        if (!race_channel) {
            fprintf(stderr, "failed to create race channel at iteration %d\n", iteration);
            return 5;
        }

        SelectThreadArgs args = {
            .channel = race_channel,
            .selected = -1,
        };
        pthread_t thread;
        if (pthread_create(&thread, NULL, select_until_closed, &args) != 0) {
            fprintf(stderr, "failed to create select thread at iteration %d\n", iteration);
            go_channel_free(race_channel);
            return 6;
        }

        if ((iteration & 1) != 0) {
            sched_yield();
        }
        go_channel_close(race_channel);
        pthread_join(thread, NULL);

        if (args.selected != 0) {
            fprintf(stderr, "closed race channel selected %d at iteration %d\n",
                    args.selected, iteration);
            go_channel_free(race_channel);
            return 7;
        }
        go_channel_free(race_channel);
    }

    return 0;
}
