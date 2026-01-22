/* relay_info.h - NIP-11 Relay Information Document support
 *
 * Fetches and parses NIP-11 relay metadata documents.
 * Used to display relay name, description, and supported NIPs in the UI.
 */
#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * RelayInfo:
 *
 * NIP-11 Relay Information Document (simplified).
 * Contains metadata about a Nostr relay.
 */
typedef struct _RelayInfo RelayInfo;

struct _RelayInfo {
  gchar *url;              /* Original relay URL (ws:// or wss://) */
  gchar *name;             /* Relay name */
  gchar *description;      /* Relay description */
  gchar *software;         /* Software name (e.g., "strfry") */
  gchar *version;          /* Software version */
  gchar *contact;          /* Contact info */

  /* Supported NIPs */
  gint *supported_nips;    /* Array of NIP numbers */
  gsize supported_nips_count;

  /* Key limitations */
  gboolean auth_required;
  gboolean payment_required;

  /* Fetch metadata */
  gint64 fetched_at;       /* Unix timestamp when fetched */
  gboolean fetch_failed;   /* TRUE if last fetch failed */
  gchar *fetch_error;      /* Error message if fetch failed */
};

/**
 * relay_info_new:
 *
 * Creates a new empty RelayInfo.
 *
 * Returns: (transfer full): A newly allocated RelayInfo.
 */
RelayInfo *relay_info_new(void);

/**
 * relay_info_free:
 * @info: (nullable): A RelayInfo to free.
 *
 * Frees all resources associated with a RelayInfo.
 */
void relay_info_free(RelayInfo *info);

/**
 * relay_info_parse_json:
 * @json: JSON string containing NIP-11 relay information document.
 * @url: (nullable): Original relay URL to store.
 *
 * Parses a NIP-11 JSON document into a RelayInfo struct.
 *
 * Returns: (transfer full) (nullable): A newly allocated RelayInfo,
 *          or NULL on parse error.
 */
RelayInfo *relay_info_parse_json(const gchar *json, const gchar *url);

/**
 * Callback for async relay info fetch.
 */
typedef void (*RelayInfoCallback)(RelayInfo *info, const gchar *error, gpointer user_data);

/**
 * relay_info_fetch_async:
 * @relay_url: WebSocket URL of the relay (ws:// or wss://).
 * @callback: Callback to invoke when operation completes.
 * @user_data: User data for callback.
 *
 * Asynchronously fetches NIP-11 relay information document.
 * Converts wss:// to https:// and ws:// to http://.
 * Uses GIO for HTTP GET with Accept: application/nostr+json header.
 */
void relay_info_fetch_async(const gchar *relay_url,
                            RelayInfoCallback callback,
                            gpointer user_data);

/**
 * relay_info_format_nips:
 * @info: Relay info.
 *
 * Formats supported NIPs as a comma-separated string.
 *
 * Returns: (transfer full): Formatted string or "(none)".
 */
gchar *relay_info_format_nips(const RelayInfo *info);

/**
 * relay_info_cache_get:
 * @relay_url: WebSocket URL of the relay.
 *
 * Gets cached relay info if available and not expired.
 *
 * Returns: (transfer full) (nullable): Cached RelayInfo or NULL.
 */
RelayInfo *relay_info_cache_get(const gchar *relay_url);

/**
 * relay_info_cache_put:
 * @info: (transfer none): Relay info to cache.
 *
 * Stores relay info in the cache.
 */
void relay_info_cache_put(RelayInfo *info);

G_END_DECLS
