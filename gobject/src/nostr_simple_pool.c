#include "nostr_simple_pool.h"
#include "nostr_relay.h"
#include <glib.h>

/* NostrSimplePool GObject implementation */
G_DEFINE_TYPE(NostrSimplePool, nostr_simple_pool, G_TYPE_OBJECT)

static void nostr_simple_pool_finalize(GObject *object) {
    NostrSimplePool *self = NOSTR_SIMPLE_POOL(object);
    if (self->pool) {
        simple_pool_free(self->pool);
    }
    G_OBJECT_CLASS(nostr_simple_pool_parent_class)->finalize(object);
}

static void nostr_simple_pool_class_init(NostrSimplePoolClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = nostr_simple_pool_finalize;
}

static void nostr_simple_pool_init(NostrSimplePool *self) {
    self->pool = simple_pool_new();
}

NostrSimplePool *nostr_simple_pool_new() {
    return g_object_new(NOSTR_TYPE_SIMPLE_POOL, NULL);
}

void nostr_simple_pool_add_relay(NostrSimplePool *self, NostrRelay *relay) {
    simple_pool_add_relay(self->pool, relay->relay);
}

GPtrArray *nostr_simple_pool_query_sync(NostrSimplePool *self, NostrFilter *filter, GError **error) {
    return simple_pool_query_sync(self->pool, &filter->filter, error);
}