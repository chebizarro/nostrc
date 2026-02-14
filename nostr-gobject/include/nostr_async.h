#ifndef NOSTR_ASYNC_H
#define NOSTR_ASYNC_H

#include <glib-object.h>
#include <gio/gio.h>
#include "nostr_relay.h"

G_BEGIN_DECLS

/**
 * SECTION:nostr_async
 * @title: Async Relay Operations
 * @short_description: Deprecated async API for relay operations
 *
 * These functions provide backward-compatible async operations.
 * New code should use gnostr_relay_connect_async() and related
 * functions directly from nostr_relay.h.
 */

/**
 * nostr_relay_connect_async:
 * @self: a #GNostrRelay
 * @cancellable: (nullable): optional #GCancellable object, %NULL to ignore
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied
 * @user_data: (closure): the data to pass to the callback function
 *
 * Asynchronously connects to the relay.
 *
 * Deprecated: Use gnostr_relay_connect_async() instead.
 */
void nostr_relay_connect_async(GNostrRelay *self,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);

/**
 * nostr_relay_connect_finish:
 * @self: a #GNostrRelay
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Finishes an asynchronous relay connection.
 *
 * Returns: %TRUE if successful, %FALSE otherwise.
 *
 * Deprecated: Use gnostr_relay_connect_finish() instead.
 */
gboolean nostr_relay_connect_finish(GNostrRelay *self,
                                    GAsyncResult *result,
                                    GError **error);

/**
 * nostr_relay_publish_async:
 * @self: a #GNostrRelay
 * @event: a #NostrEvent
 * @cancellable: (nullable): optional #GCancellable object, %NULL to ignore
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied
 * @user_data: (closure): the data to pass to the callback function
 *
 * Asynchronously publishes an event to the relay.
 */
void nostr_relay_publish_async(GNostrRelay *self,
                               NostrEvent *event,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);

/**
 * nostr_relay_publish_finish:
 * @self: a #GNostrRelay
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Finishes an asynchronous event publication.
 *
 * Returns: %TRUE if successful, %FALSE otherwise.
 */
gboolean nostr_relay_publish_finish(GNostrRelay *self,
                                    GAsyncResult *result,
                                    GError **error);

/**
 * nostr_relay_query_sync_async:
 * @self: a #GNostrRelay
 * @filter: a #NostrFilter
 * @cancellable: (nullable): optional #GCancellable object, %NULL to ignore
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied
 * @user_data: (closure): the data to pass to the callback function
 *
 * Asynchronously queries events from the relay.
 *
 * Deprecated: Use subscription-based API instead.
 */
void nostr_relay_query_sync_async(GNostrRelay *self,
                                  NostrFilter *filter,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);

/**
 * nostr_relay_query_sync_finish:
 * @self: a #GNostrRelay
 * @result: a #GAsyncResult
 * @error: (nullable): return location for a #GError, or %NULL
 *
 * Finishes an asynchronous event query.
 *
 * Returns: (element-type NostrEvent) (transfer full): a list of events if successful, %NULL otherwise.
 *
 * Deprecated: Use subscription-based API instead.
 */
GPtrArray *nostr_relay_query_sync_finish(GNostrRelay *self,
                                         GAsyncResult *result,
                                         GError **error);

G_END_DECLS

#endif /* NOSTR_ASYNC_H */
