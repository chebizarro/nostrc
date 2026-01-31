/**
 * GnostrAppDataManager - Gnostr App-Specific Data Sync Manager
 *
 * High-level manager for synchronizing gnostr application data
 * across devices using NIP-78 kind 30078 events.
 *
 * Supports syncing:
 * - gnostr/preferences - UI preferences (theme, font size, etc.)
 * - gnostr/mutes - Muted users and words (syncs with mute_list.c)
 * - gnostr/bookmarks - Bookmarked notes (syncs with bookmarks.c)
 * - gnostr/drafts - Draft notes (syncs with gnostr-drafts.c)
 *
 * The manager provides:
 * - Automatic sync on login
 * - Conflict resolution (latest wins)
 * - Integration with GSettings for preferences
 * - Merge strategies for list data
 */

#ifndef GNOSTR_APP_DATA_MANAGER_H
#define GNOSTR_APP_DATA_MANAGER_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_APP_DATA_MANAGER (gnostr_app_data_manager_get_type())

G_DECLARE_FINAL_TYPE(GnostrAppDataManager, gnostr_app_data_manager,
                     GNOSTR, APP_DATA_MANAGER, GObject)

/**
 * App identifier for gnostr
 */
#define GNOSTR_APP_DATA_APP_ID "gnostr"

/**
 * Data keys supported by gnostr
 */
#define GNOSTR_APP_DATA_KEY_PREFERENCES "preferences"
#define GNOSTR_APP_DATA_KEY_MUTES       "mutes"
#define GNOSTR_APP_DATA_KEY_BOOKMARKS   "bookmarks"
#define GNOSTR_APP_DATA_KEY_DRAFTS      "drafts"

/**
 * Full d-tags (app_id/data_key format)
 */
#define GNOSTR_APP_DATA_DTAG_PREFERENCES "gnostr/preferences"
#define GNOSTR_APP_DATA_DTAG_MUTES       "gnostr/mutes"
#define GNOSTR_APP_DATA_DTAG_BOOKMARKS   "gnostr/bookmarks"
#define GNOSTR_APP_DATA_DTAG_DRAFTS      "gnostr/drafts"

/**
 * GnostrAppDataSyncStatus:
 * @GNOSTR_APP_DATA_SYNC_IDLE: No sync in progress
 * @GNOSTR_APP_DATA_SYNC_LOADING: Loading from relays
 * @GNOSTR_APP_DATA_SYNC_SAVING: Saving to relays
 * @GNOSTR_APP_DATA_SYNC_ERROR: Last sync failed
 * @GNOSTR_APP_DATA_SYNC_COMPLETE: Sync completed successfully
 */
typedef enum {
    GNOSTR_APP_DATA_SYNC_IDLE,
    GNOSTR_APP_DATA_SYNC_LOADING,
    GNOSTR_APP_DATA_SYNC_SAVING,
    GNOSTR_APP_DATA_SYNC_ERROR,
    GNOSTR_APP_DATA_SYNC_COMPLETE
} GnostrAppDataSyncStatus;

/**
 * GnostrAppDataMergeStrategy:
 * @GNOSTR_APP_DATA_MERGE_REMOTE_WINS: Remote data replaces local
 * @GNOSTR_APP_DATA_MERGE_LOCAL_WINS: Local data is kept
 * @GNOSTR_APP_DATA_MERGE_UNION: Merge lists (union of items)
 * @GNOSTR_APP_DATA_MERGE_LATEST: Keep data with newest timestamp
 */
typedef enum {
    GNOSTR_APP_DATA_MERGE_REMOTE_WINS,
    GNOSTR_APP_DATA_MERGE_LOCAL_WINS,
    GNOSTR_APP_DATA_MERGE_UNION,
    GNOSTR_APP_DATA_MERGE_LATEST
} GnostrAppDataMergeStrategy;

/**
 * Callback for sync operations
 */
typedef void (*GnostrAppDataManagerCallback)(GnostrAppDataManager *manager,
                                              gboolean success,
                                              const char *error_message,
                                              gpointer user_data);

/**
 * Callback for preferences load/save
 */
typedef void (*GnostrAppDataPreferencesCallback)(GnostrAppDataManager *manager,
                                                   gboolean success,
                                                   const char *error_message,
                                                   gpointer user_data);

/* ---- Singleton Access ---- */

/**
 * gnostr_app_data_manager_get_default:
 *
 * Gets the default (global) app data manager instance.
 *
 * Returns: (transfer none): The singleton manager instance
 */
GnostrAppDataManager *gnostr_app_data_manager_get_default(void);

/**
 * gnostr_app_data_manager_shutdown:
 *
 * Shuts down the default manager and frees resources.
 * Call this at application shutdown.
 */
void gnostr_app_data_manager_shutdown(void);

/* ---- Configuration ---- */

/**
 * gnostr_app_data_manager_set_user_pubkey:
 * @self: The manager
 * @pubkey_hex: User's public key (64 hex chars)
 *
 * Sets the current user's public key. Required for sync operations.
 */
void gnostr_app_data_manager_set_user_pubkey(GnostrAppDataManager *self,
                                              const char *pubkey_hex);

/**
 * gnostr_app_data_manager_get_user_pubkey:
 * @self: The manager
 *
 * Gets the current user's public key.
 *
 * Returns: (transfer none) (nullable): The pubkey or NULL
 */
const char *gnostr_app_data_manager_get_user_pubkey(GnostrAppDataManager *self);

/**
 * gnostr_app_data_manager_set_sync_enabled:
 * @self: The manager
 * @enabled: Whether sync is enabled
 *
 * Enables or disables automatic sync.
 */
void gnostr_app_data_manager_set_sync_enabled(GnostrAppDataManager *self,
                                               gboolean enabled);

/**
 * gnostr_app_data_manager_is_sync_enabled:
 * @self: The manager
 *
 * Checks if sync is enabled.
 *
 * Returns: TRUE if sync is enabled
 */
gboolean gnostr_app_data_manager_is_sync_enabled(GnostrAppDataManager *self);

/* ---- Sync Status ---- */

/**
 * gnostr_app_data_manager_get_sync_status:
 * @self: The manager
 *
 * Gets the current sync status.
 *
 * Returns: The sync status
 */
GnostrAppDataSyncStatus gnostr_app_data_manager_get_sync_status(GnostrAppDataManager *self);

/**
 * gnostr_app_data_manager_get_last_sync_time:
 * @self: The manager
 * @data_key: The data key (e.g., GNOSTR_APP_DATA_KEY_PREFERENCES)
 *
 * Gets the timestamp of the last successful sync for a data type.
 *
 * Returns: Unix timestamp, or 0 if never synced
 */
gint64 gnostr_app_data_manager_get_last_sync_time(GnostrAppDataManager *self,
                                                   const char *data_key);

/* ---- Preferences Sync ---- */

/**
 * gnostr_app_data_manager_load_preferences_async:
 * @self: The manager
 * @callback: Callback when load completes
 * @user_data: User data for callback
 *
 * Loads preferences from relays and applies them to GSettings.
 * Uses MERGE_LATEST strategy - remote wins if newer.
 */
void gnostr_app_data_manager_load_preferences_async(GnostrAppDataManager *self,
                                                     GnostrAppDataPreferencesCallback callback,
                                                     gpointer user_data);

/**
 * gnostr_app_data_manager_save_preferences_async:
 * @self: The manager
 * @callback: Callback when save completes
 * @user_data: User data for callback
 *
 * Saves current GSettings preferences to relays.
 */
void gnostr_app_data_manager_save_preferences_async(GnostrAppDataManager *self,
                                                     GnostrAppDataPreferencesCallback callback,
                                                     gpointer user_data);

/**
 * gnostr_app_data_manager_build_preferences_json:
 * @self: The manager
 *
 * Builds a JSON string of current preferences from GSettings.
 *
 * Returns: (transfer full): JSON string or NULL on error
 */
char *gnostr_app_data_manager_build_preferences_json(GnostrAppDataManager *self);

/**
 * gnostr_app_data_manager_apply_preferences_json:
 * @self: The manager
 * @json: Preferences JSON string
 *
 * Applies preferences from JSON to GSettings.
 *
 * Returns: TRUE on success
 */
gboolean gnostr_app_data_manager_apply_preferences_json(GnostrAppDataManager *self,
                                                         const char *json);

/* ---- Full Sync ---- */

/**
 * gnostr_app_data_manager_sync_all_async:
 * @self: The manager
 * @callback: Callback when sync completes
 * @user_data: User data for callback
 *
 * Syncs all app data types (preferences, mutes, bookmarks, drafts).
 * Loads from relays and merges with local data.
 */
void gnostr_app_data_manager_sync_all_async(GnostrAppDataManager *self,
                                             GnostrAppDataManagerCallback callback,
                                             gpointer user_data);

/**
 * gnostr_app_data_manager_sync_on_login:
 * @pubkey_hex: User's public key (64 hex chars)
 *
 * Convenience function to start sync when user logs in.
 * Sets the pubkey and starts async sync if enabled.
 */
void gnostr_app_data_manager_sync_on_login(const char *pubkey_hex);

/* ---- Individual Data Type Sync ---- */

/**
 * gnostr_app_data_manager_sync_mutes_async:
 * @self: The manager
 * @strategy: Merge strategy
 * @callback: Callback when sync completes
 * @user_data: User data for callback
 *
 * Syncs mute list with relays using NIP-78.
 * Coordinates with mute_list.c for local storage.
 */
void gnostr_app_data_manager_sync_mutes_async(GnostrAppDataManager *self,
                                               GnostrAppDataMergeStrategy strategy,
                                               GnostrAppDataManagerCallback callback,
                                               gpointer user_data);

/**
 * gnostr_app_data_manager_sync_bookmarks_async:
 * @self: The manager
 * @strategy: Merge strategy
 * @callback: Callback when sync completes
 * @user_data: User data for callback
 *
 * Syncs bookmarks with relays using NIP-78.
 * Coordinates with bookmarks.c for local storage.
 */
void gnostr_app_data_manager_sync_bookmarks_async(GnostrAppDataManager *self,
                                                   GnostrAppDataMergeStrategy strategy,
                                                   GnostrAppDataManagerCallback callback,
                                                   gpointer user_data);

/**
 * gnostr_app_data_manager_sync_drafts_async:
 * @self: The manager
 * @strategy: Merge strategy
 * @callback: Callback when sync completes
 * @user_data: User data for callback
 *
 * Syncs drafts with relays using NIP-78.
 * Coordinates with gnostr-drafts.c for local storage.
 */
void gnostr_app_data_manager_sync_drafts_async(GnostrAppDataManager *self,
                                                GnostrAppDataMergeStrategy strategy,
                                                GnostrAppDataManagerCallback callback,
                                                gpointer user_data);

/* ---- Custom App Data ---- */

/**
 * gnostr_app_data_manager_get_custom_data_async:
 * @self: The manager
 * @data_key: Custom data key
 * @callback: Callback with result
 * @user_data: User data for callback
 *
 * Gets custom app data from relays.
 */
typedef void (*GnostrAppDataGetCallback)(GnostrAppDataManager *manager,
                                          const char *content,
                                          gint64 created_at,
                                          gboolean success,
                                          const char *error_message,
                                          gpointer user_data);

void gnostr_app_data_manager_get_custom_data_async(GnostrAppDataManager *self,
                                                    const char *data_key,
                                                    GnostrAppDataGetCallback callback,
                                                    gpointer user_data);

/**
 * gnostr_app_data_manager_set_custom_data_async:
 * @self: The manager
 * @data_key: Custom data key
 * @content: Content to store (often JSON)
 * @callback: Callback when complete
 * @user_data: User data for callback
 *
 * Stores custom app data to relays.
 */
void gnostr_app_data_manager_set_custom_data_async(GnostrAppDataManager *self,
                                                    const char *data_key,
                                                    const char *content,
                                                    GnostrAppDataManagerCallback callback,
                                                    gpointer user_data);

/* ---- Utility ---- */

/**
 * gnostr_app_data_manager_clear_local_cache:
 * @self: The manager
 * @data_key: Data key to clear (or NULL for all)
 *
 * Clears locally cached sync timestamps and data.
 * Next sync will do a full reload.
 */
void gnostr_app_data_manager_clear_local_cache(GnostrAppDataManager *self,
                                                const char *data_key);

/**
 * gnostr_app_data_manager_is_syncing:
 * @self: The manager
 *
 * Checks if any sync operation is in progress.
 *
 * Returns: TRUE if syncing
 */
gboolean gnostr_app_data_manager_is_syncing(GnostrAppDataManager *self);

/* ---- Signals ---- */

/**
 * GnostrAppDataManager::sync-started
 * @self: The manager
 * @data_key: The data type being synced (or NULL for all)
 *
 * Emitted when a sync operation starts.
 */

/**
 * GnostrAppDataManager::sync-completed
 * @self: The manager
 * @data_key: The data type synced (or NULL for all)
 * @success: Whether sync succeeded
 *
 * Emitted when a sync operation completes.
 */

/**
 * GnostrAppDataManager::preferences-changed
 * @self: The manager
 *
 * Emitted when preferences are updated from remote.
 */

G_END_DECLS

#endif /* GNOSTR_APP_DATA_MANAGER_H */
