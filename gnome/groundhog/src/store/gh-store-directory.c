#include "gh-store-directory.h"

#include <string.h>

#include <sqlite3.h>

void
gh_store_directory_entry_free(GhStoreDirectoryEntry *entry)
{
  if (!entry)
    return;
  g_free(entry->pubkey);
  g_free(entry->event_id);
  g_free(entry->event_json);
  g_free(entry);
}

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
  g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "Directory entry: %s", why);
  return FALSE;
}

static gboolean
writable(GhStore *store, GError **error)
{
  if (!gh_store_is_read_only(store))
    return TRUE;
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT, "The store is read-only");
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
  gh_store_set_sqlite_error(store, rc, "Preparing a directory statement", error);
  return NULL;
}

static gboolean
run(GhStore *store, sqlite3_stmt *stmt, const gchar *what, GError **error)
{
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE)
    return TRUE;
  return gh_store_set_sqlite_error(store, rc, what, error);
}

gboolean
gh_store_directory_put(GhStore *store, const GhStoreDirectoryEntry *entry, GError **error)
{
  g_return_val_if_fail(store != NULL && entry != NULL, FALSE);
  if (!is_hex64(entry->pubkey) || !is_hex64(entry->event_id))
    return invalid(error, "pubkey and event id must be 64 lowercase hex");
  if (entry->kind < 0 || entry->kind > 65535)
    return invalid(error, "kind out of range");
  if (!entry->event_json || strlen(entry->event_json) > GH_STORE_DIRECTORY_MAX_EVENT)
    return invalid(error, "missing or oversized event");
  if (!writable(store, error))
    return FALSE;
  /* Newest wins (NIP-01): an older event never replaces a stored one, but
   * the fetch time always moves. */
  sqlite3_stmt *stmt = prepare(store,
    "INSERT INTO directory (pubkey, kind, event_id, created_at, event_json, fetched_at) "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
    "ON CONFLICT (pubkey, kind) DO UPDATE SET "
    "  event_id   = CASE WHEN excluded.created_at > created_at OR "
    "                         (excluded.created_at = created_at AND excluded.event_id < event_id) "
    "                    THEN excluded.event_id ELSE event_id END, "
    "  event_json = CASE WHEN excluded.created_at > created_at OR "
    "                         (excluded.created_at = created_at AND excluded.event_id < event_id) "
    "                    THEN excluded.event_json ELSE event_json END, "
    "  created_at = MAX(created_at, excluded.created_at), "
    "  fetched_at = MAX(fetched_at, excluded.fetched_at)", error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, entry->pubkey, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, entry->kind);
  sqlite3_bind_text(stmt, 3, entry->event_id, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 4, entry->created_at);
  sqlite3_bind_text(stmt, 5, entry->event_json, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 6, entry->fetched_at);
  return run(store, stmt, "Storing a directory entry", error);
}

gboolean
gh_store_directory_touch(GhStore *store, const gchar *pubkey, gint kind, gint64 fetched_at,
                         GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!is_hex64(pubkey))
    return invalid(error, "pubkey must be 64 lowercase hex");
  if (!writable(store, error))
    return FALSE;
  sqlite3_stmt *stmt = prepare(store,
    "UPDATE directory SET fetched_at = MAX(fetched_at, ?2) "
    "WHERE pubkey = ?1 AND (?3 < 0 OR kind = ?3)", error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, pubkey, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 2, fetched_at);
  sqlite3_bind_int(stmt, 3, kind);
  return run(store, stmt, "Updating a directory entry", error);
}

GPtrArray *
gh_store_directory_load(GhStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  sqlite3_stmt *stmt = prepare(store,
    "SELECT pubkey, kind, event_id, created_at, event_json, fetched_at FROM directory "
    "ORDER BY pubkey, kind", error);
  if (!stmt)
    return NULL;
  GPtrArray *entries =
    g_ptr_array_new_with_free_func((GDestroyNotify)gh_store_directory_entry_free);
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    GhStoreDirectoryEntry *entry = g_new0(GhStoreDirectoryEntry, 1);
    entry->pubkey = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    entry->kind = sqlite3_column_int(stmt, 1);
    entry->event_id = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
    entry->created_at = sqlite3_column_int64(stmt, 3);
    entry->event_json = g_strdup((const gchar *)sqlite3_column_text(stmt, 4));
    entry->fetched_at = sqlite3_column_int64(stmt, 5);
    g_ptr_array_add(entries, entry);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    g_ptr_array_unref(entries);
    gh_store_set_sqlite_error(store, rc, "Loading the directory", error);
    return NULL;
  }
  return entries;
}

gboolean
gh_store_directory_delete(GhStore *store, const gchar *pubkey, gint kind, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!is_hex64(pubkey))
    return invalid(error, "pubkey must be 64 lowercase hex");
  if (!writable(store, error))
    return FALSE;
  sqlite3_stmt *stmt = prepare(store,
    "DELETE FROM directory WHERE pubkey = ?1 AND (?2 < 0 OR kind = ?2)", error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, pubkey, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, kind);
  return run(store, stmt, "Deleting a directory entry", error);
}
