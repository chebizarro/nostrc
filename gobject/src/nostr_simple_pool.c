#include "nostr_simple_pool.h"
#include "nostr_relay.h"
#include <glib.h>

/* NostrSimplePool GObject implementation */
G_DEFINE_TYPE(NostrSimplePool, nostr_simple_pool, G_TYPE_OBJECT)

static void nostr_simple_pool_finalize(GObject *object) {
    NostrSimplePool *self = NOSTR_SIMPLE_POOL(object);
    if (self->pool) {
        nostr_simple_pool_free(self->pool);
    }
    G_OBJECT_CLASS(nostr_simple_pool_parent_class)->finalize(object);
}

static void nostr_simple_pool_class_init(NostrSimplePoolClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = nostr_simple_pool_finalize;
}

static void nostr_simple_pool_init(NostrSimplePool *self) {
    self->pool = nostr_simple_pool_new();
}

NostrSimplePool *gnostr_simple_pool_new(void) {
    return g_object_new(NOSTR_TYPE_SIMPLE_POOL, NULL);
}

void gnostr_simple_pool_add_relay(NostrSimplePool *self, NostrRelay *relay) {
    if (!self || !self->pool || !relay) return;
    const char *url = nostr_relay_get_url_const(relay);
    if (url) nostr_simple_pool_ensure_relay(self->pool, url);
}

GPtrArray *gnostr_simple_pool_query_sync(NostrSimplePool *self, NostrFilter *filter, GError **error) {
    (void)self; (void)filter;
    if (error) *error = g_error_new_literal(g_quark_from_static_string("nostr-simple-pool"), 1, "gnostr_simple_pool_query_sync is not implemented in this wrapper");
    return NULL;
}