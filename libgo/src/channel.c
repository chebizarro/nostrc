#include "channel.h"
#include <stdlib.h>

GoChannel *go_channel_create(size_t capacity) {
    GoChannel *chan = (GoChannel *)malloc(sizeof(GoChannel));
    chan->buffer = (void **)malloc(sizeof(void *) * capacity);
    chan->capacity = capacity;
    chan->size = 0;
    chan->in = 0;
    chan->out = 0;
    
    // Initialize the nsync mutex and condition variables
    nsync_mu_init(&chan->mutex);
    nsync_cv_init(&chan->cond_full);
    nsync_cv_init(&chan->cond_empty);
    
    return chan;
}

void go_channel_free(GoChannel *chan) {
    free(chan->buffer);
    free(chan);
}

int go_channel_send(GoChannel *chan, void *data) {
    nsync_mu_lock(&chan->mutex);
    while (chan->size == chan->capacity) {
        nsync_cv_wait(&chan->cond_full, &chan->mutex);
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
    while (chan->size == 0) {
        nsync_cv_wait(&chan->cond_empty, &chan->mutex);
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
    while (chan->size == chan->capacity) {
        if (ctx && ctx->vtable->is_canceled(ctx)) {
            nsync_mu_unlock(&chan->mutex);
            return -1; // Canceled
        }
        nsync_cv_wait(&chan->cond_full, &chan->mutex);
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
    while (chan->size == 0) {
        if (ctx && ctx->vtable->is_canceled(ctx)) {
            nsync_mu_unlock(&chan->mutex);
            return -1; // Canceled
        }
        nsync_cv_wait(&chan->cond_empty, &chan->mutex);
    }
    *data = chan->buffer[chan->out];
    chan->out = (chan->out + 1) % chan->capacity;
    chan->size--;
    nsync_cv_signal(&chan->cond_full);
    nsync_mu_unlock(&chan->mutex);
    return 0;
}
