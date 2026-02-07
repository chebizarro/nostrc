#ifndef NOSTR_POOL_H
#define NOSTR_POOL_H

#include <glib-object.h>
#include <gio/gio.h>
#include "nostr_relay.h"

G_BEGIN_DECLS

/* Note: gnostr_pool_subscribe() will be added when nostrc-wjlt
 * (NostrSubscription lifecycle) is complete. */

/* Forward declarations to avoid include conflicts between core and GObject headers */
typedef struct NostrFilters NostrFilters;

/* Define GNostrPool GObject */
#define GNOSTR_TYPE_POOL (gnostr_pool_get_type())
G_DECLARE_FINAL_TYPE(GNostrPool, gnostr_pool, GNOSTR, POOL, GObject)

/**
 * GNostrPool:
 *
 * A GObject wrapper for managing multiple Nostr relay connections.
 * Replaces GnostrSimplePool with proper GObject properties, signals,
 * and async methods suitable for GIR/language bindings.
 *
 * ## Properties
 *
 * - #GNostrPool:relays - GListStore of GNostrRelay objects
 * - #GNostrPool:default-timeout - Default timeout in milliseconds
 *
 * ## Signals
 *
 * - #GNostrPool::relay-added - Emitted when a relay is added to the pool
 * - #GNostrPool::relay-removed - Emitted when a relay is removed from the pool
 * - #GNostrPool::relay-state-changed - Emitted when any relay's state changes
 *
 * Since: 1.0
 */

/**
 * gnostr_pool_new:
 *
 * Creates a new empty pool.
 *
 * Returns: (transfer full): a new #GNostrPool
 */
GNostrPool *gnostr_pool_new(void);

/* --- Relay Management --- */

/**
 * gnostr_pool_add_relay:
 * @self: a #GNostrPool
 * @url: the relay URL to add (e.g., "wss://relay.damus.io")
 *
 * Adds a relay to the pool by URL. If the relay is already in the pool,
 * this is a no-op. Creates a new GNostrRelay internally.
 * Emits the "relay-added" signal on success.
 *
 * Returns: (transfer none): the #GNostrRelay for this URL
 */
GNostrRelay *gnostr_pool_add_relay(GNostrPool *self, const gchar *url);

/**
 * gnostr_pool_add_relay_object:
 * @self: a #GNostrPool
 * @relay: (transfer none): a #GNostrRelay to add
 *
 * Adds an existing GNostrRelay to the pool. If a relay with the same URL
 * is already present, this is a no-op and returns FALSE.
 * Emits the "relay-added" signal on success.
 *
 * Returns: %TRUE if the relay was added, %FALSE if already present
 */
gboolean gnostr_pool_add_relay_object(GNostrPool *self, GNostrRelay *relay);

/**
 * gnostr_pool_remove_relay:
 * @self: a #GNostrPool
 * @url: the relay URL to remove
 *
 * Removes a relay from the pool by URL. Disconnects the relay.
 * Emits the "relay-removed" signal on success.
 *
 * Returns: %TRUE if the relay was found and removed, %FALSE otherwise
 */
gboolean gnostr_pool_remove_relay(GNostrPool *self, const gchar *url);

/**
 * gnostr_pool_get_relay:
 * @self: a #GNostrPool
 * @url: the relay URL to look up
 *
 * Gets a relay from the pool by URL.
 *
 * Returns: (transfer none) (nullable): the #GNostrRelay, or %NULL if not found
 */
GNostrRelay *gnostr_pool_get_relay(GNostrPool *self, const gchar *url);

/**
 * gnostr_pool_get_relays:
 * @self: a #GNostrPool
 *
 * Gets the GListStore backing the pool's relay list. The list model
 * contains GNostrRelay objects and can be used with GtkListView etc.
 *
 * Returns: (transfer none): a #GListStore of #GNostrRelay objects
 */
GListStore *gnostr_pool_get_relays(GNostrPool *self);

/**
 * gnostr_pool_get_relay_count:
 * @self: a #GNostrPool
 *
 * Returns: the number of relays in the pool
 */
guint gnostr_pool_get_relay_count(GNostrPool *self);

/**
 * gnostr_pool_sync_relays:
 * @self: a #GNostrPool
 * @urls: (array length=url_count): array of relay URLs
 * @url_count: number of URLs
 *
 * Synchronizes the pool relay set with the given URL list.
 * Adds new relays and removes relays not in the list.
 */
void gnostr_pool_sync_relays(GNostrPool *self, const gchar **urls, gsize url_count);

/* --- Async Query API --- */

/**
 * gnostr_pool_query_async:
 * @self: a #GNostrPool
 * @filters: (transfer none): filters for the query
 * @cancellable: (nullable): optional #GCancellable
 * @callback: (scope async): callback when query completes
 * @user_data: (closure): user data for @callback
 *
 * Asynchronously queries all connected relays with the given filters.
 * Results are collected until EOSE from all relays or timeout.
 */
void gnostr_pool_query_async(GNostrPool      *self,
                             NostrFilters     *filters,
                             GCancellable     *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer          user_data);

/**
 * gnostr_pool_query_finish:
 * @self: a #GNostrPool
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Finishes an asynchronous query operation.
 *
 * Returns: (element-type utf8) (transfer full) (nullable): a #GPtrArray
 *   of event JSON strings, or %NULL on error. Free with g_ptr_array_unref().
 */
GPtrArray *gnostr_pool_query_finish(GNostrPool    *self,
                                    GAsyncResult  *result,
                                    GError       **error);

/* --- Properties --- */

/**
 * gnostr_pool_get_default_timeout:
 * @self: a #GNostrPool
 *
 * Gets the default timeout in milliseconds for query operations.
 *
 * Returns: the timeout in milliseconds
 */
guint gnostr_pool_get_default_timeout(GNostrPool *self);

/**
 * gnostr_pool_set_default_timeout:
 * @self: a #GNostrPool
 * @timeout_ms: timeout in milliseconds (0 for no timeout)
 *
 * Sets the default timeout for query operations.
 */
void gnostr_pool_set_default_timeout(GNostrPool *self, guint timeout_ms);

/**
 * gnostr_pool_connect_all_async:
 * @self: a #GNostrPool
 * @cancellable: (nullable): optional #GCancellable
 * @callback: (scope async): callback when all connections complete
 * @user_data: (closure): user data for @callback
 *
 * Asynchronously connects to all relays in the pool.
 */
void gnostr_pool_connect_all_async(GNostrPool         *self,
                                   GCancellable       *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer            user_data);

/**
 * gnostr_pool_connect_all_finish:
 * @self: a #GNostrPool
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Finishes an asynchronous connect-all operation.
 *
 * Returns: %TRUE if all relays connected, %FALSE if any failed
 */
gboolean gnostr_pool_connect_all_finish(GNostrPool   *self,
                                        GAsyncResult *result,
                                        GError      **error);

/**
 * gnostr_pool_disconnect_all:
 * @self: a #GNostrPool
 *
 * Disconnects all relays in the pool.
 */
void gnostr_pool_disconnect_all(GNostrPool *self);

G_END_DECLS

#endif /* NOSTR_POOL_H */
