#include <glib.h>
#include "nostr_event.h"
#include "nostr_relay.h"
#include "nostr_async.h"

static void on_connect(GObject *source_object, GAsyncResult *result, gpointer user_data) {
    GError *error = NULL;
    NostrRelay *relay = NOSTR_RELAY(source_object);
    if (nostr_relay_connect_finish(relay, result, &error)) {
        g_print("Connected to relay\n");

        // Create an event
        NostrEvent *event = nostr_event_new();
        nostr_event_set_id(event, "example_id");
        nostr_event_set_pubkey(event, "example_pubkey");
        nostr_event_set_created_at(event, time(NULL));
        nostr_event_set_kind(event, KIND_TEXT_NOTE);
        nostr_event_set_content(event, "Hello, Nostr!");
        nostr_event_set_sig(event, "example_sig");

        // Publish the event
        nostr_relay_publish_async(relay, event, NULL, NULL, NULL);
    } else {
        g_printerr("Failed to connect: %s\n", error->message);
        g_error_free(error);
    }
}

int main(int argc, char *argv[]) {
    g_type_init();

    NostrRelay *relay = nostr_relay_new("wss://example.com");
    nostr_relay_connect_async(relay, NULL, on_connect, NULL);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    return 0;
}