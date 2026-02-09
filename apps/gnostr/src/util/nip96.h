/**
 * @file nip96.h
 * @brief NIP-96 HTTP File Storage Integration
 *
 * Implements NIP-96 file upload protocol using multipart form POST with
 * NIP-98 (kind 27235) authentication. Provides async upload, delete, and
 * server discovery operations.
 *
 * Reuses GnostrBlossomBlob and GnostrBlossomUploadCallback types for
 * seamless integration with existing media upload infrastructure.
 *
 * nostrc-fs5g: NIP-96 file storage upload support.
 */

#ifndef GNOSTR_NIP96_H
#define GNOSTR_NIP96_H

#include <glib-object.h>
#include <gio/gio.h>
#include "blossom.h"  /* GnostrBlossomBlob, GnostrBlossomUploadCallback */

G_BEGIN_DECLS

/**
 * NIP-96 server info parsed from .well-known/nostr/nip96.json
 */
typedef struct {
  char *api_url;          /* Upload/download API base URL (required) */
  char *download_url;     /* CDN download URL (optional, NULL if same as api_url) */
  char *tos_url;          /* Terms of service URL (optional) */
  char **content_types;   /* NULL-terminated array of supported MIME types (optional) */
  gint64 max_byte_size;   /* Max upload size in bytes from free plan (0 = unknown) */
  gboolean nip98_required; /* Whether NIP-98 auth is required */
} GnostrNip96ServerInfo;

/**
 * Free a NIP-96 server info struct.
 */
void gnostr_nip96_server_info_free(GnostrNip96ServerInfo *info);

/**
 * Discover NIP-96 server capabilities.
 *
 * Fetches /.well-known/nostr/nip96.json from the given server URL.
 * Results are cached per server URL for the session lifetime.
 *
 * @param server_url Base URL of the server (e.g., "https://nostr.build")
 * @param cancellable Optional GCancellable
 * @param callback GAsyncReadyCallback for completion
 * @param user_data User data for callback
 */
void gnostr_nip96_discover_async(const char *server_url,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);

/**
 * Finish a NIP-96 discovery operation.
 *
 * @param res The GAsyncResult
 * @param error Return location for error, or NULL
 * @return Server info (caller takes ownership), or NULL on error
 */
GnostrNip96ServerInfo *gnostr_nip96_discover_finish(GAsyncResult *res,
                                                     GError **error);

/**
 * Upload a file to a NIP-96 server asynchronously.
 *
 * Discovers the server's api_url, creates a NIP-98 kind 27235 auth event,
 * signs it via signer IPC, and uploads the file as multipart/form-data.
 *
 * Uses the same callback and result types as Blossom for compatibility.
 *
 * @param server_url Base URL of the NIP-96 server
 * @param file_path Path to the file to upload
 * @param mime_type MIME type of the file, or NULL to auto-detect
 * @param callback Callback when upload completes
 * @param user_data User data for callback
 * @param cancellable Optional GCancellable
 */
void gnostr_nip96_upload_async(const char *server_url,
                                const char *file_path,
                                const char *mime_type,
                                GnostrBlossomUploadCallback callback,
                                gpointer user_data,
                                GCancellable *cancellable);

/**
 * Delete a file from a NIP-96 server.
 *
 * @param server_url Base URL of the NIP-96 server
 * @param sha256 SHA-256 hash of the file to delete (hex)
 * @param callback Callback when delete completes
 * @param user_data User data for callback
 * @param cancellable Optional GCancellable
 */
void gnostr_nip96_delete_async(const char *server_url,
                                const char *sha256,
                                GnostrBlossomDeleteCallback callback,
                                gpointer user_data,
                                GCancellable *cancellable);

/**
 * Error domain for NIP-96 operations
 */
#define GNOSTR_NIP96_ERROR (gnostr_nip96_error_quark())
GQuark gnostr_nip96_error_quark(void);

typedef enum {
  GNOSTR_NIP96_ERROR_DISCOVERY_FAILED,
  GNOSTR_NIP96_ERROR_UNSUPPORTED_TYPE,
  GNOSTR_NIP96_ERROR_FILE_TOO_LARGE,
  GNOSTR_NIP96_ERROR_AUTH_FAILED,
  GNOSTR_NIP96_ERROR_UPLOAD_FAILED,
  GNOSTR_NIP96_ERROR_PARSE_ERROR,
  GNOSTR_NIP96_ERROR_SERVER_ERROR,
  GNOSTR_NIP96_ERROR_FILE_NOT_FOUND,
  GNOSTR_NIP96_ERROR_FILE_READ,
  GNOSTR_NIP96_ERROR_CANCELLED
} GnostrNip96Error;

G_END_DECLS

#endif /* GNOSTR_NIP96_H */
