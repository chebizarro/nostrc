#ifndef NOSTR_ASYNC_H
#define NOSTR_ASYNC_H

#include <glib-object.h>
#include "nostr_relay.h"

/**
 * nostr_relay_connect_async:
 * @self: a #NostrRelay.
 * @cancellable: (nullable): optional #GCancellable object, %NULL to ignore.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to the callback function.
 *
 * Asynchronously connects to the relay.
 */
void nostr_relay_connect_async(NostrRelay *self,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);

/**
 * nostr_relay_connect_finish:
 * @self: a #NostrRelay.
 * @result: a #GAsyncResult.
 * @error: (nullable): return location for a #GError, or %NULL.
 *
 * Finishes an asynchronous relay connection.
 *
 * Returns: %TRUE if successful, %FALSE otherwise.
 */
gboolean nostr_relay_connect_finish(NostrRelay *self,
                                    GAsyncResult *result,
                                    GError **error);

/**
 * nostr_relay_publish_async:
 * @self: a #NostrRelay.
 * @event: a #NostrEvent.
 * @cancellable: (nullable): optional #GCancellable object, %NULL to ignore.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to the callback function.
 *
 * Asynchronously publishes an event to the relay.
 */
void nostr_relay_publish_async(NostrRelay *self,
                               NostrEvent *event,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data);

/**
 * nostr_relay_publish_finish:
 * @self: a #NostrRelay.
 * @result: a #GAsyncResult.
 * @error: (nullable): return location for a #GError, or %NULL.
 *
 * Finishes an asynchronous event publication.
 *
 * Returns: %TRUE if successful, %FALSE otherwise.
 */
gboolean nostr_relay_publish_finish(NostrRelay *self,
                                    GAsyncResult *result,
                                    GError **error);

/**
 * nostr_relay_query_sync_async:
 * @self: a #NostrRelay.
 * @filter: a #NostrFilter.
 * @cancellable: (nullable): optional #GCancellable object, %NULL to ignore.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to the callback function.
 *
 * Asynchronously queries events from the relay.
 */
void nostr_relay_query_sync_async(NostrRelay *self,
                                  NostrFilter *filter,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);

/**
 * nostr_relay_query_sync_finish:
 * @self: a #NostrRelay.
 * @result: a #GAsyncResult.
 * @error: (nullable): return location for a #GError, or %NULL.
 *
 * Finishes an asynchronous event query.
 *
 * Returns: (element-type NostrEvent) (transfer full): a list of events if successful, %NULL otherwise.
 */
GPtrArray *nostr_relay_query_sync_finish(NostrRelay *self,
                                         GAsyncResult *result,
                                         GError **error);

#endif // NOSTR_ASYNC_H