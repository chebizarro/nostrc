/**
 * NIP-89 App Handlers - Application Handler Recommendations
 *
 * NIP-89 defines two event kinds:
 * - Kind 31989: Application handler information (published by app developers)
 * - Kind 31990: User's recommended handlers for specific event kinds
 *
 * This module provides:
 * - Parsing of kind 31989 and 31990 events
 * - Cache management for discovered handlers
 * - Query API for finding handlers by event kind
 * - User preference storage and retrieval
 */

#ifndef GNOSTR_NIP89_HANDLERS_H
#define GNOSTR_NIP89_HANDLERS_H

#include <glib.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* ============== Event Kind Constants ============== */

#define GNOSTR_NIP89_KIND_HANDLER_INFO       31989
#define GNOSTR_NIP89_KIND_HANDLER_RECOMMEND  31990

/* ============== Platform Types ============== */

/**
 * GnostrNip89Platform:
 * Platform types for app handlers
 */
typedef enum {
  GNOSTR_NIP89_PLATFORM_UNKNOWN = 0,
  GNOSTR_NIP89_PLATFORM_WEB,        /* Browser-based app */
  GNOSTR_NIP89_PLATFORM_IOS,        /* iOS native app */
  GNOSTR_NIP89_PLATFORM_ANDROID,    /* Android native app */
  GNOSTR_NIP89_PLATFORM_MACOS,      /* macOS native app */
  GNOSTR_NIP89_PLATFORM_WINDOWS,    /* Windows native app */
  GNOSTR_NIP89_PLATFORM_LINUX,      /* Linux native app */
} GnostrNip89Platform;

/* ============== Handler Information (kind 31989) ============== */

/**
 * GnostrNip89HandlerInfo:
 * Information about an application handler (from kind 31989 events).
 *
 * Published by app developers to advertise their application's capabilities.
 */
typedef struct {
  char *event_id_hex;       /* Event ID of the handler info */
  char *pubkey_hex;         /* Pubkey of the app developer/publisher */
  char *d_tag;              /* Unique identifier (usually app identifier) */

  /* Profile-like metadata (from content JSON) */
  char *name;               /* App display name */
  char *display_name;       /* Alternative display name */
  char *picture;            /* App icon URL */
  char *about;              /* App description */
  char *banner;             /* Banner image URL */
  char *website;            /* App website URL */
  char *nip05;              /* NIP-05 identifier for the app */
  char *lud16;              /* Lightning address for app developer */

  /* Handler-specific tags */
  guint *handled_kinds;     /* Array of event kinds this app handles */
  gsize n_handled_kinds;    /* Number of handled kinds */

  /* Platform-specific URLs/identifiers */
  GPtrArray *platforms;     /* Array of GnostrNip89PlatformHandler */

  /* Timestamps */
  gint64 created_at;        /* When this handler info was published */
  gint64 cached_at;         /* When we cached this locally */
} GnostrNip89HandlerInfo;

/**
 * GnostrNip89PlatformHandler:
 * Platform-specific handler entry with URL template
 */
typedef struct {
  GnostrNip89Platform platform;
  char *platform_name;      /* Raw platform string (e.g., "web", "ios") */
  char *url_template;       /* URL template with <bech32> placeholder */
  char *identifier;         /* App store identifier (for mobile apps) */
} GnostrNip89PlatformHandler;

/* ============== Handler Recommendations (kind 31990) ============== */

/**
 * GnostrNip89Recommendation:
 * A user's recommendation for a specific event kind (from kind 31990 events).
 */
typedef struct {
  char *event_id_hex;       /* Event ID of the recommendation */
  char *pubkey_hex;         /* Pubkey of the user making the recommendation */
  char *d_tag;              /* Event kind being recommended for (as string) */
  guint recommended_kind;   /* Parsed event kind */

  /* Referenced handler (from "a" tag) */
  char *handler_a_tag;      /* "31989:pubkey:d-tag" coordinate */
  char *handler_pubkey;     /* Extracted handler pubkey */
  char *handler_d_tag;      /* Extracted handler d-tag */

  /* Optional relay hint */
  char *relay_hint;         /* Relay URL where handler info might be found */

  /* Timestamps */
  gint64 created_at;
  gint64 cached_at;
} GnostrNip89Recommendation;

/* ============== Memory Management ============== */

/**
 * gnostr_nip89_handler_info_free:
 * @info: Handler info to free
 *
 * Frees all memory associated with a handler info structure.
 */
void gnostr_nip89_handler_info_free(GnostrNip89HandlerInfo *info);

/**
 * gnostr_nip89_platform_handler_free:
 * @handler: Platform handler to free
 *
 * Frees a platform-specific handler entry.
 */
void gnostr_nip89_platform_handler_free(GnostrNip89PlatformHandler *handler);

/**
 * gnostr_nip89_recommendation_free:
 * @rec: Recommendation to free
 *
 * Frees all memory associated with a recommendation.
 */
void gnostr_nip89_recommendation_free(GnostrNip89Recommendation *rec);

/* ============== Parsing ============== */

/**
 * gnostr_nip89_parse_handler_info:
 * @event_json: JSON string of the kind 31989 event
 *
 * Parses a kind 31989 (application handler information) event.
 *
 * Returns: (transfer full) (nullable): Parsed handler info or NULL on failure
 */
GnostrNip89HandlerInfo *gnostr_nip89_parse_handler_info(const char *event_json);

/**
 * gnostr_nip89_parse_recommendation:
 * @event_json: JSON string of the kind 31990 event
 *
 * Parses a kind 31990 (handler recommendation) event.
 *
 * Returns: (transfer full) (nullable): Parsed recommendation or NULL on failure
 */
GnostrNip89Recommendation *gnostr_nip89_parse_recommendation(const char *event_json);

/**
 * gnostr_nip89_parse_platform:
 * @platform_str: Platform string (e.g., "web", "ios", "android")
 *
 * Converts platform string to enum value.
 *
 * Returns: Platform enum value
 */
GnostrNip89Platform gnostr_nip89_parse_platform(const char *platform_str);

/**
 * gnostr_nip89_platform_to_string:
 * @platform: Platform enum value
 *
 * Converts platform enum to display string.
 *
 * Returns: (transfer none): Static string for the platform
 */
const char *gnostr_nip89_platform_to_string(GnostrNip89Platform platform);

/* ============== URL Generation ============== */

/**
 * gnostr_nip89_build_handler_url:
 * @handler: Handler info
 * @platform: Target platform
 * @event_bech32: Bech32-encoded event reference (nevent, naddr, etc.)
 *
 * Builds the URL to open an event in the specified handler.
 * Replaces <bech32> placeholder in URL template.
 *
 * Returns: (transfer full) (nullable): URL string or NULL if platform not supported
 */
char *gnostr_nip89_build_handler_url(GnostrNip89HandlerInfo *handler,
                                      GnostrNip89Platform platform,
                                      const char *event_bech32);

/**
 * gnostr_nip89_get_current_platform:
 *
 * Detects the current platform at runtime.
 *
 * Returns: Current platform enum value
 */
GnostrNip89Platform gnostr_nip89_get_current_platform(void);

/* ============== Cache Management ============== */

/**
 * gnostr_nip89_cache_init:
 *
 * Initializes the NIP-89 handler cache. Call once at startup.
 */
void gnostr_nip89_cache_init(void);

/**
 * gnostr_nip89_cache_shutdown:
 *
 * Cleans up and frees the NIP-89 handler cache.
 */
void gnostr_nip89_cache_shutdown(void);

/**
 * gnostr_nip89_cache_add_handler:
 * @info: Handler info to cache (takes ownership)
 *
 * Adds or updates a handler in the cache.
 */
void gnostr_nip89_cache_add_handler(GnostrNip89HandlerInfo *info);

/**
 * gnostr_nip89_cache_add_recommendation:
 * @rec: Recommendation to cache (takes ownership)
 *
 * Adds or updates a recommendation in the cache.
 */
void gnostr_nip89_cache_add_recommendation(GnostrNip89Recommendation *rec);

/**
 * gnostr_nip89_cache_get_handlers_for_kind:
 * @event_kind: The event kind to find handlers for
 *
 * Finds all cached handlers that support the given event kind.
 *
 * Returns: (transfer container) (element-type GnostrNip89HandlerInfo):
 *          Array of handler info pointers. Free with g_ptr_array_unref().
 */
GPtrArray *gnostr_nip89_cache_get_handlers_for_kind(guint event_kind);

/**
 * gnostr_nip89_cache_get_recommendations_for_kind:
 * @event_kind: The event kind to find recommendations for
 * @user_pubkey: (nullable): Optional user pubkey to filter by
 *
 * Finds recommendations for a specific event kind.
 *
 * Returns: (transfer container): Array of recommendations. Free with g_ptr_array_unref().
 */
GPtrArray *gnostr_nip89_cache_get_recommendations_for_kind(guint event_kind,
                                                            const char *user_pubkey);

/**
 * gnostr_nip89_cache_get_handler_by_a_tag:
 * @a_tag: The "a" tag coordinate (31989:pubkey:d-tag)
 *
 * Looks up a specific handler by its NIP-33 coordinate.
 *
 * Returns: (transfer none) (nullable): Handler info or NULL if not found
 */
GnostrNip89HandlerInfo *gnostr_nip89_cache_get_handler_by_a_tag(const char *a_tag);

/**
 * gnostr_nip89_cache_get_all_handlers:
 *
 * Gets all cached handlers.
 *
 * Returns: (transfer container): Array of all cached handlers
 */
GPtrArray *gnostr_nip89_cache_get_all_handlers(void);

/* ============== User Preferences ============== */

/**
 * gnostr_nip89_get_preferred_handler:
 * @event_kind: Event kind to get preference for
 *
 * Gets the user's preferred handler for a specific event kind.
 * Preference is stored in local settings.
 *
 * Returns: (transfer none) (nullable): Preferred handler or NULL if no preference set
 */
GnostrNip89HandlerInfo *gnostr_nip89_get_preferred_handler(guint event_kind);

/**
 * gnostr_nip89_set_preferred_handler:
 * @event_kind: Event kind to set preference for
 * @handler_a_tag: Handler "a" tag coordinate, or NULL to clear preference
 *
 * Sets the user's preferred handler for a specific event kind.
 */
void gnostr_nip89_set_preferred_handler(guint event_kind, const char *handler_a_tag);

/**
 * gnostr_nip89_clear_all_preferences:
 *
 * Clears all handler preferences.
 */
void gnostr_nip89_clear_all_preferences(void);

/* ============== Subscription/Query Helpers ============== */

/**
 * gnostr_nip89_build_handler_filter:
 * @kinds: (nullable): Array of event kinds to find handlers for, or NULL for all
 * @n_kinds: Number of kinds in array
 *
 * Builds a NIP-01 filter JSON for querying kind 31989 events.
 *
 * Returns: (transfer full): Filter JSON string. Caller must g_free().
 */
char *gnostr_nip89_build_handler_filter(const guint *kinds, gsize n_kinds);

/**
 * gnostr_nip89_build_recommendation_filter:
 * @event_kind: Event kind to find recommendations for
 * @followed_pubkeys: (nullable): Array of followed pubkeys to filter by
 * @n_pubkeys: Number of pubkeys
 *
 * Builds a NIP-01 filter JSON for querying kind 31990 events.
 *
 * Returns: (transfer full): Filter JSON string. Caller must g_free().
 */
char *gnostr_nip89_build_recommendation_filter(guint event_kind,
                                                const char **followed_pubkeys,
                                                gsize n_pubkeys);

/* ============== Async Query API ============== */

/**
 * GnostrNip89QueryCallback:
 * @handlers: (element-type GnostrNip89HandlerInfo): Array of found handlers
 * @recommendations: (element-type GnostrNip89Recommendation): Array of recommendations
 * @error: (nullable): Error if query failed
 * @user_data: User data
 *
 * Callback for async handler queries.
 */
typedef void (*GnostrNip89QueryCallback)(GPtrArray *handlers,
                                          GPtrArray *recommendations,
                                          GError *error,
                                          gpointer user_data);

/**
 * gnostr_nip89_query_handlers_async:
 * @event_kind: Event kind to query handlers for
 * @callback: Callback when query completes
 * @user_data: User data for callback
 * @cancellable: (nullable): Optional cancellable
 *
 * Queries relays for handlers that support the given event kind.
 * Results are automatically cached.
 */
void gnostr_nip89_query_handlers_async(guint event_kind,
                                        GnostrNip89QueryCallback callback,
                                        gpointer user_data,
                                        GCancellable *cancellable);

/* ============== Kind Description Helpers ============== */

/**
 * gnostr_nip89_get_kind_description:
 * @kind: Event kind number
 *
 * Gets a human-readable description of an event kind.
 *
 * Returns: (transfer none): Description string (static)
 */
const char *gnostr_nip89_get_kind_description(guint kind);

/**
 * gnostr_nip89_is_replaceable_kind:
 * @kind: Event kind number
 *
 * Checks if a kind is replaceable (10000-19999 or 0,3).
 *
 * Returns: TRUE if replaceable
 */
gboolean gnostr_nip89_is_replaceable_kind(guint kind);

/**
 * gnostr_nip89_is_ephemeral_kind:
 * @kind: Event kind number
 *
 * Checks if a kind is ephemeral (20000-29999).
 *
 * Returns: TRUE if ephemeral
 */
gboolean gnostr_nip89_is_ephemeral_kind(guint kind);

/**
 * gnostr_nip89_is_addressable_kind:
 * @kind: Event kind number
 *
 * Checks if a kind is parameterized replaceable/addressable (30000-39999).
 *
 * Returns: TRUE if addressable
 */
gboolean gnostr_nip89_is_addressable_kind(guint kind);

G_END_DECLS

#endif /* GNOSTR_NIP89_HANDLERS_H */
