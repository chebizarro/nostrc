#ifndef GNOSTR_POOL_CHANNEL_WAIT_H
#define GNOSTR_POOL_CHANNEL_WAIT_H

#include "select.h"
#include <nsync_time.h>

static void pool_wait_channels(GoSelectCase *cases, size_t count, uint64_t timeout_ms) {
    GoSelectWaiter *waiter = go_select_waiter_create();
    if (!waiter) return;
    for (size_t i = 0; i < count; i++)
        go_channel_register_select_waiter(cases[i].chan, waiter);
    bool ready = false;
    for (size_t i = 0; i < count; i++)
        if (go_channel_has_data(cases[i].chan) || go_channel_is_closed(cases[i].chan))
            ready = true;
    nsync_time deadline = nsync_time_add(nsync_time_now(), nsync_time_ms(timeout_ms));
    nsync_mu_lock(&waiter->mutex);
    while (!ready && !atomic_load(&waiter->signaled)) {
        if (nsync_cv_wait_with_deadline(&waiter->cond, &waiter->mutex, deadline, NULL) != 0)
            break;
    }
    nsync_mu_unlock(&waiter->mutex);
    for (size_t i = 0; i < count; i++)
        go_channel_unregister_select_waiter(cases[i].chan, waiter);
    go_select_waiter_unref(waiter);
}

#endif
