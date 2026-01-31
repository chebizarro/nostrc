/**
 * NIP-78 App-Specific Data Support
 *
 * Provides parsing and creation of kind 30078 events for arbitrary
 * application-specific data storage on Nostr relays.
 *
 * NIP-78 defines:
 * - kind 30078: Arbitrary custom app data (parameterized replaceable)
 * - Tags: ["d", "app-identifier/data-key"]
 * - Content: Arbitrary data (often JSON)
 * - Used for app settings, state, preferences sync across devices
 *
 * The d-tag format is "app-identifier/data-key" where:
 * - app-identifier: unique identifier for the application (e.g., "gnostr")
 * - data-key: identifies the type of data (e.g., "preferences", "mutes")
 */

#ifndef GNOSTR_NIP78_APP_DATA_H
#define GNOSTR_NIP78_APP_DATA_H

#include <glib-object.h>
#include <gio/gio.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

/**
 * Nostr kind for application-specific data (NIP-78)
 */
#define GNOSTR_NIP78_KIND_APP_DATA 30078

/**
 * GnostrAppData:
 *
 * Represents a single NIP-78 app-specific data entry.
 * Contains parsed information from a kind 30078 event.
 */
typedef struct _GnostrAppData {
    char *app_id;         /* Application identifier (e.g., "gnostr") */
    char *data_key;       /* Data key within the app namespace */
    char *d_tag;          /* Full d-tag value (app_id/data_key) */
    char *content;        /* Raw content string (often JSON) */
    char *event_id;       /* Event ID (hex, 64 chars) */
    char *pubkey;         /* Author's public key (hex, 64 chars) */
    gint64 created_at;    /* Event creation timestamp */
} GnostrAppData;

/**
 * Callback for async app data operations
 */
typedef void (*GnostrAppDataCallback)(gboolean success,
                                       const char *error_msg,
                                       gpointer user_data);

/**
 * Callback for async app data fetch
 */
typedef void (*GnostrAppDataFetchCallback)(GnostrAppData *data,
                                            gboolean success,
                                            const char *error_msg,
                                            gpointer user_data);

/**
 * Callback for fetching multiple app data entries
 */
typedef void (*GnostrAppDataListCallback)(GPtrArray *data_list,
                                           gboolean success,
                                           const char *error_msg,
                                           gpointer user_data);

/* ---- Memory Management ---- */

/**
 * gnostr_app_data_new:
 *
 * Creates a new empty GnostrAppData structure.
 *
 * Returns: (transfer full): A new GnostrAppData, free with gnostr_app_data_free()
 */
GnostrAppData *gnostr_app_data_new(void);

/**
 * gnostr_app_data_free:
 * @data: The app data to free (may be NULL)
 *
 * Frees a GnostrAppData structure and all its resources.
 */
void gnostr_app_data_free(GnostrAppData *data);

/**
 * gnostr_app_data_copy:
 * @data: The app data to copy
 *
 * Creates a deep copy of a GnostrAppData structure.
 *
 * Returns: (transfer full) (nullable): A copy of the data, or NULL on error
 */
GnostrAppData *gnostr_app_data_copy(const GnostrAppData *data);

/* ---- Parsing ---- */

/**
 * gnostr_app_data_parse_event:
 * @event_json: JSON string of a kind 30078 event
 *
 * Parses a kind 30078 event JSON into a GnostrAppData structure.
 * Extracts the d-tag and parses it into app_id and data_key.
 * The content field is stored as raw JSON for use with helper functions.
 *
 * Returns: (transfer full) (nullable): Parsed app data, or NULL on error
 */
GnostrAppData *gnostr_app_data_parse_event(const char *event_json);

/**
 * gnostr_app_data_parse_d_tag:
 * @d_tag: The d-tag value (e.g., "gnostr/preferences")
 * @out_app_id: (out) (transfer full): Output for app ID
 * @out_data_key: (out) (transfer full): Output for data key
 *
 * Parses a d-tag string into app_id and data_key components.
 * Format: "app_id/data_key" or just "app_id" (data_key empty)
 *
 * Returns: TRUE if parsing succeeded
 */
gboolean gnostr_app_data_parse_d_tag(const char *d_tag,
                                      char **out_app_id,
                                      char **out_data_key);

/**
 * gnostr_app_data_build_d_tag:
 * @app_id: Application identifier
 * @data_key: Data key (may be NULL for app-level data)
 *
 * Builds a d-tag string from app_id and data_key.
 *
 * Returns: (transfer full): The d-tag string (e.g., "gnostr/preferences")
 */
char *gnostr_app_data_build_d_tag(const char *app_id, const char *data_key);

/* ---- Event Creation ---- */

/**
 * gnostr_app_data_build_event_json:
 * @app_id: Application identifier
 * @data_key: Data key
 * @content: Content string (often JSON)
 *
 * Builds an unsigned kind 30078 event JSON for signing.
 * The event includes:
 * - kind: 30078
 * - created_at: current timestamp
 * - tags: [["d", "app_id/data_key"]]
 * - content: the provided content
 *
 * Returns: (transfer full): Unsigned event JSON string
 */
char *gnostr_app_data_build_event_json(const char *app_id,
                                        const char *data_key,
                                        const char *content);

/**
 * gnostr_app_data_build_event_json_full:
 * @app_id: Application identifier
 * @data_key: Data key
 * @content: Content string (often JSON)
 * @extra_tags_json: (nullable): Additional tags JSON array string (e.g., "[["p","..."],["e","..."]]")
 *
 * Builds an unsigned kind 30078 event JSON with extra tags.
 *
 * Returns: (transfer full): Unsigned event JSON string
 */
char *gnostr_app_data_build_event_json_full(const char *app_id,
                                             const char *data_key,
                                             const char *content,
                                             const char *extra_tags_json);

/* ---- JSON Content Helpers ---- */

/**
 * gnostr_app_data_get_json_string:
 * @data: The app data
 * @key: JSON object key
 *
 * Gets a string value from the parsed JSON content.
 *
 * Returns: (transfer none) (nullable): String value or NULL
 */
const char *gnostr_app_data_get_json_string(const GnostrAppData *data,
                                             const char *key);

/**
 * gnostr_app_data_get_json_int:
 * @data: The app data
 * @key: JSON object key
 * @default_val: Default value if key not found
 *
 * Gets an integer value from the parsed JSON content.
 *
 * Returns: Integer value or default_val
 */
gint64 gnostr_app_data_get_json_int(const GnostrAppData *data,
                                     const char *key,
                                     gint64 default_val);

/**
 * gnostr_app_data_get_json_bool:
 * @data: The app data
 * @key: JSON object key
 * @default_val: Default value if key not found
 *
 * Gets a boolean value from the parsed JSON content.
 *
 * Returns: Boolean value or default_val
 */
gboolean gnostr_app_data_get_json_bool(const GnostrAppData *data,
                                        const char *key,
                                        gboolean default_val);

/**
 * gnostr_app_data_get_json_raw:
 * @data: The app data
 * @key: JSON object key
 *
 * Gets a raw JSON value from the content as a string.
 *
 * Returns: (transfer full) (nullable): Raw JSON value string or NULL
 */
char *gnostr_app_data_get_json_raw(const GnostrAppData *data,
                                    const char *key);

/* ---- Relay Operations ---- */

/**
 * gnostr_app_data_fetch_async:
 * @pubkey_hex: User's public key (64 hex chars)
 * @app_id: Application identifier
 * @data_key: Data key
 * @callback: Callback when fetch completes
 * @user_data: User data for callback
 *
 * Fetches app data from relays for a specific app_id and data_key.
 * Uses the latest event if multiple are found (replaceable event semantics).
 */
void gnostr_app_data_fetch_async(const char *pubkey_hex,
                                  const char *app_id,
                                  const char *data_key,
                                  GnostrAppDataFetchCallback callback,
                                  gpointer user_data);

/**
 * gnostr_app_data_fetch_all_async:
 * @pubkey_hex: User's public key (64 hex chars)
 * @app_id: Application identifier
 * @callback: Callback when fetch completes
 * @user_data: User data for callback
 *
 * Fetches all app data entries for an app_id from relays.
 * Useful for loading all settings/data for an application.
 */
void gnostr_app_data_fetch_all_async(const char *pubkey_hex,
                                      const char *app_id,
                                      GnostrAppDataListCallback callback,
                                      gpointer user_data);

/**
 * gnostr_app_data_publish_async:
 * @app_id: Application identifier
 * @data_key: Data key
 * @content: Content to publish
 * @callback: Callback when publish completes
 * @user_data: User data for callback
 *
 * Signs and publishes app data to relays.
 * Creates a kind 30078 event with the given d-tag and content.
 */
void gnostr_app_data_publish_async(const char *app_id,
                                    const char *data_key,
                                    const char *content,
                                    GnostrAppDataCallback callback,
                                    gpointer user_data);

/**
 * gnostr_app_data_delete_async:
 * @app_id: Application identifier
 * @data_key: Data key
 * @callback: Callback when delete completes
 * @user_data: User data for callback
 *
 * Deletes app data by publishing an empty event with the same d-tag.
 * Per NIP-78/replaceable event semantics, this replaces the previous data.
 */
void gnostr_app_data_delete_async(const char *app_id,
                                   const char *data_key,
                                   GnostrAppDataCallback callback,
                                   gpointer user_data);

/* ---- Utility ---- */

/**
 * gnostr_app_data_is_valid_app_id:
 * @app_id: Application identifier to validate
 *
 * Validates an app_id string. Must be non-empty and contain only
 * alphanumeric characters, hyphens, and underscores.
 *
 * Returns: TRUE if valid
 */
gboolean gnostr_app_data_is_valid_app_id(const char *app_id);

/**
 * gnostr_app_data_is_valid_data_key:
 * @data_key: Data key to validate
 *
 * Validates a data_key string. Must not contain "/" character.
 *
 * Returns: TRUE if valid
 */
gboolean gnostr_app_data_is_valid_data_key(const char *data_key);

G_END_DECLS

#endif /* GNOSTR_NIP78_APP_DATA_H */
