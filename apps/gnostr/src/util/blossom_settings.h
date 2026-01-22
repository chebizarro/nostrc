/**
 * gnostr Blossom Settings
 *
 * Manages user Blossom server preferences (kind 10063).
 * Provides GSettings-backed local config and kind 10063 event sync.
 */

#ifndef GNOSTR_BLOSSOM_SETTINGS_H
#define GNOSTR_BLOSSOM_SETTINGS_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * Blossom server entry
 */
typedef struct {
  char *url;        /* Server URL (e.g., "https://blossom.example.com") */
  gboolean enabled; /* Whether this server is active for uploads */
} GnostrBlossomServer;

/**
 * Free a server entry
 */
void gnostr_blossom_server_free(GnostrBlossomServer *server);

/**
 * Get the singleton Blossom settings manager.
 * Loads settings from GSettings on first call.
 *
 * @return The settings manager (do not free)
 */
GObject *gnostr_blossom_settings_get_default(void);

/**
 * Get the default/primary Blossom server URL.
 *
 * @return Server URL (static string, do not free) or NULL if none configured
 */
const char *gnostr_blossom_settings_get_default_server(void);

/**
 * Set the default/primary Blossom server URL.
 *
 * @param url Server URL to set as default
 */
void gnostr_blossom_settings_set_default_server(const char *url);

/**
 * Get the list of configured Blossom servers.
 *
 * @param out_count Output: number of servers in the list
 * @return Array of server entries (caller must free with gnostr_blossom_servers_free)
 */
GnostrBlossomServer **gnostr_blossom_settings_get_servers(gsize *out_count);

/**
 * Free a server list returned by gnostr_blossom_settings_get_servers.
 */
void gnostr_blossom_servers_free(GnostrBlossomServer **servers, gsize count);

/**
 * Add a server to the list.
 *
 * @param url Server URL to add
 * @return TRUE if added, FALSE if already exists
 */
gboolean gnostr_blossom_settings_add_server(const char *url);

/**
 * Remove a server from the list.
 *
 * @param url Server URL to remove
 * @return TRUE if removed, FALSE if not found
 */
gboolean gnostr_blossom_settings_remove_server(const char *url);

/**
 * Parse a kind 10063 event and update settings.
 *
 * @param event_json JSON string of the kind 10063 event
 * @return TRUE if settings were updated, FALSE on error
 */
gboolean gnostr_blossom_settings_from_event(const char *event_json);

/**
 * Build a kind 10063 event JSON from current settings.
 * The event is unsigned; use signer IPC to sign before publishing.
 *
 * @return Newly allocated JSON string (caller frees)
 */
char *gnostr_blossom_settings_to_event(void);

/**
 * Callback for async server list load from relays
 */
typedef void (*GnostrBlossomSettingsLoadCallback)(gboolean success,
                                                    GError *error,
                                                    gpointer user_data);

/**
 * Load server list from relays (kind 10063 query).
 *
 * @param pubkey_hex User's public key (hex)
 * @param callback Callback when load completes
 * @param user_data User data for callback
 */
void gnostr_blossom_settings_load_from_relays_async(const char *pubkey_hex,
                                                      GnostrBlossomSettingsLoadCallback callback,
                                                      gpointer user_data);

/**
 * Callback for async server list publish
 */
typedef void (*GnostrBlossomSettingsPublishCallback)(gboolean success,
                                                       GError *error,
                                                       gpointer user_data);

/**
 * Publish current server list to relays (kind 10063 event).
 *
 * @param callback Callback when publish completes
 * @param user_data User data for callback
 */
void gnostr_blossom_settings_publish_async(GnostrBlossomSettingsPublishCallback callback,
                                             gpointer user_data);

/**
 * Reorder a server in the list (move from one position to another).
 *
 * @param from_index Current index of the server
 * @param to_index Destination index for the server
 * @return TRUE if reorder succeeded, FALSE if indices are invalid
 */
gboolean gnostr_blossom_settings_reorder_server(gsize from_index, gsize to_index);

/**
 * Set the enabled state of a server at a given index.
 *
 * @param index Server index
 * @param enabled Whether the server should be enabled
 * @return TRUE if successful, FALSE if index is invalid
 */
gboolean gnostr_blossom_settings_set_server_enabled(gsize index, gboolean enabled);

/**
 * Get the count of configured servers.
 *
 * @return Number of servers in the list
 */
gsize gnostr_blossom_settings_get_server_count(void);

/**
 * Get a server URL by index.
 *
 * @param index Server index
 * @return Server URL (static, do not free) or NULL if invalid index
 */
const char *gnostr_blossom_settings_get_server_url(gsize index);

/**
 * Get the list of enabled server URLs in priority order.
 * This is useful for upload fallback - try servers in order until one succeeds.
 *
 * @param out_count Output: number of URLs returned
 * @return Array of URLs (caller must free array with g_free, but not strings)
 */
const char **gnostr_blossom_settings_get_enabled_urls(gsize *out_count);

/**
 * Clear all servers from the list.
 */
void gnostr_blossom_settings_clear_servers(void);

/**
 * Default public Blossom servers (fallback if none configured)
 */
#define GNOSTR_BLOSSOM_DEFAULT_SERVER "https://blossom.primal.net"

G_END_DECLS

#endif /* GNOSTR_BLOSSOM_SETTINGS_H */
