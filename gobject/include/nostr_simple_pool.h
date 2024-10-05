#ifndef NOSTR_SIMPLE_POOL_H
#define NOSTR_SIMPLE_POOL_H

#include <glib-object.h>
#include "simplepool.h"

/* Define NostrSimplePool GObject */
#define NOSTR_TYPE_SIMPLE_POOL (nostr_simple_pool_get_type())
G_DECLARE_FINAL_TYPE(NostrSimplePool, nostr_simple_pool, NOSTR, SIMPLE_POOL, GObject)

struct _NostrSimplePool {
    GObject parent_instance;
    SimplePool *pool;
};

NostrSimplePool *nostr_simple_pool_new();
void nostr_simple_pool_add_relay(NostrSimplePool *self, NostrRelay *relay);
GPtrArray *nostr_simple_pool_query_sync(NostrSimplePool *self, NostrFilter *filter, GError **error);

#endif // NOSTR_SIMPLE_POOL_H