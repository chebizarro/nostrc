#ifndef NOSTR_RELAY_H
#define NOSTR_RELAY_H

#include <glib-object.h>
#include "nostr-relay.h"

/* Define NostrRelay GObject */
/* GLib wrapper type is prefixed with G to avoid clashing with core NostrRelay */
#define GNOSTR_TYPE_RELAY (gnostr_relay_get_type())
G_DECLARE_FINAL_TYPE(GNostrRelay, gnostr_relay, GNOSTR, RELAY, GObject)

struct _GNostrRelay {
    GObject parent_instance;
    NostrRelay *relay;
};

enum {
    SIGNAL_CONNECTED,
    SIGNAL_DISCONNECTED,
    SIGNAL_EVENT_RECEIVED,
    SIGNAL_ERROR,
    NOSTR_RELAY_SIGNALS_COUNT
};

/* GObject convenience API (prefixed with gnostr_ to avoid clashes with core
 * libnostr C API which uses nostr_relay_*). */
GNostrRelay *gnostr_relay_new(const gchar *url);
gboolean gnostr_relay_connect(GNostrRelay *self, GError **error);
gboolean gnostr_relay_publish(GNostrRelay *self, NostrEvent *event, GError **error);
GPtrArray *gnostr_relay_query_sync(GNostrRelay *self, NostrFilter *filter, GError **error);

#endif // NOSTR_RELAY_H