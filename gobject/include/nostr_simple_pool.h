#ifndef NOSTR_SIMPLE_POOL_H
#define NOSTR_SIMPLE_POOL_H

#include <glib-object.h>
#include <gio/gio.h>
#include "nostr-simple-pool.h"
#include "nostr_relay.h"

/* Define GnostrSimplePool GObject (wrapper around core NostrSimplePool) */
#define GNOSTR_TYPE_SIMPLE_POOL (gnostr_simple_pool_get_type())
G_DECLARE_FINAL_TYPE(GnostrSimplePool, gnostr_simple_pool, GNOSTR, SIMPLE_POOL, GObject)

struct _GnostrSimplePool {
    GObject parent_instance;
    NostrSimplePool *pool; /* core handle */
};

/* GObject convenience API (prefixed with gnostr_ to avoid clashes with core
 * libnostr C API which uses nostr_simple_pool_*). */
GnostrSimplePool *gnostr_simple_pool_new(void);
void gnostr_simple_pool_add_relay(GnostrSimplePool *self, NostrRelay *relay);
GPtrArray *gnostr_simple_pool_query_sync(GnostrSimplePool *self, NostrFilter *filter, GError **error);

/* Async API for live subscribe-many and one-off backfill */
void gnostr_simple_pool_subscribe_many_async(GnostrSimplePool *self,
                                             const char **urls,
                                             size_t url_count,
                                             NostrFilters *filters,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback cb,
                                             gpointer user_data);
gboolean gnostr_simple_pool_subscribe_many_finish(GnostrSimplePool *self,
                                                  GAsyncResult *res,
                                                  GError **error);

void gnostr_simple_pool_backfill_async(GnostrSimplePool *self,
                                       const char **urls,
                                       size_t url_count,
                                       NostrFilters *filters,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback cb,
                                       gpointer user_data);
GPtrArray *gnostr_simple_pool_backfill_finish(GnostrSimplePool *self,
                                              GAsyncResult *res,
                                              GError **error);

#endif // NOSTR_SIMPLE_POOL_H