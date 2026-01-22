/**
 * gnostr NIP-51 Settings Sync
 *
 * Manages application settings synchronization using NIP-51 lists.
 * Stores preferences in kind 30078 (application-specific data) events.
 */

#ifndef GNOSTR_NIP51_SETTINGS_H
#define GNOSTR_NIP51_SETTINGS_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * Application-specific settings stored in kind 30078 events.
 * The d-tag is "gnostr/settings" to namespace our app's data.
 */
#define GNOSTR_NIP51_SETTINGS_D_TAG "gnostr/settings"

/**
 * Nostr kind for application-specific data (NIP-78)
 */
#define GNOSTR_KIND_APP_SPECIFIC_DATA 30078

/**
 * Callback for async settings load operation
 */
typedef void (*GnostrNip51SettingsLoadCallback)(gboolean success,
                                                  const gchar *error_msg,
                                                  gpointer user_data);

/**
 * Callback for async settings backup operation
 */
typedef void (*GnostrNip51SettingsBackupCallback)(gboolean success,
                                                    const gchar *error_msg,
                                                    gpointer user_data);

/**
 * Load settings from relays (kind 30078 with d-tag "gnostr/settings").
 * Fetches the latest settings event and applies it to local GSettings.
 *
 * @param pubkey_hex    User's public key (hex)
 * @param callback      Callback when load completes (may be NULL)
 * @param user_data     User data for callback
 */
void gnostr_nip51_settings_load_async(const gchar *pubkey_hex,
                                       GnostrNip51SettingsLoadCallback callback,
                                       gpointer user_data);

/**
 * Backup current settings to relays.
 * Creates a kind 30078 event with current app settings and publishes it.
 *
 * @param callback      Callback when backup completes (may be NULL)
 * @param user_data     User data for callback
 */
void gnostr_nip51_settings_backup_async(GnostrNip51SettingsBackupCallback callback,
                                         gpointer user_data);

/**
 * Build unsigned kind 30078 event JSON from current settings.
 * Returns newly allocated JSON string (caller frees with g_free).
 */
gchar *gnostr_nip51_settings_build_event_json(void);

/**
 * Parse a kind 30078 settings event and apply to local config.
 *
 * @param event_json    JSON string of the kind 30078 event
 * @return TRUE if settings were applied, FALSE on error
 */
gboolean gnostr_nip51_settings_from_event(const gchar *event_json);

/**
 * Check if NIP-51 settings sync is enabled.
 *
 * @return TRUE if sync is enabled
 */
gboolean gnostr_nip51_settings_sync_enabled(void);

/**
 * Set NIP-51 settings sync enabled state.
 *
 * @param enabled   Whether to enable sync
 */
void gnostr_nip51_settings_set_sync_enabled(gboolean enabled);

/**
 * Get the timestamp of the last successful sync.
 *
 * @return Unix timestamp, or 0 if never synced
 */
gint64 gnostr_nip51_settings_last_sync(void);

/**
 * Auto-sync settings on login (if enabled).
 * This is called after successful sign-in to restore user settings.
 *
 * @param pubkey_hex    User's public key (hex)
 */
void gnostr_nip51_settings_auto_sync_on_login(const gchar *pubkey_hex);

G_END_DECLS

#endif /* GNOSTR_NIP51_SETTINGS_H */
