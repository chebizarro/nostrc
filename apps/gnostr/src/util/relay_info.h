#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * GnostrRelayInfo:
 *
 * NIP-11 Relay Information Document.
 * Contains metadata about a Nostr relay fetched via HTTP GET
 * with Accept: application/nostr+json header.
 */
typedef struct _GnostrRelayInfo GnostrRelayInfo;

struct _GnostrRelayInfo {
  gchar *url;              /* Original relay URL (ws:// or wss://) */
  gchar *name;             /* Relay name */
  gchar *description;      /* Relay description */
  gchar *pubkey;           /* Admin pubkey (hex) */
  gchar *contact;          /* Contact info (email/URL) */
  gchar *software;         /* Software name (e.g., "strfry", "nostr-rs-relay") */
  gchar *version;          /* Software version */
  gchar *icon;             /* Icon URL */
  gchar *posting_policy;   /* URL to posting policy */
  gchar *payments_url;     /* URL to payments page */

  /* Supported NIPs */
  gint *supported_nips;    /* Array of NIP numbers */
  gsize supported_nips_count;

  /* Limitations */
  gint max_message_length;
  gint max_subscriptions;
  gint max_filters;
  gint max_limit;
  gint max_subid_length;
  gint max_event_tags;
  gint max_content_length;
  gint min_pow_difficulty;
  gboolean auth_required;
  gboolean payment_required;
  gboolean restricted_writes;

  /* Tags/categories */
  gchar **relay_countries;
  gsize relay_countries_count;
  gchar **language_tags;
  gsize language_tags_count;
  gchar **tags;
  gsize tags_count;

  /* Caching metadata */
  gint64 fetched_at;       /* Unix timestamp when fetched */
  gboolean fetch_failed;   /* TRUE if last fetch failed */
  gchar *fetch_error;      /* Error message if fetch failed */
};

/**
 * gnostr_relay_info_new:
 *
 * Creates a new empty GnostrRelayInfo.
 *
 * Returns: (transfer full): A newly allocated GnostrRelayInfo.
 */
GnostrRelayInfo *gnostr_relay_info_new(void);

/**
 * gnostr_relay_info_free:
 * @info: (nullable): A GnostrRelayInfo to free.
 *
 * Frees all resources associated with a GnostrRelayInfo.
 */
void gnostr_relay_info_free(GnostrRelayInfo *info);

/**
 * gnostr_relay_info_parse_json:
 * @json: JSON string containing NIP-11 relay information document.
 * @url: (nullable): Original relay URL to store.
 *
 * Parses a NIP-11 JSON document into a GnostrRelayInfo struct.
 *
 * Returns: (transfer full) (nullable): A newly allocated GnostrRelayInfo,
 *          or NULL on parse error.
 */
GnostrRelayInfo *gnostr_relay_info_parse_json(const gchar *json, const gchar *url);

/**
 * gnostr_relay_info_fetch_async:
 * @relay_url: WebSocket URL of the relay (ws:// or wss://).
 * @cancellable: (nullable): A GCancellable.
 * @callback: Callback to invoke when operation completes.
 * @user_data: User data for callback.
 *
 * Asynchronously fetches NIP-11 relay information document.
 * Converts wss:// to https:// and ws:// to http://.
 * Sends HTTP GET with Accept: application/nostr+json header.
 */
void gnostr_relay_info_fetch_async(const gchar *relay_url,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data);

/**
 * gnostr_relay_info_fetch_finish:
 * @result: The GAsyncResult from the callback.
 * @error: (out) (optional): Return location for error.
 *
 * Finishes an async relay info fetch operation.
 *
 * Returns: (transfer full) (nullable): A GnostrRelayInfo on success,
 *          or NULL on error.
 */
GnostrRelayInfo *gnostr_relay_info_fetch_finish(GAsyncResult *result,
                                                 GError **error);

/* ---- Relay Info Cache ---- */

/**
 * gnostr_relay_info_cache_get:
 * @relay_url: WebSocket URL of the relay.
 *
 * Gets cached relay info if available and not expired.
 *
 * Returns: (transfer full) (nullable): Cached GnostrRelayInfo or NULL.
 */
GnostrRelayInfo *gnostr_relay_info_cache_get(const gchar *relay_url);

/**
 * gnostr_relay_info_cache_put:
 * @info: (transfer none): Relay info to cache.
 *
 * Stores relay info in the cache.
 */
void gnostr_relay_info_cache_put(GnostrRelayInfo *info);

/**
 * gnostr_relay_info_cache_clear:
 *
 * Clears all cached relay info.
 */
void gnostr_relay_info_cache_clear(void);

/**
 * gnostr_relay_info_format_nips:
 * @info: Relay info.
 *
 * Formats supported NIPs as a comma-separated string.
 *
 * Returns: (transfer full): Formatted string or "(none)".
 */
gchar *gnostr_relay_info_format_nips(const GnostrRelayInfo *info);

/**
 * gnostr_relay_info_format_limitations:
 * @info: Relay info.
 *
 * Formats limitations as human-readable multi-line string.
 *
 * Returns: (transfer full): Formatted string or "(none specified)".
 */
gchar *gnostr_relay_info_format_limitations(const GnostrRelayInfo *info);

G_END_DECLS
