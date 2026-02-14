#ifndef APPS_GNOSTR_UTIL_RELAY_INFO_H
#define APPS_GNOSTR_UTIL_RELAY_INFO_H

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
  gint64 created_at_lower_limit;  /* Oldest event timestamp accepted (seconds before now) */
  gint64 created_at_upper_limit;  /* Newest event timestamp accepted (seconds after now) */
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

/* ---- Event Validation Against Relay Limits (NIP-11) ---- */

/**
 * GnostrRelayLimitViolation:
 * @GNOSTR_LIMIT_NONE: No violation
 * @GNOSTR_LIMIT_CONTENT_LENGTH: Content exceeds max_content_length
 * @GNOSTR_LIMIT_EVENT_TAGS: Too many tags (exceeds max_event_tags)
 * @GNOSTR_LIMIT_MESSAGE_LENGTH: Serialized message exceeds max_message_length
 * @GNOSTR_LIMIT_TIMESTAMP_TOO_OLD: created_at is older than created_at_lower_limit
 * @GNOSTR_LIMIT_TIMESTAMP_TOO_NEW: created_at is newer than created_at_upper_limit
 * @GNOSTR_LIMIT_POW_REQUIRED: Event requires proof-of-work (min_pow_difficulty)
 * @GNOSTR_LIMIT_AUTH_REQUIRED: Relay requires authentication
 * @GNOSTR_LIMIT_PAYMENT_REQUIRED: Relay requires payment
 * @GNOSTR_LIMIT_RESTRICTED_WRITES: Relay has restricted writes
 *
 * Types of relay limit violations that can occur when validating an event.
 */
typedef enum {
  GNOSTR_LIMIT_NONE = 0,
  GNOSTR_LIMIT_CONTENT_LENGTH = 1 << 0,
  GNOSTR_LIMIT_EVENT_TAGS = 1 << 1,
  GNOSTR_LIMIT_MESSAGE_LENGTH = 1 << 2,
  GNOSTR_LIMIT_TIMESTAMP_TOO_OLD = 1 << 3,
  GNOSTR_LIMIT_TIMESTAMP_TOO_NEW = 1 << 4,
  GNOSTR_LIMIT_POW_REQUIRED = 1 << 5,
  GNOSTR_LIMIT_AUTH_REQUIRED = 1 << 6,
  GNOSTR_LIMIT_PAYMENT_REQUIRED = 1 << 7,
  GNOSTR_LIMIT_RESTRICTED_WRITES = 1 << 8,
} GnostrRelayLimitViolation;

/**
 * GnostrRelayValidationResult:
 *
 * Result of validating an event against relay limitations.
 * Contains both the violation flags and human-readable messages.
 */
typedef struct _GnostrRelayValidationResult GnostrRelayValidationResult;

struct _GnostrRelayValidationResult {
  GnostrRelayLimitViolation violations;  /* Bitmask of violations */
  gchar *relay_url;                      /* Relay URL that was checked */
  gchar *relay_name;                     /* Relay name if available */
  /* Detailed violation info */
  gint content_length;                   /* Actual content length */
  gint max_content_length;               /* Relay's limit */
  gint tag_count;                        /* Actual tag count */
  gint max_tags;                         /* Relay's limit */
  gint message_length;                   /* Actual serialized message length */
  gint max_message_length;               /* Relay's limit */
  gint64 event_created_at;               /* Event timestamp */
  gint64 min_allowed_timestamp;          /* Oldest allowed timestamp */
  gint64 max_allowed_timestamp;          /* Newest allowed timestamp */
};

/**
 * gnostr_relay_validation_result_new:
 *
 * Creates a new validation result.
 *
 * Returns: (transfer full): A newly allocated GnostrRelayValidationResult.
 */
GnostrRelayValidationResult *gnostr_relay_validation_result_new(void);

/**
 * gnostr_relay_validation_result_free:
 * @result: (nullable): A validation result to free.
 *
 * Frees all resources associated with a validation result.
 */
void gnostr_relay_validation_result_free(GnostrRelayValidationResult *result);

/**
 * gnostr_relay_validation_result_is_valid:
 * @result: A validation result.
 *
 * Checks if the validation passed (no violations).
 *
 * Returns: TRUE if no violations, FALSE otherwise.
 */
gboolean gnostr_relay_validation_result_is_valid(const GnostrRelayValidationResult *result);

/**
 * gnostr_relay_validation_result_format_errors:
 * @result: A validation result.
 *
 * Formats all violations as a human-readable string.
 *
 * Returns: (transfer full): Formatted error string or NULL if valid.
 */
gchar *gnostr_relay_validation_result_format_errors(const GnostrRelayValidationResult *result);

/**
 * gnostr_relay_info_validate_event:
 * @info: (nullable): Relay info with limitations. If NULL, returns valid result.
 * @content: (nullable): Event content string.
 * @content_length: Length of content in bytes (-1 to calculate from content).
 * @tag_count: Number of tags in the event.
 * @created_at: Event timestamp (Unix seconds).
 * @serialized_length: Length of serialized event message (-1 if unknown).
 *
 * Validates event parameters against relay limitations.
 * This is a lightweight check that can be done before signing.
 *
 * Returns: (transfer full): Validation result. Caller must free with
 *          gnostr_relay_validation_result_free().
 */
GnostrRelayValidationResult *gnostr_relay_info_validate_event(
    const GnostrRelayInfo *info,
    const gchar *content,
    gssize content_length,
    gint tag_count,
    gint64 created_at,
    gssize serialized_length);

/**
 * gnostr_relay_info_validate_for_publishing:
 * @info: (nullable): Relay info with limitations.
 *
 * Checks if a relay allows publishing based on its limitations.
 * This checks auth_required, payment_required, and restricted_writes flags.
 *
 * Returns: (transfer full): Validation result with any policy violations.
 */
GnostrRelayValidationResult *gnostr_relay_info_validate_for_publishing(
    const GnostrRelayInfo *info);

G_END_DECLS
#endif /* APPS_GNOSTR_UTIL_RELAY_INFO_H */
