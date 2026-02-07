#ifndef GNOSTR_STORE_H
#define GNOSTR_STORE_H

#include <glib-object.h>
#include "nostr-filter.h"

G_BEGIN_DECLS

/* Forward declarations */
typedef struct _GNostrEvent GNostrEvent;

/* ============ GNostrStore GInterface ============ */

#define G_NOSTR_TYPE_STORE (g_nostr_store_get_type())
G_DECLARE_INTERFACE(GNostrStore, g_nostr_store, G_NOSTR, STORE, GObject)

/**
 * GNostrStoreInterface:
 * @save_event: Save an event to the store
 * @query: Query events matching a filter
 * @delete_event: Delete an event by ID
 * @count: Count events matching a filter
 *
 * Interface for Nostr event storage backends.
 *
 * Since: 0.1
 */
struct _GNostrStoreInterface {
    GTypeInterface parent_interface;

    gboolean (*save_event)(GNostrStore *self, GNostrEvent *event, GError **error);
    GPtrArray *(*query)(GNostrStore *self, NostrFilter *filter, GError **error);
    gboolean (*delete_event)(GNostrStore *self, const gchar *event_id, GError **error);
    gint (*count)(GNostrStore *self, NostrFilter *filter, GError **error);
};

/* Public interface methods */

/**
 * g_nostr_store_save_event:
 * @self: a #GNostrStore
 * @event: (transfer none): the event to save
 * @error: (nullable): return location for a #GError
 *
 * Saves an event to the store.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean g_nostr_store_save_event(GNostrStore *self, GNostrEvent *event, GError **error);

/**
 * g_nostr_store_query:
 * @self: a #GNostrStore
 * @filter: (transfer none): the filter to match against
 * @error: (nullable): return location for a #GError
 *
 * Queries the store for events matching the filter.
 *
 * Returns: (transfer full) (element-type GNostrEvent) (nullable):
 *   a #GPtrArray of #GNostrEvent, or %NULL on error
 */
GPtrArray *g_nostr_store_query(GNostrStore *self, NostrFilter *filter, GError **error);

/**
 * g_nostr_store_delete_event:
 * @self: a #GNostrStore
 * @event_id: the 64-character hex event ID to delete
 * @error: (nullable): return location for a #GError
 *
 * Deletes an event from the store by ID.
 * Not all backends support deletion.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean g_nostr_store_delete_event(GNostrStore *self, const gchar *event_id, GError **error);

/**
 * g_nostr_store_count:
 * @self: a #GNostrStore
 * @filter: (transfer none): the filter to match against
 * @error: (nullable): return location for a #GError
 *
 * Counts events matching the filter.
 *
 * Returns: the number of matching events, or -1 on error
 */
gint g_nostr_store_count(GNostrStore *self, NostrFilter *filter, GError **error);

/* ============ GNostrNdbStore Implementation ============ */

#define G_NOSTR_TYPE_NDB_STORE (g_nostr_ndb_store_get_type())
G_DECLARE_FINAL_TYPE(GNostrNdbStore, g_nostr_ndb_store, G_NOSTR, NDB_STORE, GObject)

/**
 * GNostrNdbStore:
 *
 * A #GNostrStore implementation backed by NostrDB (NDB).
 * Requires that storage_ndb_init() has already been called.
 *
 * Since: 0.1
 */

/**
 * g_nostr_ndb_store_new:
 *
 * Creates a new NDB-backed store instance.
 * The underlying NDB database must already be initialized
 * via storage_ndb_init() before using this store.
 *
 * Returns: (transfer full): a new #GNostrNdbStore
 */
GNostrNdbStore *g_nostr_ndb_store_new(void);

G_END_DECLS

#endif /* GNOSTR_STORE_H */
