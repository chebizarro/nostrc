#include <stdio.h>

#include "go.h"

int main(void) {
    GoContext *ctx = go_context_background();
    GoChannel *chan = go_channel_create(1);
    int value = 42;
    int result = 0;

    if (!ctx || !chan) {
        fprintf(stderr, "failed to create background context or channel\n");
        result = 1;
        goto cleanup;
    }

    if (go_context_is_canceled(ctx)) {
        fprintf(stderr, "background context unexpectedly reported canceled\n");
        result = 2;
        goto cleanup;
    }

    if (go_channel_send_with_context(chan, &value, ctx) != 0) {
        fprintf(stderr, "send with background context unexpectedly failed\n");
        result = 3;
    }

cleanup:
    if (chan)
        go_channel_free(chan);
    if (ctx)
        go_context_unref(ctx);
    return result;
}
