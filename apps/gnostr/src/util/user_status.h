/**
 * NIP-38: User Statuses
 *
 * User status events (kind 30315) allow users to share ephemeral status updates.
 * Status types include "general" (current activity) and "music" (currently playing).
 *
 * Event structure:
 * - kind: 30315
 * - d tag: status type ("general" or "music")
 * - content: status text
 * - r tag (optional): link/URL
 * - expiration tag (optional, NIP-40): auto-expire timestamp
 */

#ifndef GNOSTR_USER_STATUS_H
#define GNOSTR_USER_STATUS_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * GnostrUserStatusType:
 * @GNOSTR_STATUS_GENERAL: General activity status
 * @GNOSTR_STATUS_MUSIC: Currently playing music
 *
 * Types of user status as defined by NIP-38.
 */
typedef enum {
  GNOSTR_STATUS_GENERAL = 0,
  GNOSTR_STATUS_MUSIC   = 1,
} GnostrUserStatusType;

/**
 * GnostrUserStatus:
 *
 * Structure representing a user's status (NIP-38).
 */
typedef struct {
  gchar *pubkey_hex;        /* Author's pubkey (64-char hex) */
  GnostrUserStatusType type; /* Status type (general/music) */
  gchar *content;           /* Status text */
  gchar *link_url;          /* Optional link (r tag) */
  gint64 created_at;        /* Event timestamp */
  gint64 expiration;        /* NIP-40 expiration (0 = no expiration) */
  gchar *event_id;          /* Event ID for reference */
} GnostrUserStatus;

/**
 * gnostr_user_status_free:
 * @status: (transfer full) (nullable): Status to free
 *
 * Frees a user status structure.
 */
void gnostr_user_status_free(GnostrUserStatus *status);

/**
 * gnostr_user_status_copy:
 * @status: (nullable): Status to copy
 *
 * Deep-copies a user status structure.
 *
 * Returns: (transfer full) (nullable): New copy, or NULL
 */
GnostrUserStatus *gnostr_user_status_copy(const GnostrUserStatus *status);

/**
 * gnostr_user_status_is_expired:
 * @status: Status to check
 *
 * Checks if the status has expired (NIP-40).
 *
 * Returns: TRUE if expired, FALSE otherwise
 */
gboolean gnostr_user_status_is_expired(const GnostrUserStatus *status);

/**
 * gnostr_user_status_type_to_string:
 * @type: Status type
 *
 * Converts status type to its "d" tag value.
 *
 * Returns: (transfer none): "general" or "music"
 */
const gchar *gnostr_user_status_type_to_string(GnostrUserStatusType type);

/**
 * gnostr_user_status_type_from_string:
 * @str: "d" tag value
 *
 * Parses status type from "d" tag value.
 *
 * Returns: Status type (defaults to GENERAL if unknown)
 */
GnostrUserStatusType gnostr_user_status_type_from_string(const gchar *str);

/* ============== Parsing API ============== */

/**
 * gnostr_user_status_parse_event:
 * @event_json: Kind 30315 event JSON
 *
 * Parses a user status from event JSON.
 *
 * Returns: (transfer full) (nullable): Parsed status, or NULL on error
 */
GnostrUserStatus *gnostr_user_status_parse_event(const gchar *event_json);

/* ============== Cache API ============== */

/**
 * gnostr_user_status_cache_init:
 *
 * Initialize the user status cache. Called once at startup.
 */
void gnostr_user_status_cache_init(void);

/**
 * gnostr_user_status_cache_shutdown:
 *
 * Shutdown the user status cache. Called at app exit.
 */
void gnostr_user_status_cache_shutdown(void);

/**
 * gnostr_user_status_cache_get:
 * @pubkey_hex: User's pubkey (64-char hex)
 * @type: Status type to get
 *
 * Gets cached status for a user. Returns NULL if not cached or expired.
 *
 * Returns: (transfer full) (nullable): Cached status (caller owns), or NULL
 */
GnostrUserStatus *gnostr_user_status_cache_get(const gchar *pubkey_hex,
                                                GnostrUserStatusType type);

/**
 * gnostr_user_status_cache_set:
 * @status: (transfer none): Status to cache
 *
 * Caches a user status. Replaces existing status of same type for same user.
 */
void gnostr_user_status_cache_set(const GnostrUserStatus *status);

/**
 * gnostr_user_status_cache_remove:
 * @pubkey_hex: User's pubkey
 * @type: Status type to remove
 *
 * Removes a specific status from cache.
 */
void gnostr_user_status_cache_remove(const gchar *pubkey_hex,
                                      GnostrUserStatusType type);

/* ============== Fetch API ============== */

/**
 * GnostrUserStatusCallback:
 * @statuses: (element-type GnostrUserStatus) (transfer full) (nullable): Array of statuses
 * @user_data: User data
 *
 * Callback for async status fetch operations.
 * The array and its contents are owned by the caller.
 */
typedef void (*GnostrUserStatusCallback)(GPtrArray *statuses, gpointer user_data);

/**
 * gnostr_user_status_fetch_async:
 * @pubkey_hex: User's pubkey (64-char hex)
 * @cancellable: (nullable): Cancellable
 * @callback: Callback when complete
 * @user_data: User data for callback
 *
 * Fetches user statuses from relays asynchronously.
 * First checks cache, then queries relays if needed.
 * Results are automatically cached.
 */
void gnostr_user_status_fetch_async(const gchar *pubkey_hex,
                                     GCancellable *cancellable,
                                     GnostrUserStatusCallback callback,
                                     gpointer user_data);

/* ============== Publish API ============== */

/**
 * GnostrUserStatusPublishCallback:
 * @success: Whether publish succeeded
 * @error_msg: (nullable): Error message if failed
 * @user_data: User data
 *
 * Callback for async status publish operations.
 */
typedef void (*GnostrUserStatusPublishCallback)(gboolean success,
                                                 const gchar *error_msg,
                                                 gpointer user_data);

/**
 * gnostr_user_status_publish_async:
 * @type: Status type
 * @content: Status text (empty string to clear status)
 * @link_url: (nullable): Optional link URL
 * @expiration_seconds: Seconds until expiration (0 = no expiration)
 * @callback: (nullable): Callback when complete
 * @user_data: User data for callback
 *
 * Publishes a user status to relays.
 * Pass empty content to clear the status.
 */
void gnostr_user_status_publish_async(GnostrUserStatusType type,
                                       const gchar *content,
                                       const gchar *link_url,
                                       gint64 expiration_seconds,
                                       GnostrUserStatusPublishCallback callback,
                                       gpointer user_data);

/**
 * gnostr_user_status_clear_async:
 * @type: Status type to clear
 * @callback: (nullable): Callback when complete
 * @user_data: User data for callback
 *
 * Clears (deletes) a user status by publishing an empty one.
 */
void gnostr_user_status_clear_async(GnostrUserStatusType type,
                                     GnostrUserStatusPublishCallback callback,
                                     gpointer user_data);

/**
 * gnostr_user_status_build_event_json:
 * @type: Status type
 * @content: Status text
 * @link_url: (nullable): Optional link URL
 * @expiration_seconds: Seconds until expiration (0 = no expiration)
 *
 * Builds an unsigned kind 30315 event JSON for signing.
 *
 * Returns: (transfer full): Unsigned event JSON string (caller frees)
 */
gchar *gnostr_user_status_build_event_json(GnostrUserStatusType type,
                                            const gchar *content,
                                            const gchar *link_url,
                                            gint64 expiration_seconds);

G_END_DECLS

#endif /* GNOSTR_USER_STATUS_H */
