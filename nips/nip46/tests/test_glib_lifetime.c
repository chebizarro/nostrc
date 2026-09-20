#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_client_g.h"
#include <gio/gio.h>
#include <assert.h>
#include <stdlib.h>

typedef struct {
    GMainLoop *loop;
    int called;
    int operation;
    int cancelled;
} Completion;

static void completed(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)source;
    Completion *completion = user_data;
    GError *error = NULL;
    char *value = completion->operation == 0
        ? nostr_nip46_client_sign_event_g_finish(result, &error)
        : completion->operation == 1
        ? nostr_nip46_client_connect_rpc_g_finish(result, &error)
        : nostr_nip46_client_get_public_key_rpc_g_finish(result, &error);
    assert(value == NULL);
    assert(error != NULL);
    if (completion->cancelled)
        assert(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
    free(value);
    g_error_free(error);
    completion->called++;
    g_main_loop_quit(completion->loop);
}

int main(void) {
    for (int i = 0; i < 300; i++) {
        Completion completion = { g_main_loop_new(NULL, FALSE), 0, i % 3, (i / 3) % 2 };
        NostrNip46Session *session = nostr_nip46_client_new();
        assert(session);
        GCancellable *cancel = g_cancellable_new();
        if (completion.cancelled) g_cancellable_cancel(cancel);
        if (completion.operation == 0)
            nostr_nip46_client_sign_event_g_async(
                session, "{\"kind\":1}", cancel, completed, &completion);
        else if (completion.operation == 1)
            nostr_nip46_client_connect_rpc_g_async(
                session, NULL, NULL, cancel, completed, &completion);
        else
            nostr_nip46_client_get_public_key_rpc_g_async(
                session, cancel, completed, &completion);
        g_object_unref(cancel);
        /* Drop the owner immediately. Task data must retain the session until
         * the worker and completion paths have both stopped using it. */
        nostr_nip46_session_free(session);
        g_main_loop_run(completion.loop);
        assert(completion.called == 1);
        g_main_loop_unref(completion.loop);
    }
    return 0;
}
