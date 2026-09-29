#include "gh-store-blossom.h"

#include <string.h>

#include <sqlite3.h>

/* The key range of the consent rows: every key that starts with the prefix
 * sorts before the prefix with its last byte raised by one. */
#define CONSENT_RANGE_END "blossom-consent;"
G_STATIC_ASSERT(sizeof GH_STORE_BLOSSOM_CONSENT_PREFIX == sizeof CONSENT_RANGE_END);

static gboolean
invalid(GError **error, const gchar *why)
{
  g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "Attachment server: %s", why);
  return FALSE;
}

/* A normalized http(s) URL: the caller normalizes, this refuses anything
 * that is plainly not one. */
static gboolean
server_ok(const gchar *server)
{
  if (!server || !*server || strlen(server) > GH_STORE_BLOSSOM_MAX_SERVER ||
      !g_utf8_validate(server, -1, NULL))
    return FALSE;
  for (const gchar *p = server; *p; p++)
    if ((guchar)*p < 0x20 || *p == 0x7f)
      return FALSE;
  return g_str_has_prefix(server, "https://") || g_str_has_prefix(server, "http://");
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
  gh_store_set_sqlite_error(store, rc, "Preparing an attachment server statement", error);
  return NULL;
}

gboolean
gh_store_blossom_set_consent(GhStore *store, const gchar *server, gboolean consent, gint64 now,
                             GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!server_ok(server))
    return invalid(error, "not a server address");
  if (consent && now <= 0)
    return invalid(error, "a consent needs its time");
  if (gh_store_is_read_only(store)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT, "The store is read-only");
    return FALSE;
  }
  g_autofree gchar *key = g_strconcat(GH_STORE_BLOSSOM_CONSENT_PREFIX, server, NULL);
  sqlite3_stmt *stmt = prepare(store, consent
    ? "INSERT INTO meta (key, value) VALUES (?1, ?2) "
      "ON CONFLICT (key) DO UPDATE SET value = excluded.value"
    : "DELETE FROM meta WHERE key = ?1", error);
  if (!stmt)
    return FALSE;
  g_autofree gchar *when = g_strdup_printf("%" G_GINT64_FORMAT, now);
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  if (consent)
    sqlite3_bind_text(stmt, 2, when, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE)
    return TRUE;
  return gh_store_set_sqlite_error(store, rc, "Saving an attachment server consent", error);
}

GStrv
gh_store_blossom_dup_consents(GhStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  sqlite3_stmt *stmt = prepare(store, "SELECT key FROM meta WHERE key >= ?1 AND key < ?2 "
                                      "ORDER BY key", error);
  if (!stmt)
    return NULL;
  sqlite3_bind_text(stmt, 1, GH_STORE_BLOSSOM_CONSENT_PREFIX, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, CONSENT_RANGE_END, -1, SQLITE_STATIC);
  g_autoptr(GStrvBuilder) servers = g_strv_builder_new();
  const gsize prefix = strlen(GH_STORE_BLOSSOM_CONSENT_PREFIX);
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const gchar *key = (const gchar *)sqlite3_column_text(stmt, 0);
    /* Only a row this module could have written counts. */
    if (key && g_str_has_prefix(key, GH_STORE_BLOSSOM_CONSENT_PREFIX) && server_ok(key + prefix))
      g_strv_builder_add(servers, key + prefix);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(store, rc, "Reading attachment server consents", error);
    return NULL;
  }
  return g_strv_builder_end(servers);
}
