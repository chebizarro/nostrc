#include "gh-store-media.h"

#include <sodium.h>
#include <sqlite3.h>
#include <string.h>

static gboolean
is_hex64(const gchar *s)
{
  if (!s || strlen(s) != 64)
    return FALSE;
  for (guint i = 0; i < 64; i++)
    if (!g_ascii_isdigit(s[i]) && (s[i] < 'a' || s[i] > 'f'))
      return FALSE;
  return TRUE;
}

static gboolean
invalid(GError **error, const gchar *why)
{
  g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "Media cache: %s", why);
  return FALSE;
}

/* Refuses to run inside a transaction SQLite already abandoned (see
 * gh-store-conversations.c). */
static sqlite3_stmt *
prepare(GhStore *store, const char *sql, GError **error)
{
  struct sqlite3 *db = gh_store_get_db(store);
  if (gh_store_get_transaction_depth(store) > 0 && sqlite3_get_autocommit(db)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                        "The store transaction was rolled back by an earlier error");
    return NULL;
  }
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK)
    return stmt;
  sqlite3_finalize(stmt);
  gh_store_set_sqlite_error(store, rc, "Preparing a media cache statement", error);
  return NULL;
}

static gboolean
step_done(GhStore *store, sqlite3_stmt *stmt, const gchar *what, GError **error)
{
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE || gh_store_set_sqlite_error(store, rc, what, error);
}

static gboolean
writable(GhStore *store, GError **error)
{
  if (!gh_store_is_read_only(store))
    return TRUE;
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT, "The store is read-only");
  return FALSE;
}

gboolean
gh_store_media_get_total(GhStore *store, gint64 *out_bytes, GError **error)
{
  g_return_val_if_fail(store != NULL && out_bytes != NULL, FALSE);
  *out_bytes = 0;
  sqlite3_stmt *stmt = prepare(store, "SELECT COALESCE(SUM(LENGTH(bytes)), 0) FROM media",
                               error);
  if (!stmt)
    return FALSE;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    *out_bytes = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW || gh_store_set_sqlite_error(store, rc, "Measuring the media cache",
                                                       error);
}

/* Evicts least recently used rows (ties: by key) until the total fits. */
static gboolean
evict(GhStore *store, gint64 cap, GError **error)
{
  gint64 total = 0;
  if (!gh_store_media_get_total(store, &total, error))
    return FALSE;
  while (total > cap) {
    sqlite3_stmt *stmt = prepare(store, "SELECT sha256, LENGTH(bytes) FROM media "
                                        "ORDER BY last_used, sha256 LIMIT 1", error);
    if (!stmt)
      return FALSE;
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      return rc == SQLITE_DONE || gh_store_set_sqlite_error(store, rc, "Evicting media", error);
    }
    g_autofree gchar *oldest = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    total -= sqlite3_column_int64(stmt, 1);
    sqlite3_finalize(stmt);
    if (!gh_store_media_remove(store, oldest, error))
      return FALSE;
  }
  return TRUE;
}

static gboolean
put_in_transaction(GhStore *store, gpointer data, GError **error)
{
  gpointer *args = data;
  const gchar *sha256 = args[0], *mime = args[1];
  GBytes *bytes = args[2];
  gsize size = 0;
  gconstpointer raw = g_bytes_get_data(bytes, &size);
  /* Only for a file a stored message names: plaintext never outlives its
   * message (a download that finishes after the message expired or was
   * forgotten keeps nothing). */
  sqlite3_stmt *named = prepare(store, "SELECT EXISTS (SELECT 1 FROM messages m WHERE "
                                       GH_STORE_MEDIA_FILE("m") " AND "
                                       GH_STORE_MEDIA_X_OF("m") " = ?1)", error);
  if (!named)
    return FALSE;
  sqlite3_bind_text(named, 1, sha256, -1, SQLITE_STATIC);
  int rc = sqlite3_step(named);
  gboolean exists = rc == SQLITE_ROW && sqlite3_column_int(named, 0) != 0;
  sqlite3_finalize(named);
  if (rc != SQLITE_ROW)
    return gh_store_set_sqlite_error(store, rc, "Looking up the file's message", error);
  if (!exists) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND,
                        "Media cache: no stored message names this file");
    return FALSE;
  }
  sqlite3_stmt *stmt = prepare(store, "INSERT OR REPLACE INTO media (sha256, mime, bytes, "
                                      "last_used) VALUES (?1, ?2, ?3, ?4)", error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, sha256, -1, SQLITE_STATIC);
  if (mime)
    sqlite3_bind_text(stmt, 2, mime, -1, SQLITE_STATIC);
  else
    sqlite3_bind_null(stmt, 2);
  sqlite3_bind_blob64(stmt, 3, raw, size, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 4, gh_clock_get_unix(gh_store_get_clock(store)));
  return step_done(store, stmt, "Caching media", error) &&
         evict(store, GH_STORE_MEDIA_CAP, error);
}

gboolean
gh_store_media_put(GhStore *store, const gchar *sha256, const gchar *mime, GBytes *bytes,
                   GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!is_hex64(sha256))
    return invalid(error, "the key must be 64 lowercase hex");
  gsize size = bytes ? g_bytes_get_size(bytes) : 0;
  if (size == 0 || size > GH_STORE_MAX_VALUE_SIZE || size > GH_STORE_MEDIA_CAP)
    return invalid(error, "the item is empty or larger than the cache allows");
  if (mime && strlen(mime) > 127)
    return invalid(error, "the MIME type is too long");
  if (!writable(store, error))
    return FALSE;
  gpointer args[] = { (gpointer)sha256, (gpointer)mime, bytes };
  return gh_store_transaction(store, put_in_transaction, args, error);
}

static void
wipe_free(gpointer data)
{
  sodium_free(data);
}

GBytes *
gh_store_media_get(GhStore *store, const gchar *sha256, gchar **out_mime, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  if (out_mime)
    *out_mime = NULL;
  if (!is_hex64(sha256)) {
    invalid(error, "the key must be 64 lowercase hex");
    return NULL;
  }
  sqlite3_stmt *stmt = prepare(store, "SELECT mime, bytes FROM media WHERE sha256 = ?1", error);
  if (!stmt)
    return NULL;
  sqlite3_bind_text(stmt, 1, sha256, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
      gh_store_set_sqlite_error(store, rc, "Reading the media cache", error);
    return NULL;
  }
  const void *raw = sqlite3_column_blob(stmt, 1);
  gsize size = (gsize)sqlite3_column_bytes(stmt, 1);
  /* Guarded memory, wiped when the last reference goes. */
  guint8 *copy = size ? sodium_malloc(size) : NULL;
  if (size && (!copy || !raw)) {
    sqlite3_finalize(stmt);
    if (copy)
      sodium_free(copy);
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                        "Out of memory reading the media cache");
    return NULL;
  }
  if (size)
    memcpy(copy, raw, size);
  if (out_mime && sqlite3_column_type(stmt, 0) == SQLITE_TEXT)
    *out_mime = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  GBytes *bytes = size ? g_bytes_new_with_free_func(copy, size, wipe_free, copy)
                       : g_bytes_new(NULL, 0);
  if (!gh_store_is_read_only(store)) {
    sqlite3_stmt *touch = prepare(store, "UPDATE media SET last_used = ?1 WHERE sha256 = ?2",
                                  NULL);
    if (touch) {
      sqlite3_bind_int64(touch, 1, gh_clock_get_unix(gh_store_get_clock(store)));
      sqlite3_bind_text(touch, 2, sha256, -1, SQLITE_STATIC);
      (void)step_done(store, touch, "Marking media used", NULL);
    }
  }
  return bytes;
}

gboolean
gh_store_media_remove(GhStore *store, const gchar *sha256, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!is_hex64(sha256))
    return invalid(error, "the key must be 64 lowercase hex");
  if (!writable(store, error))
    return FALSE;
  sqlite3_stmt *stmt = prepare(store, "DELETE FROM media WHERE sha256 = ?1", error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, sha256, -1, SQLITE_STATIC);
  return step_done(store, stmt, "Removing media", error);
}

gboolean
gh_store_media_prune(GhStore *store, gint64 cap, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (cap < 0)
    return invalid(error, "a negative size");
  if (!writable(store, error))
    return FALSE;
  return evict(store, cap, error);
}
