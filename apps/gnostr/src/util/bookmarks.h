/**
 * NIP-51 Bookmark List Service
 *
 * Provides bookmark list management for the gnostr GTK app.
 * Handles loading/parsing kind 10003 events and managing bookmarks.
 *
 * Relay Sync (NIP-51):
 * - Fetches user's bookmark list from relays on login via gnostr_bookmarks_fetch_async()
 * - Merges remote bookmarks with local (prefers most recent by created_at)
 * - Publishes updated bookmark list when user adds/removes bookmarks
 * - Supports both public bookmarks (in tags) and private bookmarks (encrypted content)
 *
 * Note: Private bookmark encryption requires NIP-44 which is not yet implemented.
 * Private bookmarks are currently stored locally only.
 */

#ifndef GNOSTR_BOOKMARKS_H
#define GNOSTR_BOOKMARKS_H

#include <gio/gio.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

/* ---- Opaque Bookmark List Handle ---- */
typedef struct _GnostrBookmarks GnostrBookmarks;

/* ---- Initialization ---- */

/**
 * gnostr_bookmarks_get_default:
 *
 * Gets the singleton bookmark list instance for the app.
 * Creates it on first call. Thread-safe.
 *
 * Returns: (transfer none): the shared bookmark list instance
 */
GnostrBookmarks *gnostr_bookmarks_get_default(void);

/**
 * gnostr_bookmarks_shutdown:
 *
 * Releases the singleton instance. Call at app shutdown.
 */
void gnostr_bookmarks_shutdown(void);

/* ---- Loading ---- */

/**
 * gnostr_bookmarks_load_from_json:
 * @self: bookmark list instance
 * @event_json: JSON string of kind 10003 event
 *
 * Parses a kind 10003 bookmark list event and caches entries.
 * This is for loading from local storage or relay response.
 *
 * Returns: TRUE on success
 */
gboolean gnostr_bookmarks_load_from_json(GnostrBookmarks *self,
                                          const char *event_json);

/**
 * gnostr_bookmarks_fetch_async:
 * @self: bookmark list instance
 * @pubkey_hex: user's public key (64 hex chars)
 * @relays: (nullable): NULL-terminated array of relay URLs, or NULL to use configured relays
 * @callback: callback when fetch completes
 * @user_data: user data for callback
 *
 * Fetches the user's bookmark list (kind 10003) from relays asynchronously.
 * Uses SimplePool to query configured relays and merges results with local bookmarks.
 * Conflict resolution: prefers the most recent event by created_at timestamp.
 *
 * If @relays is NULL, uses the user's configured relay list from GSettings.
 */
typedef void (*GnostrBookmarksFetchCallback)(GnostrBookmarks *self,
                                              gboolean success,
                                              gpointer user_data);

void gnostr_bookmarks_fetch_async(GnostrBookmarks *self,
                                   const char *pubkey_hex,
                                   const char * const *relays,
                                   GnostrBookmarksFetchCallback callback,
                                   gpointer user_data);

/* ---- Query Functions ---- */

/**
 * gnostr_bookmarks_is_bookmarked:
 * @self: bookmark list instance
 * @event_id_hex: event ID to check (64 hex chars)
 *
 * Checks if an event is bookmarked.
 *
 * Returns: TRUE if bookmarked
 */
gboolean gnostr_bookmarks_is_bookmarked(GnostrBookmarks *self,
                                         const char *event_id_hex);

/* ---- Modification Functions ---- */

/**
 * gnostr_bookmarks_add:
 * @self: bookmark list instance
 * @event_id_hex: event ID to bookmark (64 hex chars)
 * @relay_hint: (nullable): optional relay URL hint
 * @is_private: TRUE to encrypt this entry
 *
 * Adds an event to the bookmark list (locally).
 * Call gnostr_bookmarks_save_async() to persist to relays.
 */
void gnostr_bookmarks_add(GnostrBookmarks *self,
                           const char *event_id_hex,
                           const char *relay_hint,
                           gboolean is_private);

/**
 * gnostr_bookmarks_remove:
 * @self: bookmark list instance
 * @event_id_hex: event ID to remove (64 hex chars)
 *
 * Removes an event from the bookmark list.
 */
void gnostr_bookmarks_remove(GnostrBookmarks *self,
                              const char *event_id_hex);

/**
 * gnostr_bookmarks_toggle:
 * @self: bookmark list instance
 * @event_id_hex: event ID to toggle (64 hex chars)
 * @relay_hint: (nullable): optional relay URL hint (for add)
 *
 * Toggles the bookmark state of an event.
 * If bookmarked, removes it. If not bookmarked, adds it (public).
 *
 * Returns: TRUE if the event is now bookmarked, FALSE if removed
 */
gboolean gnostr_bookmarks_toggle(GnostrBookmarks *self,
                                  const char *event_id_hex,
                                  const char *relay_hint);

/* ---- Persistence ---- */

/**
 * gnostr_bookmarks_save_async:
 * @self: bookmark list instance
 * @callback: callback when save completes
 * @user_data: user data for callback
 *
 * Signs and publishes the bookmark list (kind 10003) to relays via signer IPC.
 * Uses the user's configured write relays for publishing.
 *
 * The event is signed using the nostr-homed signer service. After signing,
 * the event is published to all configured write relays. The callback is
 * invoked with success=TRUE if at least one relay accepts the event.
 *
 * Note: Private bookmarks (is_private=TRUE) are currently stored as public
 * until NIP-44 encryption is implemented.
 */
typedef void (*GnostrBookmarksSaveCallback)(GnostrBookmarks *self,
                                             gboolean success,
                                             const char *error_msg,
                                             gpointer user_data);

void gnostr_bookmarks_save_async(GnostrBookmarks *self,
                                  GnostrBookmarksSaveCallback callback,
                                  gpointer user_data);

/* ---- Accessors ---- */

/**
 * gnostr_bookmarks_get_event_ids:
 * @self: bookmark list instance
 * @count: (out): number of bookmarked events
 *
 * Gets all bookmarked event IDs.
 *
 * Returns: (transfer container): array of event ID hex strings (do not free strings)
 */
const char **gnostr_bookmarks_get_event_ids(GnostrBookmarks *self, size_t *count);

/**
 * gnostr_bookmarks_is_dirty:
 * @self: bookmark list instance
 *
 * Returns: TRUE if there are unsaved changes
 */
gboolean gnostr_bookmarks_is_dirty(GnostrBookmarks *self);

/**
 * gnostr_bookmarks_get_count:
 * @self: bookmark list instance
 *
 * Returns: number of bookmarked events
 */
size_t gnostr_bookmarks_get_count(GnostrBookmarks *self);

/* ---- Auto-Sync ---- */

/**
 * gnostr_bookmarks_sync_on_login:
 * @pubkey_hex: user's public key (64 hex chars)
 *
 * Convenience function to fetch bookmarks when user logs in.
 * Fetches the user's bookmark list from configured relays and merges
 * with any local bookmarks.
 *
 * This is a fire-and-forget operation; no callback is provided.
 * Connect to the "changed" signal to be notified when sync completes.
 */
void gnostr_bookmarks_sync_on_login(const char *pubkey_hex);

/**
 * gnostr_bookmarks_get_last_sync_time:
 * @self: bookmark list instance
 *
 * Returns: the created_at timestamp of the last synced event, or 0 if never synced
 */
gint64 gnostr_bookmarks_get_last_sync_time(GnostrBookmarks *self);

/* ---- Signals ---- */

/**
 * "changed" signal
 *
 * Emitted when the bookmark list is modified locally or loaded from relays.
 * Connect to update UI state.
 */

G_END_DECLS

#endif /* GNOSTR_BOOKMARKS_H */
