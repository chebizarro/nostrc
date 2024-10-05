#include "channel.h"
#include <stdlib.h>

GoChannel *go_channel_create(size_t capacity) {
    GoChannel *chan = (GoChannel *)malloc(sizeof(GoChannel));
    chan->buffer = (void **)malloc(sizeof(void *) * capacity);
    chan->capacity = capacity;
    chan->size = 0;
    chan->in = 0;
    chan->out = 0;
    pthread_mutex_init(&chan->mutex, NULL);
    pthread_cond_init(&chan->cond_full, NULL);
    pthread_cond_init(&chan->cond_empty, NULL);
    return chan;
}

void go_channel_free(GoChannel *chan) {
    free(chan->buffer);
    pthread_mutex_destroy(&chan->mutex);
    pthread_cond_destroy(&chan->cond_full);
    pthread_cond_destroy(&chan->cond_empty);
    free(chan);
}

int go_channel_send(GoChannel *chan, void *data) {
    pthread_mutex_lock(&chan->mutex);
    while (chan->size == chan->capacity) {
        pthread_cond_wait(&chan->cond_full, &chan->mutex);
    }
    chan->buffer[chan->in] = data;
    chan->in = (chan->in + 1) % chan->capacity;
    chan->size++;
    pthread_cond_signal(&chan->cond_empty);
    pthread_mutex_unlock(&chan->mutex);
    return 0;
}

int go_channel_receive(GoChannel *chan, void **data) {
    pthread_mutex_lock(&chan->mutex);
    while (chan->size == 0) {
        pthread_cond_wait(&chan->cond_empty, &chan->mutex);
    }
    *data = chan->buffer[chan->out];
    chan->out = (chan->out + 1) % chan->capacity;
    chan->size--;
    pthread_cond_signal(&chan->cond_full);
    pthread_mutex_unlock(&chan->mutex);
    return 0;
}

int go_channel_send_with_context(GoChannel *chan, void *data, GoContext *ctx) {
    pthread_mutex_lock(&chan->mutex);
    while (chan->size == chan->capacity) {
        if (ctx && ctx->vtable->is_canceled(ctx)) {
            pthread_mutex_unlock(&chan->mutex);
            return -1; // Canceled
        }
        pthread_cond_wait(&chan->cond_full, &chan->mutex);
    }
    chan->buffer[chan->in] = data;
    chan->in = (chan->in + 1) % chan->capacity;
    chan->size++;
    pthread_cond_signal(&chan->cond_empty);
    pthread_mutex_unlock(&chan->mutex);
    return 0;
}

int go_channel_receive_with_context(GoChannel *chan, void **data, GoContext *ctx) {
    pthread_mutex_lock(&chan->mutex);
    while (chan->size == 0) {
        if (ctx && ctx->vtable->is_canceled(ctx)) {
            pthread_mutex_unlock(&chan->mutex);
            return -1; // Canceled
        }
        pthread_cond_wait(&chan->cond_empty, &chan->mutex);
    }
    *data = chan->buffer[chan->out];
    chan->out = (chan->out + 1) % chan->capacity;
    chan->size--;
    pthread_cond_signal(&chan->cond_full);
    pthread_mutex_unlock(&chan->mutex);
    return 0;
}
