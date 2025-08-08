#include "channel.h"
#include <stdio.h>
#include <stdlib.h>

/* Condition function to check if the channel has space */
int go_channel_has_space(const void *chan) {
    GoChannel *c = (GoChannel *)chan;
    return c->size < c->capacity;
}

/* Non-blocking send: returns 0 on success, -1 if full or closed */
int go_channel_try_send(GoChannel *chan, void *data) {
    int rc = -1;
    nsync_mu_lock(&chan->mutex);
    if (!chan->closed && chan->size < chan->capacity) {
        chan->buffer[chan->in] = data;
        chan->in = (chan->in + 1) % chan->capacity;
        chan->size++;
        nsync_cv_signal(&chan->cond_empty);
        rc = 0;
    }
    nsync_mu_unlock(&chan->mutex);
    return rc;
}

/* Non-blocking receive: returns 0 on success, -1 if empty (or closed and empty) */
int go_channel_try_receive(GoChannel *chan, void **data) {
    int rc = -1;
    nsync_mu_lock(&chan->mutex);
    if (chan->size > 0) {
        *data = chan->buffer[chan->out];
        chan->out = (chan->out + 1) % chan->capacity;
        chan->size--;
        nsync_cv_signal(&chan->cond_full);
        rc = 0;
    }
    nsync_mu_unlock(&chan->mutex);
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
    nsync_mu_lock(&chan->mutex);

    while (chan->size == chan->capacity && !chan->closed) {
        nsync_cv_wait(&chan->cond_full, &chan->mutex);
    }

    if (chan->closed) {
        nsync_mu_unlock(&chan->mutex);
        return -1; // Cannot send to a closed channel
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

/* Receive data from the channel */
int go_channel_receive(GoChannel *chan, void **data) {
    nsync_mu_lock(&chan->mutex);

    while (chan->size == 0 && !chan->closed) {
        nsync_cv_wait(&chan->cond_empty, &chan->mutex);
    }

    if (chan->closed && chan->size == 0) {
        nsync_mu_unlock(&chan->mutex);
        return -1; // Channel is closed and empty
    }

    // Get data from the buffer
    *data = chan->buffer[chan->out];
    chan->out = (chan->out + 1) % chan->capacity;
    chan->size--;

    // Signal senders that space is available
    nsync_cv_signal(&chan->cond_full);

    nsync_mu_unlock(&chan->mutex);
    return 0;
}

/* Send data to the channel with cancellation context */
int go_channel_send_with_context(GoChannel *chan, void *data, GoContext *ctx) {
    nsync_mu_lock(&chan->mutex);

    while (chan->size == chan->capacity && !chan->closed) {
        if (ctx && go_context_is_canceled(ctx)) {
            nsync_mu_unlock(&chan->mutex);
            return -1; // Canceled
        }
        // Wait until space is available
        nsync_cv_wait(&chan->cond_full, &chan->mutex);
    }

    if (chan->closed) {
        nsync_mu_unlock(&chan->mutex);
        return -1; // Channel is closed
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
    nsync_mu_lock(&chan->mutex);

    while (chan->size == 0 && !chan->closed) {
        if (ctx && go_context_is_canceled(ctx)) {
            nsync_mu_unlock(&chan->mutex);
            return -1; // Canceled
        }
        // Wait until data is available
        nsync_cv_wait(&chan->cond_empty, &chan->mutex);
    }

    if (chan->closed && chan->size == 0) {
        nsync_mu_unlock(&chan->mutex);
        return -1; // Channel is closed and empty
    }

    // Get data from the buffer
    *data = chan->buffer[chan->out];
    chan->out = (chan->out + 1) % chan->capacity;
    chan->size--;

    // Signal senders that space is available
    nsync_cv_signal(&chan->cond_full);

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
    }

    nsync_mu_unlock(&chan->mutex);
}
