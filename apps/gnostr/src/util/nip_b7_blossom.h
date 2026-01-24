/*
 * nip_b7_blossom.h - NIP-B7 Blossom Protocol Support (BUD-01/02/03)
 *
 * Blossom is a protocol for storing blobs (binary large objects) on
 * Nostr-connected servers. Blobs are identified by their SHA-256 hash.
 *
 * Event Kind:
 *   - Kind 10063: User's blob server list (replaceable event)
 *     Similar to NIP-65 relay lists, this stores a user's preferred
 *     Blossom servers for file storage.
 *
 * Tags for kind 10063:
 *   - ["server", "<url>"] - Blossom server URL (repeatable)
 *
 * HTTP Endpoints (BUD-01):
 *   - GET /<sha256>         - Download blob
 *   - HEAD /<sha256>        - Check if blob exists
 *   - PUT /upload           - Upload new blob (with auth)
 *   - DELETE /<sha256>      - Delete blob (with auth)
 *   - GET /list/<pubkey>    - List user's blobs
 *
 * Authentication uses NIP-98 style HTTP Auth with kind 24242 events.
 *
 * This module provides parsing utilities for Blossom event data.
 * For HTTP operations, see blossom.h which provides async upload/delete.
 */

#ifndef NIP_B7_BLOSSOM_H
#define NIP_B7_BLOSSOM_H

#include <glib.h>

G_BEGIN_DECLS

/* Event kind for user's blob server list (replaceable) */
#define NIPB7_KIND_BLOB_SERVERS 10063

/* Auth event kind for Blossom operations (from BUD-01) */
#define NIPB7_KIND_BLOSSOM_AUTH 24242

/**
 * GnostrBlob:
 *
 * Represents blob metadata returned from a Blossom server.
 * This corresponds to the JSON response from upload/list operations.
 */
typedef struct _GnostrBlob {
  gchar *sha256;       /* SHA-256 hash of the blob (64 hex chars) */
  gsize size;          /* File size in bytes */
  gchar *mime_type;    /* MIME type (e.g., "image/png") */
  gchar *url;          /* Full URL to access the blob */
  gint64 created_at;   /* Upload timestamp (unix seconds), 0 if unknown */
} GnostrBlob;

/**
 * GnostrBlobServerList:
 *
 * Represents a user's list of Blossom servers from kind 10063.
 * Parsed from event tags where each ["server", "<url>"] defines a server.
 */
typedef struct _GnostrBlobServerList {
  gchar **servers;     /* NULL-terminated array of server URLs */
  gsize server_count;  /* Number of servers in the list */
} GnostrBlobServerList;

/* ============== GnostrBlob API ============== */

/**
 * gnostr_blob_new:
 *
 * Creates a new empty blob metadata structure.
 *
 * Returns: (transfer full): A new GnostrBlob. Free with gnostr_blob_free().
 */
GnostrBlob *gnostr_blob_new(void);

/**
 * gnostr_blob_free:
 * @blob: (nullable): Blob metadata to free.
 *
 * Frees a blob metadata structure and all its contents.
 */
void gnostr_blob_free(GnostrBlob *blob);

/**
 * gnostr_blob_copy:
 * @blob: Blob to copy.
 *
 * Creates a deep copy of a blob metadata structure.
 *
 * Returns: (transfer full) (nullable): Copy of the blob, or NULL if input is NULL.
 */
GnostrBlob *gnostr_blob_copy(const GnostrBlob *blob);

/**
 * gnostr_blob_parse_response:
 * @json_data: JSON string from Blossom upload response.
 *
 * Parses blob metadata from a Blossom server JSON response.
 * Expected format:
 *   {
 *     "sha256": "<hash>",
 *     "size": 12345,
 *     "type": "image/png",
 *     "url": "https://server/abc123..."
 *   }
 *
 * Returns: (transfer full) (nullable): Parsed blob or NULL on error.
 */
GnostrBlob *gnostr_blob_parse_response(const gchar *json_data);

/**
 * gnostr_blob_parse_list_response:
 * @json_data: JSON array string from Blossom list response.
 *
 * Parses an array of blob metadata from a list response.
 * Expected format: array of blob objects as in parse_response.
 *
 * Returns: (transfer full) (element-type GnostrBlob): GPtrArray of blobs,
 *          or NULL on error. Free array with g_ptr_array_unref().
 */
GPtrArray *gnostr_blob_parse_list_response(const gchar *json_data);

/**
 * gnostr_blob_validate_sha256:
 * @hash: SHA-256 hash string to validate.
 *
 * Validates that a string is a properly formatted SHA-256 hash.
 * Must be exactly 64 lowercase hexadecimal characters.
 *
 * Returns: TRUE if valid SHA-256 hash, FALSE otherwise.
 */
gboolean gnostr_blob_validate_sha256(const gchar *hash);

/* ============== GnostrBlobServerList API ============== */

/**
 * gnostr_blob_server_list_new:
 *
 * Creates a new empty blob server list.
 *
 * Returns: (transfer full): A new server list. Free with gnostr_blob_server_list_free().
 */
GnostrBlobServerList *gnostr_blob_server_list_new(void);

/**
 * gnostr_blob_server_list_free:
 * @list: (nullable): Server list to free.
 *
 * Frees a blob server list and all its contents.
 */
void gnostr_blob_server_list_free(GnostrBlobServerList *list);

/**
 * gnostr_blob_server_list_parse:
 * @tags_json: JSON array string containing event tags.
 *
 * Parses a blob server list from kind 10063 event tags.
 * Extracts all ["server", "<url>"] tags.
 *
 * Example input:
 *   [["server", "https://blossom1.example.com"], ["server", "https://blossom2.example.com"]]
 *
 * Returns: (transfer full) (nullable): Parsed server list or NULL on error.
 */
GnostrBlobServerList *gnostr_blob_server_list_parse(const gchar *tags_json);

/**
 * gnostr_blob_server_list_parse_event:
 * @event_json: Full JSON string of a kind 10063 event.
 *
 * Parses a blob server list from a complete kind 10063 event.
 * Validates the event kind and extracts server tags.
 *
 * Returns: (transfer full) (nullable): Parsed server list or NULL on error.
 */
GnostrBlobServerList *gnostr_blob_server_list_parse_event(const gchar *event_json);

/**
 * gnostr_blob_server_list_add:
 * @list: Server list to modify.
 * @server_url: Server URL to add.
 *
 * Adds a server URL to the list if not already present.
 * The URL is normalized (trailing slashes removed).
 *
 * Returns: TRUE if added, FALSE if already present or invalid.
 */
gboolean gnostr_blob_server_list_add(GnostrBlobServerList *list,
                                      const gchar *server_url);

/**
 * gnostr_blob_server_list_remove:
 * @list: Server list to modify.
 * @server_url: Server URL to remove.
 *
 * Removes a server URL from the list.
 *
 * Returns: TRUE if removed, FALSE if not found.
 */
gboolean gnostr_blob_server_list_remove(GnostrBlobServerList *list,
                                         const gchar *server_url);

/**
 * gnostr_blob_server_list_contains:
 * @list: Server list to search.
 * @server_url: Server URL to find.
 *
 * Checks if a server URL is in the list.
 *
 * Returns: TRUE if found, FALSE otherwise.
 */
gboolean gnostr_blob_server_list_contains(const GnostrBlobServerList *list,
                                           const gchar *server_url);

/**
 * gnostr_blob_server_list_to_tags_json:
 * @list: Server list to convert.
 *
 * Converts the server list to a JSON tags array for event creation.
 * Each server becomes a ["server", "<url>"] tag.
 *
 * Returns: (transfer full): JSON string of tags array.
 */
gchar *gnostr_blob_server_list_to_tags_json(const GnostrBlobServerList *list);

/* ============== URL Building Utilities ============== */

/**
 * gnostr_blossom_build_blob_path:
 * @server_url: Base server URL (e.g., "https://blossom.example.com").
 * @sha256: SHA-256 hash of the blob.
 *
 * Builds the URL path for GET/HEAD/DELETE operations on a blob.
 * Format: "<server_url>/<sha256>"
 *
 * Returns: (transfer full): Full URL path.
 */
gchar *gnostr_blossom_build_blob_path(const gchar *server_url,
                                       const gchar *sha256);

/**
 * gnostr_blossom_build_upload_path:
 * @server_url: Base server URL.
 *
 * Builds the URL path for PUT upload operations.
 * Format: "<server_url>/upload"
 *
 * Returns: (transfer full): Full URL path.
 */
gchar *gnostr_blossom_build_upload_path(const gchar *server_url);

/**
 * gnostr_blossom_build_delete_path:
 * @server_url: Base server URL.
 * @sha256: SHA-256 hash of the blob to delete.
 *
 * Builds the URL path for DELETE operations.
 * Format: "<server_url>/<sha256>"
 *
 * Returns: (transfer full): Full URL path.
 */
gchar *gnostr_blossom_build_delete_path(const gchar *server_url,
                                         const gchar *sha256);

/**
 * gnostr_blossom_build_list_path:
 * @server_url: Base server URL.
 * @pubkey_hex: Public key in hex format (64 chars).
 *
 * Builds the URL path for listing a user's blobs.
 * Format: "<server_url>/list/<pubkey>"
 *
 * Returns: (transfer full): Full URL path.
 */
gchar *gnostr_blossom_build_list_path(const gchar *server_url,
                                       const gchar *pubkey_hex);

/**
 * gnostr_blossom_normalize_url:
 * @url: Server URL to normalize.
 *
 * Normalizes a Blossom server URL:
 *   - Removes trailing slashes
 *   - Ensures https:// prefix if no scheme
 *
 * Returns: (transfer full): Normalized URL.
 */
gchar *gnostr_blossom_normalize_url(const gchar *url);

G_END_DECLS

#endif /* NIP_B7_BLOSSOM_H */
