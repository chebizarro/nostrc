#include "nostr_relay_store.h"
#include "nostr_event.h"
#include "nostr_filter.h"
#include <glib.h>

/* NostrRelayStore interface implementation */
G_DEFINE_INTERFACE(NostrRelayStore, nostr_relay_store, G_TYPE_OBJECT)

static void nostr_relay_store_default_init(NostrRelayStoreInterface *iface) {
    /* Provide default implementation if necessary */
}

/* NostrMultiStore GObject implementation */
G_DEFINE_TYPE(NostrMultiStore, nostr_multi_store, G_TYPE_OBJECT)

static void nostr_multi_store_finalize(GObject *object) {
    NostrMultiStore *self = NOSTR_MULTI_STORE(object);
    if (self->multi) {
        multi_store_free(self->multi);
    }
    G_OBJECT_CLASS(nostr_multi_store_parent_class)->finalize(object);
}

static void nostr_multi_store_class_init(NostrMultiStoreClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = nostr_multi_store_finalize;
}

static void nostr_multi_store_init(NostrMultiStore *self) {
    self->multi = multi_store_new();
}

NostrMultiStore *nostr_multi_store_new() {
    return g_object_new(NOSTR_TYPE_MULTI_STORE, NULL);
}

void nostr_multi_store_add_store(NostrMultiStore *self, NostrRelayStore *store) {
    g_return_if_fail(NOSTR_IS_RELAY_STORE(store));
    multi_store_add_store(self->multi, store);
}