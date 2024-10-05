#ifndef NOSTR_RELAY_STORE_H
#define NOSTR_RELAY_STORE_H

#include <glib-object.h>
#include "relay_store.h"

/* Define NostrRelayStore GObject interface */
#define NOSTR_TYPE_RELAY_STORE (nostr_relay_store_get_type())
G_DECLARE_INTERFACE(NostrRelayStore, nostr_relay_store, NOSTR, RELAY_STORE, GObject)

struct _NostrRelayStoreInterface {
    GTypeInterface parent_interface;

    gboolean (*publish)(NostrRelayStore *self, NostrEvent *event, GError **error);
    gboolean (*query_sync)(NostrRelayStore *self, NostrFilter *filter, GPtrArray **events, GError **error);
};

/* Define NostrMultiStore GObject */
#define NOSTR_TYPE_MULTI_STORE (nostr_multi_store_get_type())
G_DECLARE_FINAL_TYPE(NostrMultiStore, nostr_multi_store, NOSTR, MULTI_STORE, GObject)

struct _NostrMultiStore {
    GObject parent_instance;
    MultiStore *multi;
};

NostrMultiStore *nostr_multi_store_new();
void nostr_multi_store_add_store(NostrMultiStore *self, NostrRelayStore *store);

#endif // NOSTR_RELAY_STORE_H