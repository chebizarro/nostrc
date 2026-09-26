/* nd-file-store.c - NIP-94 file store for WebDAV (SQLite-backed)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nd-file-store.h"

#include <sqlite3.h>
#include <string.h>

struct _NdFileStore {
  NdStoreDb *db;
};

#define FILE_COLUMNS \
  "path, content, mime, sha256, blossom_url, pubkey, created_at, modified_at"

static NdFileEntry *
entry_from_row(sqlite3_stmt *stmt, GError **error)
{
  const gchar *path = (const gchar *)sqlite3_column_text(stmt, 0);
  const void  *blob = sqlite3_column_blob(stmt, 1);
  int          blob_len = sqlite3_column_bytes(stmt, 1);
  const gchar *mime = (const gchar *)sqlite3_column_text(stmt, 2);
  const gchar *sha256 = (const gchar *)sqlite3_column_text(stmt, 3);

  g_autoptr(GBytes) content = g_bytes_new(blob, (gsize)blob_len);
  NdFileEntry *entry = nd_file_entry_new(path, content, mime);

  if (sha256 == NULL || strcmp(entry->sha256, sha256) != 0) {
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_DATA,
                "Stored file %s does not match its recorded SHA-256", path);
    nd_file_entry_free(entry);
    return NULL;
  }

  entry->blossom_url = g_strdup((const gchar *)sqlite3_column_text(stmt, 4));
  entry->pubkey = g_strdup((const gchar *)sqlite3_column_text(stmt, 5));
  entry->created_at = sqlite3_column_int64(stmt, 6);
  entry->modified_at = sqlite3_column_int64(stmt, 7);
  return entry;
}

static void
bind_text_or_null(sqlite3_stmt *stmt, int idx, const gchar *value)
{
  if (value)
    sqlite3_bind_text(stmt, idx, value, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(stmt, idx);
}

NdFileStore *
nd_file_store_new(NdStoreDb *db)
{
  g_return_val_if_fail(db != NULL, NULL);
  NdFileStore *store = g_new0(NdFileStore, 1);
  store->db = nd_store_db_ref(db);
  return store;
}

void
nd_file_store_free(NdFileStore *store)
{
  if (store == NULL) return;
  nd_store_db_unref(store->db);
  g_free(store);
}

gboolean
nd_file_store_put(NdFileStore       *store,
                  const NdFileEntry *entry,
                  gboolean          *out_created,
                  GError           **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(entry != NULL && entry->path != NULL, FALSE);
  g_return_val_if_fail(entry->content != NULL, FALSE);

  sqlite3 *h = nd_store_db_get_handle(store->db);
  g_autofree gchar *etag = nd_file_entry_compute_etag(entry);
  gsize data_len = 0;
  const void *data = g_bytes_get_data(entry->content, &data_len);
  gboolean existed = FALSE;

  if (data_len > G_MAXINT) {
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_DATA,
                "File %s is too large to store (%" G_GSIZE_FORMAT " bytes)",
                entry->path, data_len);
    return FALSE;
  }

  if (!nd_store_db_begin(store->db, error))
    return FALSE;

  if (!nd_store_db_row_exists(store->db, ND_STORE_COLLECTION_FILES,
                              entry->path, &existed, error))
    goto fail;

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h,
        "INSERT INTO files (path, content, mime, etag, sha256, size,"
        "                   blossom_url, pubkey, created_at, modified_at)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)"
        " ON CONFLICT(path) DO UPDATE SET"
        "   content = excluded.content, mime = excluded.mime,"
        "   etag = excluded.etag, sha256 = excluded.sha256,"
        "   size = excluded.size, blossom_url = excluded.blossom_url,"
        "   pubkey = excluded.pubkey, created_at = excluded.created_at,"
        "   modified_at = excluded.modified_at",
        -1, &stmt, NULL) != SQLITE_OK) {
    nd_store_db_set_sql_error(store->db, error, "prepare file upsert");
    goto fail;
  }

  sqlite3_bind_text(stmt, 1, entry->path, -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, data_len ? data : "", (int)data_len, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, entry->mime_type, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, etag, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, entry->sha256, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 6, (sqlite3_int64)data_len);
  bind_text_or_null(stmt, 7, entry->blossom_url);
  bind_text_or_null(stmt, 8, entry->pubkey);
  sqlite3_bind_int64(stmt, 9, entry->created_at);
  sqlite3_bind_int64(stmt, 10, entry->modified_at);

  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE)
    nd_store_db_set_sql_error(store->db, error, "file upsert");
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE)
    goto fail;

  if (!nd_store_db_bump_generation(store->db, ND_STORE_COLLECTION_FILES, error))
    goto fail;

  if (!nd_store_db_commit(store->db, error))
    return FALSE;

  if (out_created)
    *out_created = !existed;
  return TRUE;

fail:
  nd_store_db_rollback(store->db);
  return FALSE;
}

NdFileEntry *
nd_file_store_get(NdFileStore *store,
                  const gchar *path,
                  GError     **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  g_return_val_if_fail(path != NULL, NULL);

  sqlite3 *h = nd_store_db_get_handle(store->db);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, "SELECT " FILE_COLUMNS " FROM files WHERE path = ?1",
                         -1, &stmt, NULL) != SQLITE_OK) {
    nd_store_db_set_sql_error(store->db, error, "prepare file lookup");
    return NULL;
  }
  sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);

  NdFileEntry *entry = NULL;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    entry = entry_from_row(stmt, error);
  else if (rc != SQLITE_DONE)
    nd_store_db_set_sql_error(store->db, error, "file lookup");

  sqlite3_finalize(stmt);
  return entry;
}

gboolean
nd_file_store_remove(NdFileStore *store,
                     const gchar *path,
                     gboolean    *out_removed,
                     GError     **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(path != NULL, FALSE);
  return nd_store_db_delete_row(store->db, ND_STORE_COLLECTION_FILES,
                                path, out_removed, error);
}

GPtrArray *
nd_file_store_list_all(NdFileStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);

  sqlite3 *h = nd_store_db_get_handle(store->db);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, "SELECT " FILE_COLUMNS " FROM files ORDER BY path",
                         -1, &stmt, NULL) != SQLITE_OK) {
    nd_store_db_set_sql_error(store->db, error, "prepare file list");
    return NULL;
  }

  GPtrArray *list =
    g_ptr_array_new_with_free_func((GDestroyNotify)nd_file_entry_free);
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    NdFileEntry *entry = entry_from_row(stmt, error);
    if (entry == NULL) {
      g_clear_pointer(&list, g_ptr_array_unref);
      break;
    }
    g_ptr_array_add(list, entry);
  }
  if (list != NULL && rc != SQLITE_DONE) {
    nd_store_db_set_sql_error(store->db, error, "file list");
    g_clear_pointer(&list, g_ptr_array_unref);
  }

  sqlite3_finalize(stmt);
  return list;
}

gboolean
nd_file_store_count(NdFileStore *store,
                    guint       *out_count,
                    GError     **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  return nd_store_db_count_rows(store->db, ND_STORE_COLLECTION_FILES,
                                out_count, error);
}

gchar *
nd_file_store_get_ctag(NdFileStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  return nd_store_db_get_ctag(store->db, ND_STORE_COLLECTION_FILES, error);
}
