#ifndef NOSTR_RELAY_H
#define NOSTR_RELAY_H

#include <glib-object.h>
#include "relay.h"

/* Define NostrRelay GObject */
#define NOSTR_TYPE_RELAY (nostr_relay_get_type())
G_DECLARE_FINAL_TYPE(NostrRelay, nostr_relay, NOSTR, RELAY, GObject)

struct _NostrRelay {
    GObject parent_instance;
    Relay *relay;
};

enum {
    SIGNAL_CONNECTED,
    SIGNAL_DISCONNECTED,
    SIGNAL_EVENT_RECEIVED,
    SIGNAL_ERROR,
    NOSTR_RELAY_SIGNALS_COUNT
};

NostrRelay *nostr_relay_new(const gchar *url);
gboolean nostr_relay_connect(NostrRelay *self, GError **error);
gboolean nostr_relay_publish(NostrRelay *self, NostrEvent *event, GError **error);
GPtrArray *nostr_relay_query_sync(NostrRelay *self, NostrFilter *filter, GError **error);

#endif // NOSTR_RELAY_H