#ifndef NOSTR_RELAY_STORE_H
#define NOSTR_RELAY_STORE_H

#include <glib-object.h>
#include "nostr-relay-store.h"

/* Define GNostrRelayStore GObject interface (G-prefixed to avoid core conflicts) */
#define G_NOSTR_TYPE_RELAY_STORE (g_nostr_relay_store_get_type())
G_DECLARE_INTERFACE(GNostrRelayStore, g_nostr_relay_store, G_NOSTR, RELAY_STORE, GObject)

struct _GNostrRelayStoreInterface {
    GTypeInterface parent_interface;

    gboolean (*publish)(GNostrRelayStore *self, NostrEvent *event, GError **error);
    gboolean (*query_sync)(GNostrRelayStore *self, NostrFilter *filter, GPtrArray **events, GError **error);
};

/* Define GNostrMultiStore GObject (G-prefixed to avoid core conflicts) */
#define G_NOSTR_TYPE_MULTI_STORE (g_nostr_multi_store_get_type())
G_DECLARE_FINAL_TYPE(GNostrMultiStore, g_nostr_multi_store, G_NOSTR, MULTI_STORE, GObject)

struct _GNostrMultiStore {
    GObject parent_instance;
    NostrMultiStore *multi;
};

GNostrMultiStore *g_nostr_multi_store_new(void);
void g_nostr_multi_store_add_store(GNostrMultiStore *self, GNostrRelayStore *store);

#endif // NOSTR_RELAY_STORE_H