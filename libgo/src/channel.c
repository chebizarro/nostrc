#include "channel.h"
#include <stdio.h>
#include <stdlib.h>

/* Condition function to check if the channel has space */
int go_channel_has_space(const void *chan) {
    GoChannel *c = (GoChannel *)chan;
    return c->size < c->capacity;
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

    if (chan->closed) {
        nsync_mu_unlock(&chan->mutex);
        return -1; // Cannot send to a closed channel
    }

    // Wait for space in the buffer
    nsync_mu_wait(&chan->mutex, &go_channel_has_space, chan, NULL);

    // Add data to the buffer
    chan->buffer[chan->in] = data;
    chan->in = (chan->in + 1) % chan->capacity;
    chan->size++;

    nsync_mu_unlock(&chan->mutex);
    return 0;
}

/* Receive data from the channel */
int go_channel_receive(GoChannel *chan, void **data) {
    nsync_mu_lock(&chan->mutex);

    // Wait for data to be available
    nsync_mu_wait(&chan->mutex, &go_channel_has_data, chan, NULL);

    if (chan->closed && chan->size == 0) {
        nsync_mu_unlock(&chan->mutex);
        return -1; // Channel is closed and empty
    }

    // Get data from the buffer
    *data = chan->buffer[chan->out];
    chan->out = (chan->out + 1) % chan->capacity;
    chan->size--;

    nsync_mu_unlock(&chan->mutex);
    return 0;
}

/* Send data to the channel with cancellation context */
int go_channel_send_with_context(GoChannel *chan, void *data, GoContext *ctx) {
    nsync_mu_lock(&chan->mutex);

    while (chan->size == chan->capacity && !chan->closed) {
        if (ctx && ctx->vtable->is_canceled(ctx)) {
            nsync_mu_unlock(&chan->mutex);
            return -1; // Canceled
        }
        nsync_mu_wait_with_deadline(&chan->mutex, &go_channel_has_space, chan, NULL, nsync_time_no_deadline, NULL);
    }

    if (chan->closed) {
        nsync_mu_unlock(&chan->mutex);
        return -1; // Channel is closed
    }

    // Add data to the buffer
    chan->buffer[chan->in] = data;
    chan->in = (chan->in + 1) % chan->capacity;
    chan->size++;

    nsync_mu_unlock(&chan->mutex);
    return 0;
}

/* Receive data from the channel with cancellation context */
int go_channel_receive_with_context(GoChannel *chan, void **data, GoContext *ctx) {
    nsync_mu_lock(&chan->mutex);

    while (chan->size == 0 && !chan->closed) {
        if (ctx && ctx->vtable->is_canceled(ctx)) {
            nsync_mu_unlock(&chan->mutex);
            return -1; // Canceled
        }
        nsync_mu_wait_with_deadline(&chan->mutex, &go_channel_has_data, chan, NULL, nsync_time_no_deadline, NULL);
    }

    if (chan->closed && chan->size == 0) {
        nsync_mu_unlock(&chan->mutex);
        return -1; // Channel is closed and empty
    }

    // Get data from the buffer
    *data = chan->buffer[chan->out];
    chan->out = (chan->out + 1) % chan->capacity;
    chan->size--;

    nsync_mu_unlock(&chan->mutex);
    return 0;
}

/* Close the channel */
void go_channel_close(GoChannel *chan) {
    nsync_mu_lock(&chan->mutex);

    if (!chan->closed) {
        chan->closed = true; // Mark the channel as closed
    }

    nsync_mu_unlock(&chan->mutex);
    go_channel_free(chan);
}
