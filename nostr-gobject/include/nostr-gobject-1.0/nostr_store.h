#ifndef GNOSTR_STORE_H
#define GNOSTR_STORE_H

#include <glib-object.h>
#include "nostr-filter.h"

G_BEGIN_DECLS

/* Forward declarations */
typedef struct _GNostrEvent GNostrEvent;

/* ============ Shared types for GNostrStore API ============ */

/**
 * GNostrNoteCounts:
 * @total_reactions: Total reaction count
 * @direct_replies: Direct reply count
 * @thread_replies: Thread reply count (includes nested)
 * @reposts: Repost count
 * @quotes: Quote count
 *
 * Per-note count structure for metadata read/write.
 */
typedef struct {
    guint total_reactions;
    guint direct_replies;
    guint thread_replies;
    guint reposts;
    guint quotes;
} GNostrNoteCounts;

/**
 * GNostrZapStats:
 * @zap_count: Number of zaps received
 * @total_msat: Total amount in millisatoshis
 *
 * Per-event zap statistics.
 */
typedef struct {
    guint zap_count;
    gint64 total_msat;
} GNostrZapStats;

/* ============ GNostrStore GInterface ============ */

#define GNOSTR_TYPE_STORE (gnostr_store_get_type())
G_DECLARE_INTERFACE(GNostrStore, gnostr_store, GNOSTR, STORE, GObject)

/**
 * GNostrStoreInterface:
 * @save_event: Save an event to the store
 * @query: Query events matching a filter
 * @delete_event: Delete an event by ID
 * @count: Count events matching a filter
 * @get_note_by_id: Get note JSON by hex event ID
 * @get_note_by_key: Get note JSON by internal store key
 * @get_profile_by_pubkey: Get profile JSON by hex pubkey
 * @text_search: Full-text search for notes
 * @search_profile: Search for profiles by name/display_name
 * @subscribe: Subscribe to notes matching a filter
 * @unsubscribe: Cancel a subscription
 * @poll_notes: Poll for new note keys from a subscription
 * @get_note_counts: Read pre-computed note metadata counts
 * @write_note_counts: Write/update note metadata counts
 * @count_reactions_batch: Batch count reactions for multiple events
 * @get_zap_stats_batch: Batch get zap stats for multiple events
 *
 * Interface for Nostr event storage backends.
 *
 * Since: 0.1
 */
struct _GNostrStoreInterface {
    GTypeInterface parent_interface;

    /* Core CRUD (Phase 1) */
    gboolean    (*save_event)    (GNostrStore *self, GNostrEvent *event, GError **error);
    GPtrArray  *(*query)         (GNostrStore *self, NostrFilter *filter, GError **error);
    gboolean    (*delete_event)  (GNostrStore *self, const gchar *event_id, GError **error);
    gint        (*count)         (GNostrStore *self, NostrFilter *filter, GError **error);

    /* Note retrieval */
    gchar      *(*get_note_by_id)  (GNostrStore *self, const gchar *id_hex, GError **error);
    gchar      *(*get_note_by_key) (GNostrStore *self, guint64 note_key, GError **error);

    /* Profile operations */
    gchar      *(*get_profile_by_pubkey) (GNostrStore *self, const gchar *pubkey_hex, GError **error);

    /* Search */
    GPtrArray  *(*text_search)    (GNostrStore *self, const gchar *query, gint limit, GError **error);
    GPtrArray  *(*search_profile) (GNostrStore *self, const gchar *query, gint limit, GError **error);

    /* Reactive store */
    guint64     (*subscribe)   (GNostrStore *self, const gchar *filter_json);
    void        (*unsubscribe) (GNostrStore *self, guint64 subid);
    gint        (*poll_notes)  (GNostrStore *self, guint64 subid, guint64 *note_keys, gint capacity);

    /* Note metadata */
    gboolean    (*get_note_counts)   (GNostrStore *self, const gchar *id_hex, GNostrNoteCounts *out);
    gboolean    (*write_note_counts) (GNostrStore *self, const gchar *id_hex, const GNostrNoteCounts *counts);

    /* Batch operations (NIP-25/57) */
    GHashTable *(*count_reactions_batch) (GNostrStore *self, const gchar * const *event_ids, guint n_ids);
    GHashTable *(*get_zap_stats_batch)   (GNostrStore *self, const gchar * const *event_ids, guint n_ids);
};

/* ============ Public interface methods ============ */

/* Core CRUD */
gboolean    gnostr_store_save_event   (GNostrStore *self, GNostrEvent *event, GError **error);
GPtrArray  *gnostr_store_query        (GNostrStore *self, NostrFilter *filter, GError **error);
gboolean    gnostr_store_delete_event (GNostrStore *self, const gchar *event_id, GError **error);
gint        gnostr_store_count        (GNostrStore *self, NostrFilter *filter, GError **error);

/**
 * gnostr_store_get_note_by_id:
 * @self: a #GNostrStore
 * @id_hex: 64-character hex event ID
 * @error: (nullable): return location for a #GError
 *
 * Gets an event as JSON by its hex ID.
 *
 * Returns: (transfer full) (nullable): event JSON string, or %NULL on error
 */
gchar      *gnostr_store_get_note_by_id (GNostrStore *self, const gchar *id_hex, GError **error);

/**
 * gnostr_store_get_note_by_key:
 * @self: a #GNostrStore
 * @note_key: internal store key for the note
 * @error: (nullable): return location for a #GError
 *
 * Gets an event as JSON by its internal store key.
 *
 * Returns: (transfer full) (nullable): event JSON string, or %NULL on error
 */
gchar      *gnostr_store_get_note_by_key (GNostrStore *self, guint64 note_key, GError **error);

/**
 * gnostr_store_get_profile_by_pubkey:
 * @self: a #GNostrStore
 * @pubkey_hex: 64-character hex public key
 * @error: (nullable): return location for a #GError
 *
 * Gets a profile as JSON by pubkey.
 *
 * Returns: (transfer full) (nullable): profile JSON string, or %NULL on error
 */
gchar      *gnostr_store_get_profile_by_pubkey (GNostrStore *self, const gchar *pubkey_hex, GError **error);

/**
 * gnostr_store_text_search:
 * @self: a #GNostrStore
 * @query: search query string
 * @limit: maximum number of results (0 for default)
 * @error: (nullable): return location for a #GError
 *
 * Full-text search for notes.
 *
 * Returns: (transfer full) (element-type utf8) (nullable):
 *   a #GPtrArray of JSON strings, or %NULL on error
 */
GPtrArray  *gnostr_store_text_search (GNostrStore *self, const gchar *query, gint limit, GError **error);

/**
 * gnostr_store_search_profile:
 * @self: a #GNostrStore
 * @query: search query string
 * @limit: maximum number of results
 * @error: (nullable): return location for a #GError
 *
 * Search for profiles by name/display_name.
 *
 * Returns: (transfer full) (element-type utf8) (nullable):
 *   a #GPtrArray of JSON strings, or %NULL on error
 */
GPtrArray  *gnostr_store_search_profile (GNostrStore *self, const gchar *query, gint limit, GError **error);

/**
 * gnostr_store_subscribe:
 * @self: a #GNostrStore
 * @filter_json: NIP-01 filter JSON string
 *
 * Subscribe to notes matching a filter.
 *
 * Returns: subscription ID (>0) or 0 on failure
 */
guint64     gnostr_store_subscribe (GNostrStore *self, const gchar *filter_json);

/**
 * gnostr_store_unsubscribe:
 * @self: a #GNostrStore
 * @subid: subscription ID from gnostr_store_subscribe()
 *
 * Cancel a subscription.
 */
void        gnostr_store_unsubscribe (GNostrStore *self, guint64 subid);

/**
 * gnostr_store_poll_notes:
 * @self: a #GNostrStore
 * @subid: subscription ID
 * @note_keys: (out) (array length=capacity): buffer for note keys
 * @capacity: size of the note_keys buffer
 *
 * Poll for new note keys from a subscription. Non-blocking.
 *
 * Returns: number of keys written, or 0 if none available
 */
gint        gnostr_store_poll_notes (GNostrStore *self, guint64 subid, guint64 *note_keys, gint capacity);

/**
 * gnostr_store_get_note_counts:
 * @self: a #GNostrStore
 * @id_hex: 64-character hex event ID
 * @out: (out): pre-computed count values
 *
 * Read pre-computed note metadata counts.
 *
 * Returns: %TRUE if counts were found, %FALSE otherwise
 */
gboolean    gnostr_store_get_note_counts (GNostrStore *self, const gchar *id_hex, GNostrNoteCounts *out);

/**
 * gnostr_store_write_note_counts:
 * @self: a #GNostrStore
 * @id_hex: 64-character hex event ID
 * @counts: the count values to store
 *
 * Write/update note metadata counts.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean    gnostr_store_write_note_counts (GNostrStore *self, const gchar *id_hex, const GNostrNoteCounts *counts);

/**
 * gnostr_store_count_reactions_batch:
 * @self: a #GNostrStore
 * @event_ids: (array length=n_ids): array of 64-char hex event IDs
 * @n_ids: number of event IDs
 *
 * Batch count reactions (kind 7) for multiple events.
 *
 * Returns: (transfer full): #GHashTable mapping event_id_hex -> GUINT_TO_POINTER(count).
 *   Only events with count > 0 appear. Caller must g_hash_table_unref().
 */
GHashTable *gnostr_store_count_reactions_batch (GNostrStore *self, const gchar * const *event_ids, guint n_ids);

/**
 * gnostr_store_get_zap_stats_batch:
 * @self: a #GNostrStore
 * @event_ids: (array length=n_ids): array of 64-char hex event IDs
 * @n_ids: number of event IDs
 *
 * Batch get zap stats for multiple events.
 *
 * Returns: (transfer full): #GHashTable mapping event_id_hex -> #GNostrZapStats* (owned).
 *   Only events with zaps appear. Caller must g_hash_table_unref().
 */
GHashTable *gnostr_store_get_zap_stats_batch (GNostrStore *self, const gchar * const *event_ids, guint n_ids);

/* ============ GNostrNdbStore Implementation ============ */

#define GNOSTR_TYPE_NDB_STORE (gnostr_ndb_store_get_type())
G_DECLARE_FINAL_TYPE(GNostrNdbStore, gnostr_ndb_store, GNOSTR, NDB_STORE, GObject)

/**
 * gnostr_ndb_store_new:
 *
 * Creates a new NDB-backed store instance.
 * The underlying NDB database must already be initialized
 * via storage_ndb_init() before using this store.
 *
 * Returns: (transfer full): a new #GNostrNdbStore
 */
GNostrNdbStore *gnostr_ndb_store_new(void);

G_END_DECLS

#endif /* GNOSTR_STORE_H */
