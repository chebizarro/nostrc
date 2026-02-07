#include "nostr_relay_store.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include <glib.h>

/* GNostrRelayStore interface implementation */
G_DEFINE_INTERFACE(GNostrRelayStore, g_nostr_relay_store, G_TYPE_OBJECT)

static void g_nostr_relay_store_default_init(GNostrRelayStoreInterface *iface) {
    /* Provide default implementation if necessary */
}

/* GNostrMultiStore GObject implementation */
G_DEFINE_TYPE(GNostrMultiStore, g_nostr_multi_store, G_TYPE_OBJECT)

static void g_nostr_multi_store_finalize(GObject *object) {
    GNostrMultiStore *self = G_NOSTR_MULTI_STORE(object);
    if (self->multi) {
        nostr_multi_store_free(self->multi);
    }
    G_OBJECT_CLASS(g_nostr_multi_store_parent_class)->finalize(object);
}

static void g_nostr_multi_store_class_init(GNostrMultiStoreClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = g_nostr_multi_store_finalize;
}

static void g_nostr_multi_store_init(GNostrMultiStore *self) {
    self->multi = nostr_multi_store_new(0);
}

GNostrMultiStore *g_nostr_multi_store_new(void) {
    return g_object_new(G_NOSTR_TYPE_MULTI_STORE, NULL);
}

void g_nostr_multi_store_add_store(GNostrMultiStore *self, GNostrRelayStore *store) {
    g_return_if_fail(G_NOSTR_IS_RELAY_STORE(store));
    /* append to underlying array */
    size_t idx = self->multi->stores_count++;
    self->multi->stores = g_realloc(self->multi->stores, self->multi->stores_count * sizeof(NostrRelayStore *));
    self->multi->stores[idx] = (NostrRelayStore *)store; /* bridge cast to core vtable */
}