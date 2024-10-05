#ifndef CHANNEL_H
#define CHANNEL_H

#include <pthread.h>
#include "context.h"
#include "refptr.h"

typedef struct GoChannel {
    void **buffer;
    size_t capacity;
    size_t size;
    size_t in;
    size_t out;
    pthread_mutex_t mutex;
    pthread_cond_t cond_full;
    pthread_cond_t cond_empty;
} GoChannel;

GoChannel *go_channel_create(size_t capacity);
void go_channel_free(GoChannel *chan);
int go_channel_send(GoChannel *chan, void *data);
int go_channel_receive(GoChannel *chan, void **data);
int go_channel_send_with_context(GoChannel *chan, void *data, GoContext *ctx);
int go_channel_receive_with_context(GoChannel *chan, void **data, GoContext *ctx);

#endif // CHANNEL_H
