/**
 * NIP-66 Relay Discovery and Monitoring
 *
 * NIP-66 defines relay discovery through:
 * - Kind 30166: Relay metadata (parameterized replaceable event)
 * - Kind 10166: Relay monitor announcement (replaceable event)
 *
 * This module provides:
 * - Parsing of kind 30166 relay metadata events
 * - Parsing of kind 10166 relay monitor events
 * - Cache management for discovered relays
 * - Query API for finding relays by criteria (region, NIPs, online status)
 * - Async relay discovery from known monitors
 */

#ifndef GNOSTR_NIP66_RELAY_DISCOVERY_H
#define GNOSTR_NIP66_RELAY_DISCOVERY_H

#include <glib.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* ============== Event Kind Constants ============== */

#define GNOSTR_NIP66_KIND_RELAY_META     30166  /* Relay metadata (parameterized replaceable) */
#define GNOSTR_NIP66_KIND_RELAY_MONITOR  10166  /* Relay monitor announcement (replaceable) */

/* ============== Relay Metadata (kind 30166) ============== */

/**
 * GnostrNip66RelayNetwork:
 * Network type for relay
 */
typedef enum {
  GNOSTR_NIP66_NETWORK_UNKNOWN = 0,
  GNOSTR_NIP66_NETWORK_CLEARNET,   /* Standard internet */
  GNOSTR_NIP66_NETWORK_TOR,        /* Tor hidden service */
  GNOSTR_NIP66_NETWORK_I2P,        /* I2P network */
} GnostrNip66RelayNetwork;

/**
 * GnostrNip66RelayMeta:
 * Relay metadata from kind 30166 events.
 *
 * Published by relay monitors to announce relay information discovered
 * through NIP-11 and connectivity tests.
 */
typedef struct {
  gchar *event_id_hex;       /* Event ID */
  gchar *pubkey_hex;         /* Publisher (monitor) pubkey */
  gchar *d_tag;              /* Relay URL as identifier */

  /* Basic relay info */
  gchar *relay_url;          /* Relay WebSocket URL */
  gchar *name;               /* Relay name (from NIP-11) */
  gchar *description;        /* Relay description */
  gchar *pubkey;             /* Relay operator pubkey */
  gchar *contact;            /* Relay contact info */
  gchar *software;           /* Relay software name */
  gchar *version;            /* Relay software version */
  gchar *icon;               /* Relay icon URL */

  /* Supported NIPs */
  gint *supported_nips;      /* Array of NIP numbers */
  gsize supported_nips_count;

  /* Geographic info */
  gchar *country_code;       /* ISO 3166-1 alpha-2 country code (e.g., "US") */
  gchar *region;             /* Geographic region (e.g., "North America") */
  gchar *city;               /* City name */
  gdouble latitude;          /* GPS latitude */
  gdouble longitude;         /* GPS longitude */
  gboolean has_geolocation;  /* TRUE if lat/lon are set */

  /* Network info */
  GnostrNip66RelayNetwork network;  /* Network type */
  gchar **language_tags;     /* Language tags */
  gsize language_tags_count;
  gchar **tags;              /* Generic tags */
  gsize tags_count;

  /* Relay limitations (from NIP-11) */
  gint max_message_length;
  gint max_content_length;
  gint max_event_tags;
  gint max_subscriptions;
  gboolean auth_required;
  gboolean payment_required;
  gboolean restricted_writes;

  /* Monitoring stats (from monitor) */
  gboolean is_online;        /* Current online status */
  gint64 last_seen;          /* Last successful connection timestamp */
  gint64 first_seen;         /* First discovery timestamp */
  gdouble uptime_percent;    /* Uptime percentage (0-100) */
  gint64 latency_ms;         /* Average latency in milliseconds */
  gint64 latency_open_ms;    /* Connection open latency */
  gint64 latency_read_ms;    /* Read latency */
  gint64 latency_write_ms;   /* Write latency */

  /* Timestamps */
  gint64 created_at;         /* Event created_at */
  gint64 cached_at;          /* Local cache timestamp */
} GnostrNip66RelayMeta;

/**
 * GnostrNip66RelayMonitor:
 * Relay monitor announcement from kind 10166 events.
 *
 * Published by monitors to announce their presence and capabilities.
 */
typedef struct {
  gchar *event_id_hex;       /* Event ID */
  gchar *pubkey_hex;         /* Monitor pubkey */

  /* Monitor info */
  gchar *name;               /* Monitor name/identifier */
  gchar *description;        /* Monitor description */
  gchar *operator_pubkey;    /* Operator pubkey */
  gchar *contact;            /* Contact info */
  gchar *website;            /* Monitor website */

  /* Monitoring capabilities */
  gchar *frequency;          /* Check frequency (e.g., "1h", "15m") */
  gchar **monitored_regions; /* Regions being monitored */
  gsize monitored_regions_count;
  gchar **relay_hints;       /* Relays where monitor publishes */
  gsize relay_hints_count;

  /* Timestamps */
  gint64 created_at;
  gint64 cached_at;
} GnostrNip66RelayMonitor;

/* ============== Memory Management ============== */

/**
 * gnostr_nip66_relay_meta_free:
 * @meta: Relay metadata to free
 *
 * Frees all memory associated with relay metadata.
 */
void gnostr_nip66_relay_meta_free(GnostrNip66RelayMeta *meta);

/**
 * gnostr_nip66_relay_monitor_free:
 * @monitor: Relay monitor to free
 *
 * Frees all memory associated with relay monitor.
 */
void gnostr_nip66_relay_monitor_free(GnostrNip66RelayMonitor *monitor);

/* ============== Parsing ============== */

/**
 * gnostr_nip66_parse_relay_meta:
 * @event_json: JSON string of the kind 30166 event
 *
 * Parses a kind 30166 (relay metadata) event.
 *
 * Returns: (transfer full) (nullable): Parsed relay metadata or NULL on failure
 */
GnostrNip66RelayMeta *gnostr_nip66_parse_relay_meta(const gchar *event_json);

/**
 * gnostr_nip66_parse_relay_monitor:
 * @event_json: JSON string of the kind 10166 event
 *
 * Parses a kind 10166 (relay monitor) event.
 *
 * Returns: (transfer full) (nullable): Parsed relay monitor or NULL on failure
 */
GnostrNip66RelayMonitor *gnostr_nip66_parse_relay_monitor(const gchar *event_json);

/**
 * gnostr_nip66_parse_network:
 * @network_str: Network string (e.g., "clearnet", "tor", "i2p")
 *
 * Converts network string to enum value.
 *
 * Returns: Network enum value
 */
GnostrNip66RelayNetwork gnostr_nip66_parse_network(const gchar *network_str);

/**
 * gnostr_nip66_network_to_string:
 * @network: Network enum value
 *
 * Converts network enum to display string.
 *
 * Returns: (transfer none): Static string for the network type
 */
const gchar *gnostr_nip66_network_to_string(GnostrNip66RelayNetwork network);

/* ============== Cache Management ============== */

/**
 * gnostr_nip66_cache_init:
 *
 * Initializes the NIP-66 relay discovery cache. Call once at startup.
 */
void gnostr_nip66_cache_init(void);

/**
 * gnostr_nip66_cache_shutdown:
 *
 * Cleans up and frees the NIP-66 relay discovery cache.
 */
void gnostr_nip66_cache_shutdown(void);

/**
 * gnostr_nip66_cache_add_relay:
 * @meta: Relay metadata to cache (takes ownership)
 *
 * Adds or updates a relay in the cache.
 */
void gnostr_nip66_cache_add_relay(GnostrNip66RelayMeta *meta);

/**
 * gnostr_nip66_cache_add_monitor:
 * @monitor: Relay monitor to cache (takes ownership)
 *
 * Adds or updates a monitor in the cache.
 */
void gnostr_nip66_cache_add_monitor(GnostrNip66RelayMonitor *monitor);

/**
 * gnostr_nip66_cache_get_relay:
 * @relay_url: Relay URL to look up
 *
 * Gets cached relay metadata by URL.
 *
 * Returns: (transfer none) (nullable): Cached relay metadata or NULL if not found
 */
GnostrNip66RelayMeta *gnostr_nip66_cache_get_relay(const gchar *relay_url);

/**
 * gnostr_nip66_cache_get_all_relays:
 *
 * Gets all cached relays.
 *
 * Returns: (transfer container) (element-type GnostrNip66RelayMeta):
 *          Array of relay metadata pointers. Free with g_ptr_array_unref().
 */
GPtrArray *gnostr_nip66_cache_get_all_relays(void);

/**
 * gnostr_nip66_cache_get_all_monitors:
 *
 * Gets all cached monitors.
 *
 * Returns: (transfer container) (element-type GnostrNip66RelayMonitor):
 *          Array of monitor pointers. Free with g_ptr_array_unref().
 */
GPtrArray *gnostr_nip66_cache_get_all_monitors(void);

/**
 * gnostr_nip66_cache_clear:
 *
 * Clears all cached relay and monitor data.
 */
void gnostr_nip66_cache_clear(void);

/* ============== Query/Filter API ============== */

/**
 * GnostrNip66FilterFlags:
 * Flags for filtering relay results
 */
typedef enum {
  GNOSTR_NIP66_FILTER_NONE          = 0,
  GNOSTR_NIP66_FILTER_ONLINE_ONLY   = 1 << 0,  /* Only online relays */
  GNOSTR_NIP66_FILTER_FREE_ONLY     = 1 << 1,  /* No payment required */
  GNOSTR_NIP66_FILTER_NO_AUTH       = 1 << 2,  /* No auth required */
  GNOSTR_NIP66_FILTER_CLEARNET_ONLY = 1 << 3,  /* Clearnet only (no Tor/I2P) */
} GnostrNip66FilterFlags;

/**
 * GnostrNip66RelayFilter:
 * Filter criteria for relay queries
 */
typedef struct {
  GnostrNip66FilterFlags flags;   /* Filter flags */
  const gchar *region;            /* Geographic region filter (NULL for any) */
  const gchar *country_code;      /* Country code filter (NULL for any) */
  const gint *required_nips;      /* Required NIPs array (NULL for any) */
  gsize required_nips_count;      /* Number of required NIPs */
  gdouble min_uptime_percent;     /* Minimum uptime (0 to disable) */
  gint64 max_latency_ms;          /* Maximum latency (0 to disable) */
} GnostrNip66RelayFilter;

/**
 * gnostr_nip66_filter_relays:
 * @filter: (nullable): Filter criteria, or NULL for no filtering
 *
 * Filters cached relays by the given criteria.
 *
 * Returns: (transfer container) (element-type GnostrNip66RelayMeta):
 *          Array of matching relay metadata. Free with g_ptr_array_unref().
 */
GPtrArray *gnostr_nip66_filter_relays(const GnostrNip66RelayFilter *filter);

/**
 * gnostr_nip66_relay_supports_nip:
 * @meta: Relay metadata
 * @nip: NIP number to check
 *
 * Checks if a relay supports a specific NIP.
 *
 * Returns: TRUE if the relay supports the NIP
 */
gboolean gnostr_nip66_relay_supports_nip(const GnostrNip66RelayMeta *meta, gint nip);

/* ============== Async Discovery API ============== */

/**
 * GnostrNip66DiscoveryCallback:
 * @relays: (element-type GnostrNip66RelayMeta): Discovered relays
 * @monitors: (element-type GnostrNip66RelayMonitor): Discovered monitors
 * @error: (nullable): Error if discovery failed
 * @user_data: User data
 *
 * Callback for async relay discovery operations.
 */
typedef void (*GnostrNip66DiscoveryCallback)(GPtrArray *relays,
                                              GPtrArray *monitors,
                                              GError *error,
                                              gpointer user_data);

/**
 * gnostr_nip66_discover_relays_async:
 * @callback: Callback when discovery completes
 * @user_data: User data for callback
 * @cancellable: (nullable): Optional cancellable
 *
 * Discovers relays by querying known relay monitors.
 * Queries configured relays for kind 30166 and 10166 events.
 * Results are automatically cached.
 */
void gnostr_nip66_discover_relays_async(GnostrNip66DiscoveryCallback callback,
                                         gpointer user_data,
                                         GCancellable *cancellable);

/**
 * gnostr_nip66_discover_from_monitors_async:
 * @monitor_pubkeys: Array of monitor pubkeys to query
 * @n_pubkeys: Number of pubkeys
 * @callback: Callback when discovery completes
 * @user_data: User data for callback
 * @cancellable: (nullable): Optional cancellable
 *
 * Discovers relays from specific monitors.
 */
void gnostr_nip66_discover_from_monitors_async(const gchar **monitor_pubkeys,
                                                 gsize n_pubkeys,
                                                 GnostrNip66DiscoveryCallback callback,
                                                 gpointer user_data,
                                                 GCancellable *cancellable);

/* ============== Well-Known Monitors ============== */

/**
 * gnostr_nip66_get_known_monitors:
 *
 * Gets a list of well-known relay monitor pubkeys.
 *
 * Returns: (transfer none): NULL-terminated array of pubkey hex strings
 */
const gchar **gnostr_nip66_get_known_monitors(void);

/**
 * gnostr_nip66_get_known_monitor_relays:
 *
 * Gets a list of relays where monitors publish their data.
 *
 * Returns: (transfer none): NULL-terminated array of relay URLs
 */
const gchar **gnostr_nip66_get_known_monitor_relays(void);

/* ============== Filter JSON Building ============== */

/**
 * gnostr_nip66_build_relay_meta_filter:
 * @relay_urls: (nullable): Specific relay URLs to query, or NULL for all
 * @n_urls: Number of URLs
 * @limit: Maximum results (0 for default)
 *
 * Builds a NIP-01 filter JSON for querying kind 30166 events.
 *
 * Returns: (transfer full): Filter JSON string. Caller must g_free().
 */
gchar *gnostr_nip66_build_relay_meta_filter(const gchar **relay_urls,
                                             gsize n_urls,
                                             gint limit);

/**
 * gnostr_nip66_build_monitor_filter:
 * @monitor_pubkeys: (nullable): Specific monitor pubkeys, or NULL for all
 * @n_pubkeys: Number of pubkeys
 *
 * Builds a NIP-01 filter JSON for querying kind 10166 events.
 *
 * Returns: (transfer full): Filter JSON string. Caller must g_free().
 */
gchar *gnostr_nip66_build_monitor_filter(const gchar **monitor_pubkeys,
                                          gsize n_pubkeys);

/* ============== Formatting Helpers ============== */

/**
 * gnostr_nip66_format_uptime:
 * @uptime_percent: Uptime percentage (0-100)
 *
 * Formats uptime as a human-readable string.
 *
 * Returns: (transfer full): Formatted string (e.g., "99.5%")
 */
gchar *gnostr_nip66_format_uptime(gdouble uptime_percent);

/**
 * gnostr_nip66_format_latency:
 * @latency_ms: Latency in milliseconds
 *
 * Formats latency as a human-readable string.
 *
 * Returns: (transfer full): Formatted string (e.g., "45ms", "1.2s")
 */
gchar *gnostr_nip66_format_latency(gint64 latency_ms);

/**
 * gnostr_nip66_format_last_seen:
 * @last_seen: Unix timestamp
 *
 * Formats last seen time as relative string.
 *
 * Returns: (transfer full): Formatted string (e.g., "5 minutes ago", "2 hours ago")
 */
gchar *gnostr_nip66_format_last_seen(gint64 last_seen);

/**
 * gnostr_nip66_format_nips:
 * @meta: Relay metadata
 *
 * Formats supported NIPs as a comma-separated string.
 *
 * Returns: (transfer full): Formatted string or "(none)"
 */
gchar *gnostr_nip66_format_nips(const GnostrNip66RelayMeta *meta);

/**
 * gnostr_nip66_get_region_for_country:
 * @country_code: ISO 3166-1 alpha-2 country code
 *
 * Maps a country code to its geographic region.
 *
 * Returns: (transfer none): Region string (e.g., "North America", "Europe")
 */
const gchar *gnostr_nip66_get_region_for_country(const gchar *country_code);

G_END_DECLS

#endif /* GNOSTR_NIP66_RELAY_DISCOVERY_H */
