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
  sqlite3_stmt *stmt = prepare(store, "SELECT (SELECT COALESCE(SUM(LENGTH(bytes)), 0) FROM "
                                      "media) + (SELECT COALESCE(SUM(LENGTH(bytes)), 0) FROM "
                                      "group_images)",
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

static gboolean remove_id(GhStore *store, const gchar *id, GError **error);

static void
wipe_free(gpointer data)
{
  sodium_free(data);
}

/* The row key of file: its identity (x, key, nonce), never x alone. */
static gchar *
file_id(const GhNip17File *file, GError **error)
{
  gchar *id = file ? gh_store_media_file_id(file->x, file->key, sizeof file->key, file->nonce,
                                            file->nonce_size)
                   : NULL;
  if (!id)
    invalid(error, "the file's x, key and nonce are required");
  return id;
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
    if (!remove_id(store, oldest, error))
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
  /* Only for a file a stored message carries (the same x, key and nonce):
   * plaintext never outlives its message (a download that finishes after the
   * message expired or was forgotten keeps nothing), and a message that only
   * names another file's x can never pin that file. */
  sqlite3_stmt *named = prepare(store, "SELECT EXISTS (SELECT 1 FROM messages m WHERE "
                                       "m.kind = 15 AND gh_media_file_id(m.raw_json) = ?1) "
                                       "OR EXISTS (SELECT 1 FROM message_media WHERE "
                                       "file_id = ?1)",
                                error);
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
                        "Media cache: no stored message carries this file");
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

static gboolean put_id(GhStore *store, const gchar *sha256, const gchar *mime, GBytes *bytes,
                       GError **error);

gboolean
gh_store_media_put(GhStore *store, const GhNip17File *file, const gchar *mime, GBytes *bytes,
                   GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_autofree gchar *sha256 = file_id(file, error);
  return sha256 && put_id(store, sha256, mime, bytes, error);
}

gboolean
gh_store_media_put_id(GhStore *store, const gchar *file_id_hex, const gchar *mime,
                      GBytes *bytes, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!is_hex64(file_id_hex))
    return invalid(error, "the key must be 64 lowercase hex");
  return put_id(store, file_id_hex, mime, bytes, error);
}

static gboolean
put_id(GhStore *store, const gchar *sha256, const gchar *mime, GBytes *bytes, GError **error)
{
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

static GBytes *get_id(GhStore *store, const gchar *sha256, gchar **out_mime, GError **error);

GBytes *
gh_store_media_get(GhStore *store, const GhNip17File *file, gchar **out_mime, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  if (out_mime)
    *out_mime = NULL;
  g_autofree gchar *sha256 = file_id(file, error);
  return sha256 ? get_id(store, sha256, out_mime, error) : NULL;
}

GBytes *
gh_store_media_get_id(GhStore *store, const gchar *file_id_hex, gchar **out_mime,
                      GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  if (out_mime)
    *out_mime = NULL;
  if (!is_hex64(file_id_hex)) {
    invalid(error, "the key must be 64 lowercase hex");
    return NULL;
  }
  return get_id(store, file_id_hex, out_mime, error);
}

/* Guarded memory, wiped when the last reference goes. */
static GBytes *
guarded_copy(const void *raw, gsize size, GError **error)
{
  guint8 *copy = size ? sodium_malloc(size) : NULL;
  if (size && (!copy || !raw)) {
    if (copy)
      sodium_free(copy);
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                        "Out of memory reading the media cache");
    return NULL;
  }
  if (!size)
    return g_bytes_new(NULL, 0);
  memcpy(copy, raw, size);
  return g_bytes_new_with_free_func(copy, size, wipe_free, copy);
}

static GBytes *
get_id(GhStore *store, const gchar *sha256, gchar **out_mime, GError **error)
{
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
  GBytes *bytes = guarded_copy(raw, size, error);
  if (!bytes) {
    sqlite3_finalize(stmt);
    return NULL;
  }
  if (out_mime && sqlite3_column_type(stmt, 0) == SQLITE_TEXT)
    *out_mime = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
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
gh_store_media_remove(GhStore *store, const GhNip17File *file, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_autofree gchar *id = file_id(file, error);
  return id && remove_id(store, id, error);
}

static gboolean
remove_id(GhStore *store, const gchar *sha256, GError **error)
{
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
  /* Clear (cap 0) deletes every decrypted copy, the group pictures too. */
  if (cap == 0) {
    sqlite3_stmt *stmt = prepare(store, "DELETE FROM group_images", error);
    if (!stmt || !step_done(store, stmt, "Clearing the group pictures", error))
      return FALSE;
  }
  return evict(store, cap, error);
}

/* ---- group pictures (W25) --------------------------------------------------------------- */

static gboolean
group_conversation(GhStore *store, const gchar *group_id_hex, gint64 *out_id, GError **error)
{
  sqlite3_stmt *stmt = prepare(store, "SELECT id FROM conversations WHERE backend = 3 AND "
                                      "backend_key = ?1", error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, group_id_hex, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  *out_id = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    return gh_store_set_sqlite_error(store, rc, "Looking up the group", error);
  if (rc == SQLITE_DONE) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND,
                        "Group pictures: no such group");
    return FALSE;
  }
  return TRUE;
}

static gboolean
group_args_valid(const gchar *group_id_hex, const gchar *image_id_hex, GError **error)
{
  gsize n = group_id_hex ? strlen(group_id_hex) : 0;
  gboolean ok = n >= 2 && n <= 510 && n % 2 == 0;
  for (gsize i = 0; ok && i < n; i++)
    ok = g_ascii_isdigit(group_id_hex[i]) || (group_id_hex[i] >= 'a' && group_id_hex[i] <= 'f');
  if (!ok)
    return invalid(error, "a group id is lowercase hex");
  if (image_id_hex && !is_hex64(image_id_hex))
    return invalid(error, "a picture identity is 64 lowercase hex");
  return TRUE;
}

gboolean
gh_store_group_image_put(GhStore *store, const gchar *group_id_hex, const gchar *image_id_hex,
                         const gchar *mime, GBytes *bytes, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!group_args_valid(group_id_hex, image_id_hex, error))
    return FALSE;
  if (!image_id_hex)
    return invalid(error, "a picture identity is required");
  gsize size = bytes ? g_bytes_get_size(bytes) : 0;
  if (size == 0 || size > GH_STORE_GROUP_IMAGE_MAX)
    return invalid(error, "the picture is empty or larger than the cache allows");
  if (mime && strlen(mime) > 127)
    return invalid(error, "the MIME type is too long");
  if (!writable(store, error))
    return FALSE;
  gint64 conversation = 0;
  if (!group_conversation(store, group_id_hex, &conversation, error))
    return FALSE;
  sqlite3_stmt *stmt = prepare(store, "INSERT OR REPLACE INTO group_images (conversation_id, "
                                      "image_id, mime, bytes, fetched_at) VALUES "
                                      "(?1, ?2, ?3, ?4, ?5)", error);
  if (!stmt)
    return FALSE;
  gsize len = 0;
  gconstpointer raw = g_bytes_get_data(bytes, &len);
  sqlite3_bind_int64(stmt, 1, conversation);
  sqlite3_bind_text(stmt, 2, image_id_hex, -1, SQLITE_STATIC);
  if (mime)
    sqlite3_bind_text(stmt, 3, mime, -1, SQLITE_STATIC);
  else
    sqlite3_bind_null(stmt, 3);
  sqlite3_bind_blob64(stmt, 4, raw, len, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 5, gh_clock_get_unix(gh_store_get_clock(store)));
  return step_done(store, stmt, "Caching a group picture", error);
}

GBytes *
gh_store_group_image_get(GhStore *store, const gchar *group_id_hex, const gchar *image_id_hex,
                         gchar **out_mime, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  if (out_mime)
    *out_mime = NULL;
  if (!group_args_valid(group_id_hex, image_id_hex, error))
    return NULL;
  if (!image_id_hex) {
    invalid(error, "a picture identity is required");
    return NULL;
  }
  sqlite3_stmt *stmt = prepare(store, "SELECT g.mime, g.bytes FROM group_images g JOIN "
                                      "conversations c ON c.id = g.conversation_id WHERE "
                                      "c.backend = 3 AND c.backend_key = ?1 AND g.image_id = ?2",
                               error);
  if (!stmt)
    return NULL;
  sqlite3_bind_text(stmt, 1, group_id_hex, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, image_id_hex, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
      gh_store_set_sqlite_error(store, rc, "Reading a group picture", error);
    return NULL;
  }
  GBytes *bytes = guarded_copy(sqlite3_column_blob(stmt, 1),
                               (gsize)sqlite3_column_bytes(stmt, 1), error);
  if (bytes && out_mime && sqlite3_column_type(stmt, 0) == SQLITE_TEXT)
    *out_mime = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return bytes;
}

gboolean
gh_store_group_image_forget(GhStore *store, const gchar *group_id_hex, const gchar *keep_id_hex,
                            GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!group_args_valid(group_id_hex, keep_id_hex, error) || !writable(store, error))
    return FALSE;
  sqlite3_stmt *stmt = prepare(store, "DELETE FROM group_images WHERE conversation_id IN "
                                      "(SELECT id FROM conversations WHERE backend = 3 AND "
                                      "backend_key = ?1) AND image_id IS NOT ?2", error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, group_id_hex, -1, SQLITE_STATIC);
  if (keep_id_hex)
    sqlite3_bind_text(stmt, 2, keep_id_hex, -1, SQLITE_STATIC);
  else
    sqlite3_bind_null(stmt, 2);
  return step_done(store, stmt, "Forgetting a group picture", error);
}
