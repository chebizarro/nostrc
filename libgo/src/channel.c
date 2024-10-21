#include "channel.h"
#include <stdlib.h>
#include <stdio.h>

GoChannel *go_channel_create(size_t capacity) {
    GoChannel *chan = malloc(sizeof(GoChannel));
    chan->buffer = malloc(sizeof(void *) * capacity);
    chan->capacity = capacity;
    chan->size = 0;
    chan->in = 0;
    chan->out = 0;
    chan->closed = 0;
    nsync_mu_init(&chan->mutex);
    nsync_cv_init(&chan->cond_full);
    nsync_cv_init(&chan->cond_empty);
    return chan;
}

void go_channel_free(GoChannel *chan) {
    if (chan == NULL) {
        return;  // Avoid freeing a NULL channel
    }

    nsync_mu_lock(&chan->mutex);
    // Free the internal buffer and other resources
    if (chan->buffer) {
        free(chan->buffer);
        chan->buffer = NULL;
    }
    nsync_mu_unlock(&chan->mutex);
    
    // Free the channel structure
    free(chan);
}

int go_channel_send(GoChannel *chan, void *data) {
    if (!chan) {
        fprintf(stderr, "Error: Attempting to send data to a NULL channel\n");
        return -1;
    }

    nsync_mu_lock(&chan->mutex);
    if (chan->closed) {
        nsync_mu_unlock(&chan->mutex);
        return -1;
    }
    while (chan->size == chan->capacity && !chan->closed) {
        nsync_cv_wait(&chan->cond_full, &chan->mutex);
    }
    if (chan->closed) {
        nsync_mu_unlock(&chan->mutex);
        return -1;
    }
    chan->buffer[chan->in] = data;
    chan->in = (chan->in + 1) % chan->capacity;
    chan->size++;
    nsync_cv_signal(&chan->cond_empty);
    nsync_mu_unlock(&chan->mutex);
    return 0;
}

int go_channel_receive(GoChannel *chan, void **data) {
    nsync_mu_lock(&chan->mutex);
    while (chan->size == 0 && !chan->closed) {
        nsync_cv_wait(&chan->cond_empty, &chan->mutex);
    }
    if (chan->size == 0 && chan->closed) {
        nsync_mu_unlock(&chan->mutex);
        return -1;
    }
    *data = chan->buffer[chan->out];
    chan->out = (chan->out + 1) % chan->capacity;
    chan->size--;
    nsync_cv_signal(&chan->cond_full);
    nsync_mu_unlock(&chan->mutex);
    return 0;
}

int go_channel_send_with_context(GoChannel *chan, void *data, GoContext *ctx) {
    nsync_mu_lock(&chan->mutex);
    while (chan->size == chan->capacity && !chan->closed) {
        if (ctx && ctx->vtable->is_canceled(ctx)) {
            nsync_mu_unlock(&chan->mutex);
            return -1; // Canceled
        }
        nsync_cv_wait(&chan->cond_full, &chan->mutex);
    }
    if (chan->size == 0 && chan->closed) {
        nsync_mu_unlock(&chan->mutex);
        return -1;
    }
    chan->buffer[chan->in] = data;
    chan->in = (chan->in + 1) % chan->capacity;
    chan->size++;
    nsync_cv_signal(&chan->cond_empty);
    nsync_mu_unlock(&chan->mutex);
    return 0;
}

int go_channel_receive_with_context(GoChannel *chan, void **data, GoContext *ctx) {
    nsync_mu_lock(&chan->mutex);
    while (chan->size == 0 && !chan->closed) {
        if (ctx && ctx->vtable->is_canceled(ctx)) {
            nsync_mu_unlock(&chan->mutex);
            return -1;
        }
        nsync_cv_wait(&chan->cond_empty, &chan->mutex);
    }
    if (chan->size == 0 && chan->closed) {
        nsync_mu_unlock(&chan->mutex);
        return -1;
    }
    *data = chan->buffer[chan->out];
    chan->out = (chan->out + 1) % chan->capacity;
    chan->size--;
    nsync_cv_signal(&chan->cond_full);
    nsync_mu_unlock(&chan->mutex);
    return 0;
}

void go_channel_close(GoChannel *chan) {
    nsync_mu_lock(&chan->mutex);

    if (!chan->closed) {
        chan->closed = 1;                      // Mark the channel as closed
        nsync_cv_broadcast(&chan->cond_empty); // Wake up any waiting receivers
    }

    nsync_mu_unlock(&chan->mutex);
}
