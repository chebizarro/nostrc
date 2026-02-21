/*
 * bc-blob-store.c - BcBlobStore: pluggable metadata + filesystem blob storage
 *
 * SPDX-License-Identifier: MIT
 *
 * Directory layout:
 *   <storage_dir>/
 *     blobs/
 *       <sha256[0:2]>/  - Two-char prefix subdirectory (fanout)
 *         <sha256>      - Raw blob content file
 *
 * Metadata is stored via a BcDbBackend (SQLite or LMDB).
 */

#include "bc-blob-store.h"
#include "bc-db-backend.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>

/* ---- BcBlobInfo ---- */

BcBlobInfo *
bc_blob_info_copy(const BcBlobInfo *info)
{
  if (info == NULL)
    return NULL;

  BcBlobInfo *copy = g_new0(BcBlobInfo, 1);
  copy->sha256        = g_strdup(info->sha256);
  copy->size          = info->size;
  copy->mime_type     = g_strdup(info->mime_type);
  copy->created_at    = info->created_at;
  copy->last_accessed = info->last_accessed;
  copy->access_count  = info->access_count;
  return copy;
}

void
bc_blob_info_free(BcBlobInfo *info)
{
  if (info == NULL)
    return;
  g_free(info->sha256);
  g_free(info->mime_type);
  g_free(info);
}

/* ---- Error quark ---- */

G_DEFINE_QUARK(bc-blob-store-error-quark, bc_blob_store_error)

/* ---- Private structure ---- */

struct _BcBlobStore {
  GObject      parent_instance;

  gchar       *storage_dir;   /* Base directory (owned) */
  gchar       *blobs_dir;     /* <storage_dir>/blobs (owned) */
  BcDbBackend *db;            /* Metadata backend (owned) */
};

G_DEFINE_TYPE(BcBlobStore, bc_blob_store, G_TYPE_OBJECT)

/* ---- Helpers: BcDbBlobMeta ↔ BcBlobInfo conversion ---- */

static BcBlobInfo *
blob_info_from_meta(const BcDbBlobMeta *meta)
{
  if (meta == NULL)
    return NULL;

  BcBlobInfo *info = g_new0(BcBlobInfo, 1);
  info->sha256        = g_strdup(meta->sha256);
  info->size          = (gint64)meta->size;
  info->mime_type     = g_strdup(meta->mime_type);
  info->created_at    = (gint64)meta->created_at;
  info->last_accessed = (gint64)meta->last_accessed;
  info->access_count  = (guint)meta->access_count;
  return info;
}

/* ---- Filesystem helpers ---- */

static gchar *
blob_content_dir(BcBlobStore *self, const gchar *sha256)
{
  gchar prefix[3] = { sha256[0], sha256[1], '\0' };
  return g_build_filename(self->blobs_dir, prefix, NULL);
}

static gchar *
blob_content_path(BcBlobStore *self, const gchar *sha256)
{
  gchar prefix[3] = { sha256[0], sha256[1], '\0' };
  return g_build_filename(self->blobs_dir, prefix, sha256, NULL);
}

static gboolean
ensure_directory(const gchar *path, GError **error)
{
  g_autoptr(GFile) dir = g_file_new_for_path(path);
  if (g_file_query_exists(dir, NULL))
    return TRUE;

  return g_file_make_directory_with_parents(dir, NULL, error);
}

/* ---- GObject lifecycle ---- */

static void
bc_blob_store_finalize(GObject *obj)
{
  BcBlobStore *self = BC_BLOB_STORE(obj);

  bc_db_backend_free(self->db);
  self->db = NULL;

  g_clear_pointer(&self->storage_dir, g_free);
  g_clear_pointer(&self->blobs_dir, g_free);

  G_OBJECT_CLASS(bc_blob_store_parent_class)->finalize(obj);
}

static void
bc_blob_store_class_init(BcBlobStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = bc_blob_store_finalize;
}

static void
bc_blob_store_init(BcBlobStore *self)
{
  (void)self;
}

/* ---- Public API ---- */

BcBlobStore *
bc_blob_store_new(const gchar *storage_dir, BcDbBackend *backend, GError **error)
{
  g_return_val_if_fail(storage_dir != NULL, NULL);
  g_return_val_if_fail(backend != NULL, NULL);

  if (!ensure_directory(storage_dir, error))
    return NULL;

  gchar *blobs_dir = g_build_filename(storage_dir, "blobs", NULL);
  if (!ensure_directory(blobs_dir, error)) {
    g_free(blobs_dir);
    return NULL;
  }

  BcBlobStore *self = g_object_new(BC_TYPE_BLOB_STORE, NULL);
  self->storage_dir = g_strdup(storage_dir);
  self->blobs_dir   = blobs_dir;
  self->db          = backend; /* takes ownership */

  return self;
}

BcBlobStore *
bc_blob_store_new_sqlite(const gchar *storage_dir, GError **error)
{
  g_return_val_if_fail(storage_dir != NULL, NULL);

  if (!ensure_directory(storage_dir, error))
    return NULL;

  g_autofree gchar *db_path = g_build_filename(storage_dir, "blobs.db", NULL);
  BcDbBackend *backend = bc_db_backend_sqlite_new(db_path, error);
  if (backend == NULL)
    return NULL;

  return bc_blob_store_new(storage_dir, backend, error);
}

gboolean
bc_blob_store_contains(BcBlobStore *self, const gchar *sha256)
{
  g_return_val_if_fail(BC_IS_BLOB_STORE(self), FALSE);
  g_return_val_if_fail(sha256 != NULL, FALSE);

  return self->db->contains(self->db->ctx, sha256);
}

BcBlobInfo *
bc_blob_store_get_info(BcBlobStore *self, const gchar *sha256, GError **error)
{
  g_return_val_if_fail(BC_IS_BLOB_STORE(self), NULL);
  g_return_val_if_fail(sha256 != NULL, NULL);

  BcDbBlobMeta *meta = self->db->get_info(self->db->ctx, sha256, error);
  if (meta == NULL) {
    if (error != NULL && *error == NULL) {
      g_set_error(error, BC_BLOB_STORE_ERROR, BC_BLOB_STORE_ERROR_NOT_FOUND,
                  "Blob %s not found", sha256);
    }
    return NULL;
  }

  BcBlobInfo *info = blob_info_from_meta(meta);
  bc_db_blob_meta_free(meta);
  return info;
}

gchar *
bc_blob_store_get_content_path(BcBlobStore *self, const gchar *sha256)
{
  g_return_val_if_fail(BC_IS_BLOB_STORE(self), NULL);
  g_return_val_if_fail(sha256 != NULL, NULL);

  return blob_content_path(self, sha256);
}

gboolean
bc_blob_store_put(BcBlobStore *self,
                  const gchar *sha256,
                  GBytes      *data,
                  const gchar *mime_type,
                  gboolean     verify,
                  GError     **error)
{
  g_return_val_if_fail(BC_IS_BLOB_STORE(self), FALSE);
  g_return_val_if_fail(sha256 != NULL, FALSE);
  g_return_val_if_fail(data != NULL, FALSE);

  if (bc_blob_store_contains(self, sha256))
    return TRUE;

  gsize data_len = 0;
  const guchar *raw = g_bytes_get_data(data, &data_len);

  if (verify) {
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, raw, data_len);
    const gchar *computed = g_checksum_get_string(checksum);

    if (g_ascii_strcasecmp(computed, sha256) != 0) {
      g_set_error(error, BC_BLOB_STORE_ERROR, BC_BLOB_STORE_ERROR_HASH_MISMATCH,
                  "SHA-256 mismatch: expected %s, got %s", sha256, computed);
      return FALSE;
    }
  }

  /* Write content file */
  g_autofree gchar *content_dir = blob_content_dir(self, sha256);
  if (!ensure_directory(content_dir, error))
    return FALSE;

  g_autofree gchar *path = blob_content_path(self, sha256);
  if (!g_file_set_contents(path, (const gchar *)raw, data_len, error))
    return FALSE;

  /* Store metadata via backend */
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;

  BcDbBlobMeta meta = {
    .sha256        = (char *)sha256,
    .size          = (int64_t)data_len,
    .mime_type     = (char *)(mime_type ? mime_type : "application/octet-stream"),
    .created_at    = now,
    .last_accessed = now,
    .access_count  = 0,
  };

  if (!self->db->put_meta(self->db->ctx, &meta, error)) {
    g_unlink(path);
    return FALSE;
  }

  g_debug("stored blob %s (%" G_GSIZE_FORMAT " bytes, %s)",
          sha256, data_len, mime_type ? mime_type : "application/octet-stream");
  return TRUE;
}

gboolean
bc_blob_store_delete(BcBlobStore *self, const gchar *sha256, GError **error)
{
  g_return_val_if_fail(BC_IS_BLOB_STORE(self), FALSE);
  g_return_val_if_fail(sha256 != NULL, FALSE);

  g_autofree gchar *path = blob_content_path(self, sha256);
  if (g_file_test(path, G_FILE_TEST_EXISTS)) {
    if (g_unlink(path) != 0) {
      g_set_error(error, BC_BLOB_STORE_ERROR, BC_BLOB_STORE_ERROR_IO,
                  "Failed to delete blob file: %s", path);
      return FALSE;
    }
  }

  if (!self->db->delete_meta(self->db->ctx, sha256, error))
    return FALSE;

  g_debug("deleted blob %s", sha256);
  return TRUE;
}

gint64
bc_blob_store_get_total_size(BcBlobStore *self)
{
  g_return_val_if_fail(BC_IS_BLOB_STORE(self), 0);
  return self->db->get_total_size(self->db->ctx);
}

guint
bc_blob_store_get_blob_count(BcBlobStore *self)
{
  g_return_val_if_fail(BC_IS_BLOB_STORE(self), 0);
  return (guint)self->db->get_blob_count(self->db->ctx);
}

gint
bc_blob_store_evict_lru(BcBlobStore *self, gint64 bytes_to_free, GError **error)
{
  g_return_val_if_fail(BC_IS_BLOB_STORE(self), -1);

  if (bytes_to_free <= 0)
    return 0;

  GPtrArray *candidates = self->db->evict_candidates(self->db->ctx,
                                                      bytes_to_free, error);
  if (candidates == NULL)
    return -1;

  gint count = 0;
  gint64 freed = 0;

  for (guint i = 0; i < candidates->len; i++) {
    BcDbBlobMeta *meta = g_ptr_array_index(candidates, i);
    GError *del_err = NULL;
    if (bc_blob_store_delete(self, meta->sha256, &del_err)) {
      count++;
      freed += meta->size;
    } else {
      g_warning("eviction: failed to delete %s: %s",
                meta->sha256, del_err ? del_err->message : "unknown");
      g_clear_error(&del_err);
    }
  }

  g_ptr_array_unref(candidates);
  g_debug("evicted %d blobs, freed %" G_GINT64_FORMAT " bytes", count, freed);
  return count;
}

GPtrArray *
bc_blob_store_list_blobs(BcBlobStore *self,
                         const gchar *cursor_sha256,
                         guint        limit,
                         GError     **error)
{
  g_return_val_if_fail(BC_IS_BLOB_STORE(self), NULL);

  GPtrArray *db_results = self->db->list_blobs(self->db->ctx, cursor_sha256,
                                                limit, error);
  if (db_results == NULL)
    return NULL;

  /* Convert BcDbBlobMeta → BcBlobInfo */
  GPtrArray *results = g_ptr_array_new_with_free_func((GDestroyNotify)bc_blob_info_free);

  for (guint i = 0; i < db_results->len; i++) {
    BcDbBlobMeta *meta = g_ptr_array_index(db_results, i);
    BcBlobInfo *info = blob_info_from_meta(meta);
    if (info != NULL)
      g_ptr_array_add(results, info);
  }

  g_ptr_array_unref(db_results);
  return results;
}
