/**
 * NIP-51 Pin List Service
 *
 * Provides pin list management for the gnostr GTK app.
 * Handles loading/parsing kind 10001 events and managing pinned notes.
 * Pinned notes appear prominently on the user's profile.
 */

#ifndef GNOSTR_PIN_LIST_H
#define GNOSTR_PIN_LIST_H

#include <gio/gio.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

/* ---- Opaque Pin List Handle ---- */
typedef struct _GnostrPinList GnostrPinList;

/* ---- Initialization ---- */

/**
 * gnostr_pin_list_get_default:
 *
 * Gets the singleton pin list instance for the app.
 * Creates it on first call. Thread-safe.
 *
 * Returns: (transfer none): the shared pin list instance
 */
GnostrPinList *gnostr_pin_list_get_default(void);

/**
 * gnostr_pin_list_shutdown:
 *
 * Releases the singleton instance. Call at app shutdown.
 */
void gnostr_pin_list_shutdown(void);

/* ---- Loading ---- */

/**
 * gnostr_pin_list_load_from_json:
 * @self: pin list instance
 * @event_json: JSON string of kind 10001 event
 *
 * Parses a kind 10001 pin list event and caches entries.
 *
 * Returns: TRUE on success
 */
gboolean gnostr_pin_list_load_from_json(GnostrPinList *self,
                                         const char *event_json);

/**
 * GnostrPinListMergeStrategy:
 * @GNOSTR_PIN_LIST_MERGE_REMOTE_WINS: Remote data replaces local
 * @GNOSTR_PIN_LIST_MERGE_LOCAL_WINS: Local data is kept
 * @GNOSTR_PIN_LIST_MERGE_UNION: Merge lists (union of pins)
 * @GNOSTR_PIN_LIST_MERGE_LATEST: Keep data with newest timestamp
 */
typedef enum {
    GNOSTR_PIN_LIST_MERGE_REMOTE_WINS,
    GNOSTR_PIN_LIST_MERGE_LOCAL_WINS,
    GNOSTR_PIN_LIST_MERGE_UNION,
    GNOSTR_PIN_LIST_MERGE_LATEST
} GnostrPinListMergeStrategy;

/**
 * GnostrPinListFetchCallback:
 * @self: pin list instance
 * @success: TRUE if fetch succeeded
 * @user_data: user data
 */
typedef void (*GnostrPinListFetchCallback)(GnostrPinList *self,
                                            gboolean success,
                                            gpointer user_data);

/**
 * gnostr_pin_list_fetch_async:
 * @self: pin list instance
 * @pubkey_hex: user's public key (64 hex chars)
 * @relays: (nullable): NULL-terminated array of relay URLs
 * @callback: callback when fetch completes
 * @user_data: user data for callback
 *
 * Fetches the user's pin list from relays asynchronously.
 */
void gnostr_pin_list_fetch_async(GnostrPinList *self,
                                  const char *pubkey_hex,
                                  const char * const *relays,
                                  GnostrPinListFetchCallback callback,
                                  gpointer user_data);

/**
 * gnostr_pin_list_fetch_with_strategy_async:
 * @self: pin list instance
 * @pubkey_hex: user's public key (64 hex chars)
 * @relays: (nullable): NULL-terminated array of relay URLs
 * @strategy: merge strategy to use
 * @callback: callback when fetch completes
 * @user_data: user data for callback
 */
void gnostr_pin_list_fetch_with_strategy_async(GnostrPinList *self,
                                                const char *pubkey_hex,
                                                const char * const *relays,
                                                GnostrPinListMergeStrategy strategy,
                                                GnostrPinListFetchCallback callback,
                                                gpointer user_data);

/* ---- Query Functions ---- */

/**
 * gnostr_pin_list_is_pinned:
 * @self: pin list instance
 * @event_id_hex: event ID to check (64 hex chars)
 *
 * Checks if an event is pinned.
 *
 * Returns: TRUE if pinned
 */
gboolean gnostr_pin_list_is_pinned(GnostrPinList *self,
                                    const char *event_id_hex);

/* ---- Modification Functions ---- */

/**
 * gnostr_pin_list_add:
 * @self: pin list instance
 * @event_id_hex: event ID to pin (64 hex chars)
 * @relay_hint: (nullable): optional relay URL hint
 *
 * Adds an event to the pin list (locally).
 * Call gnostr_pin_list_save_async() to persist to relays.
 */
void gnostr_pin_list_add(GnostrPinList *self,
                          const char *event_id_hex,
                          const char *relay_hint);

/**
 * gnostr_pin_list_remove:
 * @self: pin list instance
 * @event_id_hex: event ID to unpin (64 hex chars)
 */
void gnostr_pin_list_remove(GnostrPinList *self,
                             const char *event_id_hex);

/**
 * gnostr_pin_list_toggle:
 * @self: pin list instance
 * @event_id_hex: event ID to toggle (64 hex chars)
 * @relay_hint: (nullable): optional relay URL hint (for add)
 *
 * Toggles the pin state of an event.
 *
 * Returns: TRUE if the event is now pinned, FALSE if unpinned
 */
gboolean gnostr_pin_list_toggle(GnostrPinList *self,
                                 const char *event_id_hex,
                                 const char *relay_hint);

/* ---- Persistence ---- */

/**
 * GnostrPinListSaveCallback:
 * @self: pin list instance
 * @success: TRUE if save succeeded
 * @error_msg: (nullable): error message if failed
 * @user_data: user data
 */
typedef void (*GnostrPinListSaveCallback)(GnostrPinList *self,
                                           gboolean success,
                                           const char *error_msg,
                                           gpointer user_data);

/**
 * gnostr_pin_list_save_async:
 * @self: pin list instance
 * @callback: callback when save completes
 * @user_data: user data for callback
 *
 * Signs and publishes the pin list (kind 10001) to relays.
 */
void gnostr_pin_list_save_async(GnostrPinList *self,
                                 GnostrPinListSaveCallback callback,
                                 gpointer user_data);

/* ---- Accessors ---- */

/**
 * gnostr_pin_list_get_event_ids:
 * @self: pin list instance
 * @count: (out): number of pinned events
 *
 * Returns: (transfer container): array of event ID hex strings (do not free strings)
 */
const char **gnostr_pin_list_get_event_ids(GnostrPinList *self, size_t *count);

/**
 * gnostr_pin_list_is_dirty:
 * @self: pin list instance
 *
 * Returns: TRUE if there are unsaved changes
 */
gboolean gnostr_pin_list_is_dirty(GnostrPinList *self);

/**
 * gnostr_pin_list_get_count:
 * @self: pin list instance
 *
 * Returns: number of pinned events
 */
size_t gnostr_pin_list_get_count(GnostrPinList *self);

/* ---- Auto-Sync ---- */

/**
 * gnostr_pin_list_sync_on_login:
 * @pubkey_hex: user's public key (64 hex chars)
 *
 * Convenience function to fetch pin list when user logs in.
 */
void gnostr_pin_list_sync_on_login(const char *pubkey_hex);

/**
 * gnostr_pin_list_get_last_sync_time:
 * @self: pin list instance
 *
 * Returns: the created_at timestamp of the last synced event, or 0
 */
gint64 gnostr_pin_list_get_last_sync_time(GnostrPinList *self);

G_END_DECLS

#endif /* GNOSTR_PIN_LIST_H */
