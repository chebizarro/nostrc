#include "gh-store-reports.h"
#include <sqlite3.h>

void
gh_store_report_free(GhStoreReport *r)
{
  if (!r) return;
  g_free(r->unsigned_json); g_free(r->signed_json); g_free(r->targets_json);
  g_free(r->outcomes_json); g_free(r->state); g_free(r);
}
static sqlite3_stmt *
prepare(GhStore *store, const gchar *sql, GError **error)
{
  sqlite3_stmt *stmt = NULL;
  gint rc = sqlite3_prepare_v2(gh_store_get_db(store), sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) return stmt;
  sqlite3_finalize(stmt);
  gh_store_set_sqlite_error(store, rc, "Preparing report storage", error);
  return NULL;
}
static gboolean
finish(GhStore *store, sqlite3_stmt *stmt, GError **error)
{
  gint rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE || gh_store_set_sqlite_error(store, rc, "Saving report", error);
}
gint64
gh_store_report_approve(GhStore *store, const gchar *json, const gchar *targets,
                        gint64 created_at, GError **error)
{
  sqlite3_stmt *stmt = prepare(store,
      "INSERT INTO report_publications(unsigned_json,targets_json,outcomes_json,state,created_at) "
      "VALUES(?,?,'{}','approved',?)", error);
  if (!stmt) return 0;
  sqlite3_bind_text(stmt, 1, json, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, targets, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, created_at);
  return finish(store, stmt, error) ? sqlite3_last_insert_rowid(gh_store_get_db(store)) : 0;
}
static GhStoreReport *
read_row(sqlite3_stmt *stmt)
{
  GhStoreReport *r = g_new0(GhStoreReport, 1);
  r->id = sqlite3_column_int64(stmt, 0);
  r->created_at = sqlite3_column_int64(stmt, 1);
  r->unsigned_json = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
  r->signed_json = g_strdup((const gchar *)sqlite3_column_text(stmt, 3));
  r->targets_json = g_strdup((const gchar *)sqlite3_column_text(stmt, 4));
  r->outcomes_json = g_strdup((const gchar *)sqlite3_column_text(stmt, 5));
  r->state = g_strdup((const gchar *)sqlite3_column_text(stmt, 6));
  return r;
}
#define REPORT_COLUMNS "id,created_at,unsigned_json,signed_json,targets_json,outcomes_json,state"
GhStoreReport *
gh_store_report_load(GhStore *store, gint64 id, GError **error)
{
  sqlite3_stmt *stmt = prepare(store, "SELECT " REPORT_COLUMNS " FROM report_publications WHERE id=?", error);
  if (!stmt) return NULL;
  sqlite3_bind_int64(stmt, 1, id);
  gint rc = sqlite3_step(stmt);
  GhStoreReport *r = rc == SQLITE_ROW ? read_row(stmt) : NULL;
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    gh_store_set_sqlite_error(store, rc, "Reading report", error);
  else if (!r) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Report not found");
  return r;
}
GPtrArray *
gh_store_report_list(GhStore *store, GError **error)
{
  sqlite3_stmt *stmt = prepare(store, "SELECT " REPORT_COLUMNS " FROM report_publications ORDER BY id DESC", error);
  if (!stmt) return NULL;
  GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)gh_store_report_free);
  gint rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) g_ptr_array_add(rows, read_row(stmt));
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(store, rc, "Reading reports", error);
    g_ptr_array_unref(rows); return NULL;
  }
  return rows;
}
gboolean
gh_store_report_update(GhStore *store, gint64 id, const gchar *signed_json,
                       const gchar *outcomes, const gchar *state, GError **error)
{
  sqlite3_stmt *stmt = prepare(store,
      "UPDATE report_publications SET signed_json=COALESCE(?,signed_json),outcomes_json=?,state=? WHERE id=?", error);
  if (!stmt) return FALSE;
  if (signed_json) sqlite3_bind_text(stmt, 1, signed_json, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, outcomes, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, state, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, id);
  return finish(store, stmt, error);
}
