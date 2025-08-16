#include "channel.h"
#include "nostr/metrics.h"
#include "context.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct { GoChannel *c; GoContext *ctx; } channel_wait_arg_t;

static int channel_send_pred(const void *arg) {
    const channel_wait_arg_t *wa = (const channel_wait_arg_t *)arg;
    const GoChannel *c = wa->c;
    return (c->closed || (c->size < c->capacity) || (wa->ctx && go_context_is_canceled(wa->ctx)));
}

int go_channel_is_closed(GoChannel *chan) {
    int closed = 0;
    nsync_mu_lock(&chan->mutex);
    closed = chan->closed;
    nsync_mu_unlock(&chan->mutex);
    return closed;
}

static int channel_recv_pred(const void *arg) {
    const channel_wait_arg_t *wa = (const channel_wait_arg_t *)arg;
    const GoChannel *c = wa->c;
    return (c->closed || (c->size > 0) || (wa->ctx && go_context_is_canceled(wa->ctx)));
}

/* Condition function to check if the channel has space */
int go_channel_has_space(const void *chan) {
    GoChannel *c = (GoChannel *)chan;
    return c->size < c->capacity;
}

/* Non-blocking send: returns 0 on success, -1 if full or closed */
int go_channel_try_send(GoChannel *chan, void *data) {
    int rc = -1;
    nsync_mu_lock(&chan->mutex);
    if (chan->buffer == NULL) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_counter_add("go_chan_try_send_failures", 1);
        return -1;
    }
    if (!chan->closed && chan->size < chan->capacity) {
        chan->buffer[chan->in] = data;
        chan->in = (chan->in + 1) % chan->capacity;
        chan->size++;
        // success + depth sample (post-increment size)
        nostr_metric_counter_add("go_chan_send_successes", 1);
        nostr_metric_counter_add("go_chan_send_depth_samples", 1);
        nostr_metric_counter_add("go_chan_send_depth_sum", chan->size);
        nsync_cv_signal(&chan->cond_empty);
        nostr_metric_counter_add("go_chan_signal_empty", 1);
        rc = 0;
    }
    nsync_mu_unlock(&chan->mutex);
    if (rc != 0) {
        nostr_metric_counter_add("go_chan_try_send_failures", 1);
    }
    return rc;
}

/* Non-blocking receive: returns 0 on success, -1 if empty (or closed and empty) */
int go_channel_try_receive(GoChannel *chan, void **data) {
    int rc = -1;
    nsync_mu_lock(&chan->mutex);
    if (chan->buffer == NULL) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_counter_add("go_chan_try_recv_failures", 1);
        return -1;
    }
    if (chan->size > 0) {
        void *tmp = chan->buffer[chan->out];
        if (data) *data = tmp;
        chan->out = (chan->out + 1) % chan->capacity;
        chan->size--;
        // success + depth sample (post-decrement size)
        nostr_metric_counter_add("go_chan_recv_successes", 1);
        nostr_metric_counter_add("go_chan_recv_depth_samples", 1);
        nostr_metric_counter_add("go_chan_recv_depth_sum", chan->size);
        nsync_cv_signal(&chan->cond_full);
        nostr_metric_counter_add("go_chan_signal_full", 1);
        rc = 0;
    }
    nsync_mu_unlock(&chan->mutex);
    if (rc != 0) {
        nostr_metric_counter_add("go_chan_try_recv_failures", 1);
    }
    return rc;
}

/* Condition function to check if the channel has data */
int go_channel_has_data(const void *chan) {
    GoChannel *c = (GoChannel *)chan;
    return c->size > 0;
}

/* Create a new channel with the given capacity */
GoChannel *go_channel_create(size_t capacity) {
    GoChannel *chan = malloc(sizeof(GoChannel));
    chan->buffer = malloc(sizeof(void *) * capacity);
    chan->capacity = capacity;
    chan->size = 0;
    chan->in = 0;
    chan->out = 0;
    chan->closed = false;
    nsync_mu_init(&chan->mutex);
    nsync_cv_init(&chan->cond_full);
    nsync_cv_init(&chan->cond_empty);
    return chan;
}

/* Free the channel resources */
void go_channel_free(GoChannel *chan) {
    if (chan == NULL) {
        return;
    }

    nsync_mu_lock(&chan->mutex);
    if (chan->buffer) {
        free(chan->buffer);
        chan->buffer = NULL;
    }
    nsync_mu_unlock(&chan->mutex);
    free(chan);
}

/* Send data to the channel */
int go_channel_send(GoChannel *chan, void *data) {
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    nsync_mu_lock(&chan->mutex);
    if (chan->buffer == NULL) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, nostr_metric_histogram_get("go_chan_send_wait_ns"));
        return -1;
    }

    while (chan->size == chan->capacity && !chan->closed) {
        if (!blocked) { nostr_metric_counter_add("go_chan_block_sends", 1); blocked = 1; }
        nsync_cv_wait(&chan->cond_full, &chan->mutex);
        nostr_metric_counter_add("go_chan_send_wait_wakeups", 1);
    }

    if (chan->closed) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, nostr_metric_histogram_get("go_chan_send_wait_ns"));
        return -1; // Cannot send to a closed channel
    }

    // Add data to the buffer
    chan->buffer[chan->in] = data;
    chan->in = (chan->in + 1) % chan->capacity;
    chan->size++;
    // success + depth sample (post-increment size)
    nostr_metric_counter_add("go_chan_send_successes", 1);
    nostr_metric_counter_add("go_chan_send_depth_samples", 1);
    nostr_metric_counter_add("go_chan_send_depth_sum", chan->size);

    // Signal receivers that data is available
    nsync_cv_signal(&chan->cond_empty);
    nostr_metric_counter_add("go_chan_signal_empty", 1);

    nsync_mu_unlock(&chan->mutex);
    nostr_metric_timer_stop(&t, nostr_metric_histogram_get("go_chan_send_wait_ns"));
    return 0;
}

/* Receive data from the channel */
int go_channel_receive(GoChannel *chan, void **data) {
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    nsync_mu_lock(&chan->mutex);
    if (chan->buffer == NULL) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, nostr_metric_histogram_get("go_chan_recv_wait_ns"));
        return -1;
    }

    while (chan->size == 0 && !chan->closed) {
        if (!blocked) { nostr_metric_counter_add("go_chan_block_recvs", 1); blocked = 1; }
        nsync_cv_wait(&chan->cond_empty, &chan->mutex);
        nostr_metric_counter_add("go_chan_recv_wait_wakeups", 1);
    }

    if (chan->closed && chan->size == 0) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, nostr_metric_histogram_get("go_chan_recv_wait_ns"));
        return -1; // Channel is closed and empty
    }

    // Get data from the buffer
    void *tmp = chan->buffer[chan->out];
    if (data) *data = tmp;
    chan->out = (chan->out + 1) % chan->capacity;
    chan->size--;
    // success + depth sample (post-decrement size)
    nostr_metric_counter_add("go_chan_recv_successes", 1);
    nostr_metric_counter_add("go_chan_recv_depth_samples", 1);
    nostr_metric_counter_add("go_chan_recv_depth_sum", chan->size);

    // Signal senders that space is available
    nsync_cv_signal(&chan->cond_full);
    nostr_metric_counter_add("go_chan_signal_full", 1);

    nsync_mu_unlock(&chan->mutex);
    nostr_metric_timer_stop(&t, nostr_metric_histogram_get("go_chan_recv_wait_ns"));
    return 0;
}

/* Send data to the channel with cancellation context */
int go_channel_send_with_context(GoChannel *chan, void *data, GoContext *ctx) {
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    nsync_mu_lock(&chan->mutex);
    if (chan->buffer == NULL) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, nostr_metric_histogram_get("go_chan_send_wait_ns"));
        return -1;
    }

    channel_wait_arg_t wa = { .c = chan, .ctx = ctx };
    while (chan->size == chan->capacity && !chan->closed && !(ctx && go_context_is_canceled(ctx))) {
        if (!blocked) { nostr_metric_counter_add("go_chan_block_sends", 1); blocked = 1; }
        // Wait with a short deadline so time-based contexts can be observed
        nsync_time dl = nsync_time_add(nsync_time_now(), nsync_time_ms(50));
        nsync_mu_wait_with_deadline(&chan->mutex, channel_send_pred, &wa, NULL, dl, NULL);
        nostr_metric_counter_add("go_chan_send_wait_wakeups", 1);
    }

    if (chan->closed || (ctx && go_context_is_canceled(ctx))) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, nostr_metric_histogram_get("go_chan_send_wait_ns"));
        return -1; // Channel closed or canceled
    }

    // Add data to the buffer
    chan->buffer[chan->in] = data;
    chan->in = (chan->in + 1) % chan->capacity;
    chan->size++;

    // Signal receivers that data is available
    nsync_cv_signal(&chan->cond_empty);

    nsync_mu_unlock(&chan->mutex);
    return 0;
}

/* Receive data from the channel with cancellation context */
int go_channel_receive_with_context(GoChannel *chan, void **data, GoContext *ctx) {
    nostr_metric_timer t; nostr_metric_timer_start(&t);
    int blocked = 0;
    nsync_mu_lock(&chan->mutex);
    if (chan->buffer == NULL) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, nostr_metric_histogram_get("go_chan_recv_wait_ns"));
        return -1;
    }

    channel_wait_arg_t wa = { .c = chan, .ctx = ctx };
    while (chan->size == 0 && !chan->closed && !(ctx && go_context_is_canceled(ctx))) {
        if (!blocked) { nostr_metric_counter_add("go_chan_block_recvs", 1); blocked = 1; }
        // Wait with a short deadline so time-based contexts can be observed
        nsync_time dl = nsync_time_add(nsync_time_now(), nsync_time_ms(50));
        nsync_mu_wait_with_deadline(&chan->mutex, channel_recv_pred, &wa, NULL, dl, NULL);
        nostr_metric_counter_add("go_chan_recv_wait_wakeups", 1);
    }

    if ((chan->closed && chan->size == 0) || (ctx && go_context_is_canceled(ctx))) {
        nsync_mu_unlock(&chan->mutex);
        nostr_metric_timer_stop(&t, nostr_metric_histogram_get("go_chan_recv_wait_ns"));
        return -1; // Channel is closed and empty or canceled
    }

    // Get data from the buffer
    void *tmp2 = chan->buffer[chan->out];
    if (data) *data = tmp2;
    chan->out = (chan->out + 1) % chan->capacity;
    chan->size--;
    // success + depth sample (post-decrement size)
    nostr_metric_counter_add("go_chan_recv_successes", 1);
    nostr_metric_counter_add("go_chan_recv_depth_samples", 1);
    nostr_metric_counter_add("go_chan_recv_depth_sum", chan->size);

    // Signal senders that space is available
    nsync_cv_signal(&chan->cond_full);
    nostr_metric_counter_add("go_chan_signal_full", 1);

    nsync_mu_unlock(&chan->mutex);
    return 0;
}

/* Close the channel (non-destructive): mark closed and wake waiters. */
void go_channel_close(GoChannel *chan) {
    nsync_mu_lock(&chan->mutex);

    if (!chan->closed) {
        chan->closed = true; // Mark the channel as closed
        // Wake up all potential waiters so they can observe closed state
        nsync_cv_broadcast(&chan->cond_full);
        nsync_cv_broadcast(&chan->cond_empty);
        nostr_metric_counter_add("go_chan_close_broadcasts", 1);
    }

    nsync_mu_unlock(&chan->mutex);
}
