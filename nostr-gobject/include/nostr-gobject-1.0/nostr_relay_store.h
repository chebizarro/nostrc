#ifndef GNOSTR_RELAY_STORE_H
#define GNOSTR_RELAY_STORE_H

#include <gio/gio.h>
#include <glib-object.h>
#include "nostr-relay-store.h"

/* Define GNostrRelayStore GObject interface (G-prefixed to avoid core conflicts) */
#define GNOSTR_TYPE_RELAY_STORE (gnostr_relay_store_get_type())
G_DECLARE_INTERFACE(GNostrRelayStore, gnostr_relay_store, GNOSTR, RELAY_STORE, GObject)

struct _GNostrRelayStoreInterface {
    GTypeInterface parent_interface;

    gboolean (*publish)(GNostrRelayStore *self, NostrEvent *event, GError **error);
    gboolean (*query_sync)(GNostrRelayStore *self, NostrFilter *filter, GPtrArray **events, GError **error);
    void     (*query_async)(GNostrRelayStore *self, NostrFilter *filter,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data);
    GPtrArray *(*query_finish)(GNostrRelayStore *self, GAsyncResult *result, GError **error);
};

gboolean   gnostr_relay_store_publish(GNostrRelayStore *self,
                                       NostrEvent *event,
                                       GError **error);
gboolean   gnostr_relay_store_query_sync(GNostrRelayStore *self,
                                          NostrFilter *filter,
                                          GPtrArray **events,
                                          GError **error);
void       gnostr_relay_store_query_async(GNostrRelayStore *self,
                                          NostrFilter *filter,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);
GPtrArray *gnostr_relay_store_query_finish(GNostrRelayStore *self,
                                           GAsyncResult *result,
                                           GError **error);

/* Define GNostrMultiStore GObject (G-prefixed to avoid core conflicts) */
#define GNOSTR_TYPE_MULTI_STORE (gnostr_multi_store_get_type())
G_DECLARE_FINAL_TYPE(GNostrMultiStore, gnostr_multi_store, GNOSTR, MULTI_STORE, GObject)

struct _GNostrMultiStore {
    GObject parent_instance;
    GPtrArray *stores; /* GNostrRelayStore*, strong refs */
};

GNostrMultiStore *gnostr_multi_store_new(void);
void gnostr_multi_store_add_store(GNostrMultiStore *self, GNostrRelayStore *store);

#endif // GNOSTR_RELAY_STORE_H