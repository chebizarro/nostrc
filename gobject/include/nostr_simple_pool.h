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
    gboolean profile_fetch_in_progress; /* Prevent concurrent profile fetches */
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

/* One-shot query API */
void gnostr_simple_pool_query_single_async(GnostrSimplePool *self,
                                          const char **urls,
                                          size_t url_count,
                                          const NostrFilter *filter,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);

GPtrArray *gnostr_simple_pool_query_single_finish(GnostrSimplePool *self,
                                                 GAsyncResult *res,
                                                 GError **error);

/* Background paginator with interval (emits "events" signal with GPtrArray* batches).
 * Starts a background thread that repeatedly issues one-shot subscriptions using the
 * provided filter, advancing the filter's `until` based on the smallest created_at
 * seen in each page, and sleeping for `interval_ms` between pages. The thread exits
 * when no new (non-duplicate) events are observed in a page or when cancelled.
 *
 * The async setup completes immediately; connect to the "events" signal to receive
 * batches of NostrEvent* as they arrive. Call the _finish() in the async callback
 * to confirm setup success.
 */
void gnostr_simple_pool_paginate_with_interval_async(GnostrSimplePool *self,
                                                     const char **urls,
                                                     size_t url_count,
                                                     const NostrFilter *filter,
                                                     guint interval_ms,
                                                     GCancellable *cancellable,
                                                     GAsyncReadyCallback cb,
                                                     gpointer user_data);

gboolean gnostr_simple_pool_paginate_with_interval_finish(GnostrSimplePool *self,
                                                          GAsyncResult *res,
                                                          GError **error);

/* Demand-driven batch fetch of kind-0 profiles by authors. Collects all profile
 * events from provided relays until EOSE per relay and returns a GPtrArray of
 * serialized JSON strings (char*) representing events. The array elements must
 * be freed with g_free() by the caller. Results are deduplicated by event id. */
void gnostr_simple_pool_fetch_profiles_by_authors_async(GnostrSimplePool *self,
                                                        const char **urls,
                                                        size_t url_count,
                                                        const char *const *authors,
                                                        size_t author_count,
                                                        int limit,
                                                        GCancellable *cancellable,
                                                        GAsyncReadyCallback cb,
                                                        gpointer user_data);

GPtrArray *gnostr_simple_pool_fetch_profiles_by_authors_finish(GnostrSimplePool *self,
                                                               GAsyncResult *res,
                                                               GError **error);

/* Relay connection status check */
gboolean gnostr_simple_pool_is_relay_connected(GnostrSimplePool *self, const char *url);

/* Get list of relay URLs currently in the pool */
GPtrArray *gnostr_simple_pool_get_relay_urls(GnostrSimplePool *self);

#endif // NOSTR_SIMPLE_POOL_H