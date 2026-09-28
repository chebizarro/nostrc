#ifndef GH_RELAY_GNOSTR_WRITE_H
#define GH_RELAY_GNOSTR_WRITE_H

#include <gio/gio.h>
#include <nostr-gobject-1.0/nostr_relay.h>

G_BEGIN_DECLS

/* Internal to the GNostrRelay transports. ["<type>",<event_json>], or NULL
 * if the event JSON cannot be parsed or re-serialized. */
gchar *gh_relay_gnostr_event_frame(const gchar *type, const gchar *event_json);

/* Writes one frame on @relay's current, established core connection from a
 * worker thread (the libnostr write confirmation blocks) and completes on the
 * thread-default context of the caller. Fails when the connection is not
 * established, the write fails, or it is not confirmed within 5 s. */
void gh_relay_gnostr_write_async(GNostrRelay *relay, const gchar *frame,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data);
gboolean gh_relay_gnostr_write_finish(GAsyncResult *result, GError **error);

G_END_DECLS
#endif
