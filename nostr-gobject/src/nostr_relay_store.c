#include "nostr_relay_store.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include <glib.h>

/* GNostrRelayStore interface implementation */
G_DEFINE_INTERFACE(GNostrRelayStore, gnostr_relay_store, G_TYPE_OBJECT)

static void gnostr_relay_store_default_init(GNostrRelayStoreInterface *iface) {
    /* Provide default implementation if necessary */
}

/* GNostrMultiStore GObject implementation */
G_DEFINE_TYPE(GNostrMultiStore, gnostr_multi_store, G_TYPE_OBJECT)

static void gnostr_multi_store_finalize(GObject *object) {
    GNostrMultiStore *self = GNOSTR_MULTI_STORE(object);
    if (self->multi) {
        nostr_multi_store_free(self->multi);
    }
    G_OBJECT_CLASS(gnostr_multi_store_parent_class)->finalize(object);
}

static void gnostr_multi_store_class_init(GNostrMultiStoreClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = gnostr_multi_store_finalize;
}

static void gnostr_multi_store_init(GNostrMultiStore *self) {
    self->multi = nostr_multi_store_new(0);
}

GNostrMultiStore *gnostr_multi_store_new(void) {
    return g_object_new(GNOSTR_TYPE_MULTI_STORE, NULL);
}

void gnostr_multi_store_add_store(GNostrMultiStore *self, GNostrRelayStore *store) {
    g_return_if_fail(GNOSTR_IS_RELAY_STORE(store));
    /* append to underlying array */
    size_t idx = self->multi->stores_count++;
    self->multi->stores = g_realloc(self->multi->stores, self->multi->stores_count * sizeof(NostrRelayStore *));
    self->multi->stores[idx] = (NostrRelayStore *)store; /* bridge cast to core vtable */
}