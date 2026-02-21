/*
 * bc-db-backend-sqlite.c - SQLite implementation of BcDbBackend
 *
 * SPDX-License-Identifier: MIT
 *
 * Default metadata backend. Uses WAL mode for concurrent read performance.
 */

#include "bc-db-backend.h"
#include <sqlite3.h>
#include <string.h>

/* ── Error domain ──────────────────────────────────────────────────────── */

#define BC_DB_SQLITE_ERROR (bc_db_sqlite_error_quark())
G_DEFINE_QUARK(bc-db-sqlite-error-quark, bc_db_sqlite_error)

enum {
  BC_DB_SQLITE_ERROR_OPEN,
  BC_DB_SQLITE_ERROR_EXEC,
  BC_DB_SQLITE_ERROR_PREPARE,
};

/* ── Private context ───────────────────────────────────────────────────── */

typedef struct {
  sqlite3 *db;
} SqliteCtx;

/* ── Helpers ───────────────────────────────────────────────────────────── */

static gboolean
db_exec(sqlite3 *db, const char *sql, GError **error)
{
  char *errmsg = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
  if (rc != SQLITE_OK) {
    g_set_error(error, BC_DB_SQLITE_ERROR, BC_DB_SQLITE_ERROR_EXEC,
                "SQLite error: %s", errmsg ? errmsg : sqlite3_errmsg(db));
    sqlite3_free(errmsg);
    return FALSE;
  }
  return TRUE;
}

static gboolean
db_init_schema(sqlite3 *db, GError **error)
{
  static const char *schema_sql =
    "CREATE TABLE IF NOT EXISTS blobs ("
    "  sha256        TEXT PRIMARY KEY NOT NULL,"
    "  size          INTEGER NOT NULL,"
    "  mime_type     TEXT,"
    "  created_at    INTEGER NOT NULL,"
    "  last_accessed INTEGER NOT NULL,"
    "  access_count  INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_blobs_last_accessed ON blobs(last_accessed);"
    "CREATE INDEX IF NOT EXISTS idx_blobs_size ON blobs(size);"
    "CREATE INDEX IF NOT EXISTS idx_blobs_created_at ON blobs(created_at);";

  return db_exec(db, schema_sql, error);
}

/* ── Backend operations ────────────────────────────────────────────────── */

static gboolean
sqlite_contains(void *ctx, const char *sha256)
{
  SqliteCtx *sc = ctx;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(sc->db,
    "SELECT 1 FROM blobs WHERE sha256 = ?1 LIMIT 1",
    -1, &stmt, NULL);
  if (rc != SQLITE_OK) return FALSE;

  sqlite3_bind_text(stmt, 1, sha256, -1, SQLITE_STATIC);
  gboolean found = (sqlite3_step(stmt) == SQLITE_ROW);
  sqlite3_finalize(stmt);
  return found;
}

static BcDbBlobMeta *
sqlite_get_info(void *ctx, const char *sha256, GError **error)
{
  SqliteCtx *sc = ctx;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(sc->db,
    "SELECT sha256, size, mime_type, created_at, last_accessed, access_count "
    "FROM blobs WHERE sha256 = ?1",
    -1, &stmt, NULL);

  if (rc != SQLITE_OK) {
    g_set_error(error, BC_DB_SQLITE_ERROR, BC_DB_SQLITE_ERROR_PREPARE,
                "SQLite prepare failed: %s", sqlite3_errmsg(sc->db));
    return NULL;
  }

  sqlite3_bind_text(stmt, 1, sha256, -1, SQLITE_STATIC);

  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return NULL; /* not found — no error set, caller checks NULL */
  }

  BcDbBlobMeta *meta = g_new0(BcDbBlobMeta, 1);
  meta->sha256        = g_strdup((const char *)sqlite3_column_text(stmt, 0));
  meta->size          = sqlite3_column_int64(stmt, 1);
  meta->mime_type     = g_strdup((const char *)sqlite3_column_text(stmt, 2));
  meta->created_at    = sqlite3_column_int64(stmt, 3);
  meta->last_accessed = sqlite3_column_int64(stmt, 4);
  meta->access_count  = (uint32_t)sqlite3_column_int(stmt, 5);
  sqlite3_finalize(stmt);

  /* Touch: update last_accessed and access_count */
  int64_t now = g_get_real_time() / G_USEC_PER_SEC;
  sqlite3_stmt *update = NULL;
  rc = sqlite3_prepare_v2(sc->db,
    "UPDATE blobs SET last_accessed = ?1, access_count = access_count + 1 "
    "WHERE sha256 = ?2",
    -1, &update, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int64(update, 1, now);
    sqlite3_bind_text(update, 2, sha256, -1, SQLITE_STATIC);
    sqlite3_step(update);
    sqlite3_finalize(update);
  }

  meta->last_accessed = now;
  meta->access_count += 1;
  return meta;
}

static int64_t
sqlite_get_total_size(void *ctx)
{
  SqliteCtx *sc = ctx;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(sc->db,
    "SELECT COALESCE(SUM(size), 0) FROM blobs",
    -1, &stmt, NULL);
  if (rc != SQLITE_OK) return 0;

  int64_t total = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    total = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return total;
}

static uint32_t
sqlite_get_blob_count(void *ctx)
{
  SqliteCtx *sc = ctx;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(sc->db,
    "SELECT COUNT(*) FROM blobs", -1, &stmt, NULL);
  if (rc != SQLITE_OK) return 0;

  uint32_t count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    count = (uint32_t)sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

static gboolean
sqlite_put_meta(void *ctx, const BcDbBlobMeta *meta, GError **error)
{
  SqliteCtx *sc = ctx;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(sc->db,
    "INSERT OR IGNORE INTO blobs (sha256, size, mime_type, created_at, last_accessed, access_count) "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
    -1, &stmt, NULL);

  if (rc != SQLITE_OK) {
    g_set_error(error, BC_DB_SQLITE_ERROR, BC_DB_SQLITE_ERROR_PREPARE,
                "SQLite prepare failed: %s", sqlite3_errmsg(sc->db));
    return FALSE;
  }

  sqlite3_bind_text(stmt, 1, meta->sha256, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)meta->size);
  sqlite3_bind_text(stmt, 3, meta->mime_type ? meta->mime_type : "application/octet-stream",
                    -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 4, meta->created_at);
  sqlite3_bind_int64(stmt, 5, meta->last_accessed);
  sqlite3_bind_int(stmt, 6, (int)meta->access_count);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    g_set_error(error, BC_DB_SQLITE_ERROR, BC_DB_SQLITE_ERROR_EXEC,
                "SQLite insert failed: %s", sqlite3_errmsg(sc->db));
    return FALSE;
  }
  return TRUE;
}

static gboolean
sqlite_delete_meta(void *ctx, const char *sha256, GError **error)
{
  SqliteCtx *sc = ctx;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(sc->db,
    "DELETE FROM blobs WHERE sha256 = ?1",
    -1, &stmt, NULL);

  if (rc != SQLITE_OK) {
    g_set_error(error, BC_DB_SQLITE_ERROR, BC_DB_SQLITE_ERROR_PREPARE,
                "SQLite prepare failed: %s", sqlite3_errmsg(sc->db));
    return FALSE;
  }

  sqlite3_bind_text(stmt, 1, sha256, -1, SQLITE_STATIC);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return TRUE;
}

static GPtrArray *
sqlite_list_blobs(void *ctx, const char *cursor_sha256,
                  uint32_t limit, GError **error)
{
  SqliteCtx *sc = ctx;
  if (limit == 0) limit = 100;

  sqlite3_stmt *stmt = NULL;
  int rc;

  if (cursor_sha256 != NULL && cursor_sha256[0] != '\0') {
    rc = sqlite3_prepare_v2(sc->db,
      "SELECT sha256, size, mime_type, created_at, last_accessed, access_count "
      "FROM blobs WHERE created_at < (SELECT created_at FROM blobs WHERE sha256 = ?1) "
      "OR (created_at = (SELECT created_at FROM blobs WHERE sha256 = ?1) AND sha256 < ?1) "
      "ORDER BY created_at DESC, sha256 DESC LIMIT ?2",
      -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
      g_set_error(error, BC_DB_SQLITE_ERROR, BC_DB_SQLITE_ERROR_PREPARE,
                  "SQLite prepare failed: %s", sqlite3_errmsg(sc->db));
      return NULL;
    }
    sqlite3_bind_text(stmt, 1, cursor_sha256, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, (int)limit);
  } else {
    rc = sqlite3_prepare_v2(sc->db,
      "SELECT sha256, size, mime_type, created_at, last_accessed, access_count "
      "FROM blobs ORDER BY created_at DESC, sha256 DESC LIMIT ?1",
      -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
      g_set_error(error, BC_DB_SQLITE_ERROR, BC_DB_SQLITE_ERROR_PREPARE,
                  "SQLite prepare failed: %s", sqlite3_errmsg(sc->db));
      return NULL;
    }
    sqlite3_bind_int(stmt, 1, (int)limit);
  }

  GPtrArray *results = g_ptr_array_new_with_free_func((GDestroyNotify)bc_db_blob_meta_free);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    BcDbBlobMeta *meta = g_new0(BcDbBlobMeta, 1);
    meta->sha256        = g_strdup((const char *)sqlite3_column_text(stmt, 0));
    meta->size          = sqlite3_column_int64(stmt, 1);
    meta->mime_type     = g_strdup((const char *)sqlite3_column_text(stmt, 2));
    meta->created_at    = sqlite3_column_int64(stmt, 3);
    meta->last_accessed = sqlite3_column_int64(stmt, 4);
    meta->access_count  = (uint32_t)sqlite3_column_int(stmt, 5);
    g_ptr_array_add(results, meta);
  }

  sqlite3_finalize(stmt);
  return results;
}

static GPtrArray *
sqlite_evict_candidates(void *ctx, int64_t bytes_to_free, GError **error)
{
  SqliteCtx *sc = ctx;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(sc->db,
    "SELECT sha256, size FROM blobs ORDER BY last_accessed ASC",
    -1, &stmt, NULL);

  if (rc != SQLITE_OK) {
    g_set_error(error, BC_DB_SQLITE_ERROR, BC_DB_SQLITE_ERROR_PREPARE,
                "SQLite prepare failed: %s", sqlite3_errmsg(sc->db));
    return NULL;
  }

  GPtrArray *candidates = g_ptr_array_new_with_free_func((GDestroyNotify)bc_db_blob_meta_free);
  int64_t freed = 0;

  while (sqlite3_step(stmt) == SQLITE_ROW && freed < bytes_to_free) {
    BcDbBlobMeta *meta = g_new0(BcDbBlobMeta, 1);
    meta->sha256 = g_strdup((const char *)sqlite3_column_text(stmt, 0));
    meta->size   = sqlite3_column_int64(stmt, 1);
    g_ptr_array_add(candidates, meta);
    freed += meta->size;
  }

  sqlite3_finalize(stmt);
  return candidates;
}

static bool
sqlite_is_persistent(void *ctx)
{
  (void)ctx;
  return true;
}

static void
sqlite_destroy(void *ctx)
{
  SqliteCtx *sc = ctx;
  if (sc->db != NULL)
    sqlite3_close(sc->db);
  g_free(sc);
}

/* ── Public constructor ────────────────────────────────────────────────── */

BcDbBackend *
bc_db_backend_sqlite_new(const char *db_path, GError **error)
{
  g_return_val_if_fail(db_path != NULL, NULL);

  sqlite3 *db = NULL;
  int rc = sqlite3_open(db_path, &db);
  if (rc != SQLITE_OK) {
    g_set_error(error, BC_DB_SQLITE_ERROR, BC_DB_SQLITE_ERROR_OPEN,
                "Failed to open SQLite database: %s", sqlite3_errmsg(db));
    if (db != NULL) sqlite3_close(db);
    return NULL;
  }

  /* WAL mode for better concurrent read performance */
  db_exec(db, "PRAGMA journal_mode=WAL;", NULL);
  db_exec(db, "PRAGMA synchronous=NORMAL;", NULL);

  if (!db_init_schema(db, error)) {
    sqlite3_close(db);
    return NULL;
  }

  SqliteCtx *sc = g_new0(SqliteCtx, 1);
  sc->db = db;

  BcDbBackend *backend = g_new0(BcDbBackend, 1);
  backend->ctx             = sc;
  backend->contains        = sqlite_contains;
  backend->get_info        = sqlite_get_info;
  backend->get_total_size  = sqlite_get_total_size;
  backend->get_blob_count  = sqlite_get_blob_count;
  backend->put_meta        = sqlite_put_meta;
  backend->delete_meta     = sqlite_delete_meta;
  backend->list_blobs      = sqlite_list_blobs;
  backend->evict_candidates = sqlite_evict_candidates;
  backend->is_persistent   = sqlite_is_persistent;
  backend->destroy         = sqlite_destroy;

  return backend;
}
