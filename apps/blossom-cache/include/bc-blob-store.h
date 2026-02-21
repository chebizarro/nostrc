/*
 * bc-blob-store.h - BcBlobStore: blob metadata + filesystem content storage
 *
 * SPDX-License-Identifier: MIT
 *
 * BcBlobStore combines a pluggable metadata backend (BcDbBackend)
 * with content-addressed filesystem storage for blob data.
 */

#ifndef BC_BLOB_STORE_H
#define BC_BLOB_STORE_H

#include <glib-object.h>
#include <gio/gio.h>
#include "bc-db-backend.h"

G_BEGIN_DECLS

#define BC_TYPE_BLOB_STORE (bc_blob_store_get_type())
G_DECLARE_FINAL_TYPE(BcBlobStore, bc_blob_store, BC, BLOB_STORE, GObject)

/**
 * BcBlobInfo:
 * @sha256: SHA-256 hash of the blob (64 hex chars, owned)
 * @size: Blob size in bytes
 * @mime_type: MIME type string (owned, may be NULL)
 * @created_at: Unix timestamp when first cached
 * @last_accessed: Unix timestamp of most recent access
 * @access_count: Number of times this blob was served
 */
typedef struct {
  gchar  *sha256;
  gint64  size;
  gchar  *mime_type;
  gint64  created_at;
  gint64  last_accessed;
  guint   access_count;
} BcBlobInfo;

BcBlobInfo *bc_blob_info_copy(const BcBlobInfo *info);
void        bc_blob_info_free(BcBlobInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(BcBlobInfo, bc_blob_info_free)

/**
 * BcBlobStoreError:
 * @BC_BLOB_STORE_ERROR_IO: Filesystem I/O failure
 * @BC_BLOB_STORE_ERROR_DB: Database backend error
 * @BC_BLOB_STORE_ERROR_NOT_FOUND: Blob not in the store
 * @BC_BLOB_STORE_ERROR_HASH_MISMATCH: Content does not match expected SHA-256
 * @BC_BLOB_STORE_ERROR_TOO_LARGE: Blob exceeds maximum allowed size
 */
typedef enum {
  BC_BLOB_STORE_ERROR_IO,
  BC_BLOB_STORE_ERROR_DB,
  BC_BLOB_STORE_ERROR_NOT_FOUND,
  BC_BLOB_STORE_ERROR_HASH_MISMATCH,
  BC_BLOB_STORE_ERROR_TOO_LARGE,
} BcBlobStoreError;

#define BC_BLOB_STORE_ERROR (bc_blob_store_error_quark())
GQuark bc_blob_store_error_quark(void);

/**
 * bc_blob_store_new:
 * @storage_dir: Base directory for blob content files
 * @backend: (transfer full): metadata backend (BcBlobStore takes ownership)
 * @error: return location for error
 *
 * Creates a blob store using the given metadata backend.
 * Blob content is stored under @storage_dir/blobs/ with 2-char prefix fanout.
 *
 * Returns: (transfer full) (nullable): new blob store, or NULL on error
 */
BcBlobStore *bc_blob_store_new(const gchar *storage_dir, BcDbBackend *backend,
                                GError **error);

/**
 * bc_blob_store_new_sqlite:
 * @storage_dir: Base directory (SQLite DB created here as blobs.db)
 * @error: return location for error
 *
 * Convenience constructor that creates a blob store with the SQLite backend.
 * This is the default and backwards-compatible creation path.
 *
 * Returns: (transfer full) (nullable): new blob store, or NULL on error
 */
BcBlobStore *bc_blob_store_new_sqlite(const gchar *storage_dir, GError **error);

gboolean     bc_blob_store_contains(BcBlobStore *self, const gchar *sha256);
BcBlobInfo  *bc_blob_store_get_info(BcBlobStore *self, const gchar *sha256, GError **error);
gchar       *bc_blob_store_get_content_path(BcBlobStore *self, const gchar *sha256);
gboolean     bc_blob_store_put(BcBlobStore *self, const gchar *sha256, GBytes *data,
                               const gchar *mime_type, gboolean verify, GError **error);
gboolean     bc_blob_store_delete(BcBlobStore *self, const gchar *sha256, GError **error);
gint64       bc_blob_store_get_total_size(BcBlobStore *self);
guint        bc_blob_store_get_blob_count(BcBlobStore *self);
gint         bc_blob_store_evict_lru(BcBlobStore *self, gint64 bytes_to_free, GError **error);

/**
 * bc_blob_store_list_blobs:
 * @self: the blob store
 * @cursor_sha256: (nullable): SHA-256 of the last blob from the previous page, or NULL for first page
 * @limit: maximum number of results to return (0 for default of 100)
 *
 * Returns: (element-type BcBlobInfo) (transfer full): array of blob info, sorted by uploaded desc
 */
GPtrArray   *bc_blob_store_list_blobs(BcBlobStore *self, const gchar *cursor_sha256,
                                      guint limit, GError **error);

G_END_DECLS

#endif /* BC_BLOB_STORE_H */
