#ifndef GO_CHANNEL_H
#define GO_CHANNEL_H

#include "refptr.h"
#include <nsync.h>
#include "context.h"

typedef struct GoChannel {
    void **buffer;
    size_t capacity;
    size_t size;
    size_t in;
    size_t out;
    nsync_mu mutex;
    nsync_cv cond_full;
    nsync_cv cond_empty;
    int closed;
} GoChannel;

GoChannel *go_channel_create(size_t capacity);
void go_channel_free(GoChannel *chan);
int go_channel_send(GoChannel *chan, void *data);
int go_channel_receive(GoChannel *chan, void **data);
int go_channel_send_with_context(GoChannel *chan, void *data, GoContext *ctx);
int go_channel_receive_with_context(GoChannel *chan, void **data, GoContext *ctx);
void go_channel_close(GoChannel *chan);

#endif // GO_CHANNEL_H
