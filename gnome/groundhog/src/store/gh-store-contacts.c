#include "gh-store-contacts.h"

#include <string.h>

#include <sqlite3.h>

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
  g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "Contact: %s", why);
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
  gh_store_set_sqlite_error(store, rc, "Preparing a contacts statement", error);
  return NULL;
}

gboolean
gh_store_contacts_get_verified(GhStore *store, const gchar *pubkey, gint64 *out_verified_at,
                               GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(out_verified_at != NULL, FALSE);
  *out_verified_at = 0;
  if (!is_hex64(pubkey))
    return invalid(error, "the key must be 64 lowercase hex");
  sqlite3_stmt *stmt = prepare(store, "SELECT verified_at FROM contacts WHERE pubkey = ?1",
                               error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, pubkey, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    *out_verified_at = MAX(sqlite3_column_int64(stmt, 0), 0);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_ROW || rc == SQLITE_DONE)
    return TRUE;
  return gh_store_set_sqlite_error(store, rc, "Reading a contact", error);
}

gboolean
gh_store_contacts_set_verified(GhStore *store, const gchar *pubkey, gint64 verified_at,
                               GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!is_hex64(pubkey))
    return invalid(error, "the key must be 64 lowercase hex");
  if (verified_at < 0)
    return invalid(error, "a verification cannot predate 1970");
  if (gh_store_is_read_only(store)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT, "The store is read-only");
    return FALSE;
  }
  /* Clearing a mark never creates a row. */
  sqlite3_stmt *stmt = prepare(store, verified_at > 0
    ? "INSERT INTO contacts (pubkey, verified_at) VALUES (?1, ?2) "
      "ON CONFLICT (pubkey) DO UPDATE SET verified_at = excluded.verified_at"
    : "UPDATE contacts SET verified_at = 0 WHERE pubkey = ?1", error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, pubkey, -1, SQLITE_STATIC);
  if (verified_at > 0)
    sqlite3_bind_int64(stmt, 2, verified_at);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE)
    return TRUE;
  return gh_store_set_sqlite_error(store, rc, "Marking a contact verified", error);
}
