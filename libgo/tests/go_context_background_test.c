#include <stdio.h>
#include <assert.h>

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

    if (!go_context_done(ctx) || go_context_err(ctx) != NULL) {
        fprintf(stderr, "background context has invalid done/error state\n");
        result = 3;
        goto cleanup;
    }

    if (go_channel_send_with_context(chan, &value, ctx) != 0) {
        fprintf(stderr, "send with background context unexpectedly failed\n");
        result = 3;
    }

cleanup:
    if (chan)
        go_channel_free(chan);
    if (ctx) {
        go_context_ref(ctx);
        go_context_unref(ctx);
        if (go_context_is_canceled(ctx))
            result = 4;
        go_context_unref(ctx);
    }
    /* Exercise direct destruction as well as last-reference destruction. */
    go_context_free(go_context_background());

    GoContext *parent = go_context_background();
    CancelContextResult child = go_context_with_cancel(parent);
    if (go_context_is_canceled(child.context))
        result = 5;
    child.cancel(child.context);
    if (!go_context_is_canceled(child.context) || go_context_is_canceled(parent))
        result = 6;
    go_context_unref(child.context);
    go_context_unref(parent);
    /* A child owns its parent's lifetime, including through free's legacy API. */
    parent = go_context_background();
    child = go_context_with_cancel(parent);
    assert(atomic_load(&parent->refcount) == 2);
    go_context_free(parent);
    assert(atomic_load(&parent->refcount) == 1);
    assert(!go_context_is_canceled(child.context));
    child.cancel(child.context);
    assert(go_context_is_canceled(child.context));
    go_context_unref(child.context);

    ctx = go_context_background();
    go_context_ref(ctx);
    go_context_free(ctx);
    assert(atomic_load(&ctx->refcount) == 1);
    assert(!go_context_is_canceled(ctx));
    go_context_unref(ctx);
    return result;
}
