/**
 * gnostr Blossom Service
 *
 * BUD-01 Blossom media server client implementation.
 * Provides async upload, list, and delete operations with Nostr auth.
 */

#ifndef GNOSTR_BLOSSOM_H
#define GNOSTR_BLOSSOM_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * Blossom upload result
 */
typedef struct {
  char *sha256;      /* SHA-256 hash of uploaded file (hex) */
  char *url;         /* Full URL to access the blob */
  char *mime_type;   /* MIME type of the file */
  gint64 size;       /* File size in bytes */
} GnostrBlossomBlob;

/**
 * Free a blob result
 */
void gnostr_blossom_blob_free(GnostrBlossomBlob *blob);

/**
 * Callback for async upload completion
 *
 * @param blob The upload result (NULL on error), owned by caller after callback
 * @param error Error details if upload failed (NULL on success)
 * @param user_data User-provided data
 */
typedef void (*GnostrBlossomUploadCallback)(GnostrBlossomBlob *blob,
                                             GError *error,
                                             gpointer user_data);

/**
 * Callback for async list completion
 *
 * @param blobs Array of blobs (NULL-terminated), owned by caller
 * @param error Error details if list failed (NULL on success)
 * @param user_data User-provided data
 */
typedef void (*GnostrBlossomListCallback)(GnostrBlossomBlob **blobs,
                                           GError *error,
                                           gpointer user_data);

/**
 * Callback for async delete completion
 *
 * @param success TRUE if delete succeeded
 * @param error Error details if delete failed (NULL on success)
 * @param user_data User-provided data
 */
typedef void (*GnostrBlossomDeleteCallback)(gboolean success,
                                             GError *error,
                                             gpointer user_data);

/**
 * Upload a file to a Blossom server asynchronously.
 *
 * Creates a kind 24242 auth event, signs it via signer IPC,
 * and uploads the file with the Nostr auth header.
 *
 * @param server_url Base URL of the Blossom server (e.g., "https://blossom.example.com")
 * @param file_path Path to the file to upload
 * @param mime_type MIME type of the file (e.g., "image/png"), or NULL to auto-detect
 * @param callback Callback when upload completes
 * @param user_data User data for callback
 * @param cancellable Optional GCancellable
 */
void gnostr_blossom_upload_async(const char *server_url,
                                  const char *file_path,
                                  const char *mime_type,
                                  GnostrBlossomUploadCallback callback,
                                  gpointer user_data,
                                  GCancellable *cancellable);

/**
 * List files uploaded by a user on a Blossom server.
 *
 * @param server_url Base URL of the Blossom server
 * @param pubkey_hex Public key (hex) of the user whose files to list
 * @param callback Callback when list completes
 * @param user_data User data for callback
 * @param cancellable Optional GCancellable
 */
void gnostr_blossom_list_async(const char *server_url,
                                const char *pubkey_hex,
                                GnostrBlossomListCallback callback,
                                gpointer user_data,
                                GCancellable *cancellable);

/**
 * Delete a file from a Blossom server.
 *
 * Creates a kind 24242 auth event for delete, signs it via signer IPC.
 *
 * @param server_url Base URL of the Blossom server
 * @param sha256 SHA-256 hash of the blob to delete (hex)
 * @param callback Callback when delete completes
 * @param user_data User data for callback
 * @param cancellable Optional GCancellable
 */
void gnostr_blossom_delete_async(const char *server_url,
                                  const char *sha256,
                                  GnostrBlossomDeleteCallback callback,
                                  gpointer user_data,
                                  GCancellable *cancellable);

/**
 * Compute SHA-256 hash of a file.
 *
 * @param file_path Path to the file
 * @param out_hash Output buffer for hex hash (must be at least 65 bytes)
 * @return TRUE on success, FALSE on error
 */
gboolean gnostr_blossom_sha256_file(const char *file_path, char out_hash[65]);

/**
 * Detect MIME type from file extension.
 *
 * @param file_path Path to the file
 * @return MIME type string (static, do not free)
 */
const char *gnostr_blossom_detect_mime_type(const char *file_path);

/**
 * Build a kind 24242 Blossom auth event JSON.
 *
 * @param action "upload", "delete", or "list"
 * @param sha256 SHA-256 hash of the blob (for upload/delete), NULL for list
 * @param server_url Server URL for the 'server' tag
 * @param file_size File size for 'size' tag (upload only), 0 to skip
 * @param mime_type MIME type for 'type' tag (upload only), NULL to skip
 * @return Newly allocated JSON string (caller frees)
 */
char *gnostr_blossom_build_auth_event(const char *action,
                                       const char *sha256,
                                       const char *server_url,
                                       gint64 file_size,
                                       const char *mime_type);

/**
 * Error domain for Blossom operations
 */
#define GNOSTR_BLOSSOM_ERROR (gnostr_blossom_error_quark())
GQuark gnostr_blossom_error_quark(void);

typedef enum {
  GNOSTR_BLOSSOM_ERROR_FILE_NOT_FOUND,
  GNOSTR_BLOSSOM_ERROR_FILE_READ,
  GNOSTR_BLOSSOM_ERROR_SIGNING_FAILED,
  GNOSTR_BLOSSOM_ERROR_UPLOAD_FAILED,
  GNOSTR_BLOSSOM_ERROR_SERVER_ERROR,
  GNOSTR_BLOSSOM_ERROR_PARSE_ERROR,
  GNOSTR_BLOSSOM_ERROR_AUTH_FAILED,
  GNOSTR_BLOSSOM_ERROR_NOT_FOUND,
  GNOSTR_BLOSSOM_ERROR_CANCELLED
} GnostrBlossomError;

G_END_DECLS

#endif /* GNOSTR_BLOSSOM_H */
