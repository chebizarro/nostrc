/**
 * connect_publish.c - Example: connect to a relay and publish an event
 *
 * Demonstrates GNostrRelay async connection and GNostrEvent creation.
 */
#include <glib.h>
#include <nostr-gobject-1.0/nostr-gobject.h>

static void
on_connect(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
    GNostrRelay *relay = GNOSTR_RELAY(source_object);
    GMainLoop *loop = user_data;
    g_autoptr(GError) error = NULL;

    if (!nostr_relay_connect_finish(relay, result, &error)) {
        g_printerr("Failed to connect: %s\n", error->message);
        g_main_loop_quit(loop);
        return;
    }

    g_print("Connected to relay: %s\n", gnostr_relay_get_url(relay));

    /* Create and configure an event */
    g_autoptr(GNostrEvent) event = gnostr_event_new();
    gnostr_event_set_kind(event, NOSTR_EVENT_KIND_TEXT_NOTE);
    gnostr_event_set_content(event, "Hello, Nostr!");

    /* Publish the event (would need signing with a real key first) */
    nostr_relay_publish_async(relay, NULL, NULL, NULL, NULL);

    g_main_loop_quit(loop);
}

int
main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    GNostrRelay *relay = gnostr_relay_new("wss://relay.damus.io");

    gnostr_relay_connect_async(relay, NULL, on_connect, loop);

    g_main_loop_run(loop);

    g_object_unref(relay);
    g_main_loop_unref(loop);
    return 0;
}
