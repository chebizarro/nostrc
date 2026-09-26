/* nd-calendar-store.c - NIP-52 event store for CalDAV (SQLite-backed)
 *
 * SPDX-License-Identifier: MIT
 *
 * The ICS text is the stored representation; kind/pubkey/created_at are
 * kept in their own columns because they are not carried by ICS and feed
 * the ETag, which must survive a restart unchanged.
 */

#include "nd-calendar-store.h"

#include <sqlite3.h>

struct _NdCalendarStore {
  NdStoreDb *db;
};

#define EVENT_COLUMNS "uid, ical, kind, pubkey, created_at"

static NdCalendarEvent *
event_from_row(sqlite3_stmt *stmt, GError **error)
{
  const gchar *uid  = (const gchar *)sqlite3_column_text(stmt, 0);
  const gchar *ical = (const gchar *)sqlite3_column_text(stmt, 1);

  GError *parse_err = NULL;
  NdCalendarEvent *event = nd_ical_parse_vevent(ical ? ical : "", &parse_err);
  if (event == NULL) {
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_DATA,
                "Stored calendar event %s is unreadable: %s",
                uid ? uid : "(null)",
                parse_err ? parse_err->message : "parse failed");
    g_clear_error(&parse_err);
    return NULL;
  }

  g_free(event->uid);
  event->uid = g_strdup(uid);
  event->kind = sqlite3_column_int(stmt, 2);
  g_free(event->pubkey);
  event->pubkey = g_strdup((const gchar *)sqlite3_column_text(stmt, 3));
  event->created_at = sqlite3_column_int64(stmt, 4);
  return event;
}

NdCalendarStore *
nd_calendar_store_new(NdStoreDb *db)
{
  g_return_val_if_fail(db != NULL, NULL);
  NdCalendarStore *store = g_new0(NdCalendarStore, 1);
  store->db = nd_store_db_ref(db);
  return store;
}

void
nd_calendar_store_free(NdCalendarStore *store)
{
  if (store == NULL) return;
  nd_store_db_unref(store->db);
  g_free(store);
}

gboolean
nd_calendar_store_put(NdCalendarStore       *store,
                      const NdCalendarEvent *event,
                      gboolean              *out_created,
                      GError               **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(event != NULL && event->uid != NULL, FALSE);

  sqlite3 *h = nd_store_db_get_handle(store->db);
  g_autofree gchar *ical = nd_ical_generate_vevent(event);
  g_autofree gchar *etag = nd_ical_compute_etag(event);
  gboolean existed = FALSE;

  if (!nd_store_db_begin(store->db, error))
    return FALSE;

  if (!nd_store_db_row_exists(store->db, ND_STORE_COLLECTION_EVENTS,
                              event->uid, &existed, error))
    goto fail;

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h,
        "INSERT INTO events (uid, ical, etag, kind, pubkey, created_at, updated_at)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"
        " ON CONFLICT(uid) DO UPDATE SET"
        "   ical = excluded.ical, etag = excluded.etag, kind = excluded.kind,"
        "   pubkey = excluded.pubkey, created_at = excluded.created_at,"
        "   updated_at = excluded.updated_at",
        -1, &stmt, NULL) != SQLITE_OK) {
    nd_store_db_set_sql_error(store->db, error, "prepare event upsert");
    goto fail;
  }

  sqlite3_bind_text(stmt, 1, event->uid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, ical, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, etag, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 4, event->kind);
  if (event->pubkey)
    sqlite3_bind_text(stmt, 5, event->pubkey, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(stmt, 5);
  sqlite3_bind_int64(stmt, 6, event->created_at);
  sqlite3_bind_int64(stmt, 7, g_get_real_time() / G_USEC_PER_SEC);

  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE)
    nd_store_db_set_sql_error(store->db, error, "event upsert");
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE)
    goto fail;

  if (!nd_store_db_bump_generation(store->db, ND_STORE_COLLECTION_EVENTS, error))
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

NdCalendarEvent *
nd_calendar_store_get(NdCalendarStore *store,
                      const gchar     *uid,
                      GError         **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  g_return_val_if_fail(uid != NULL, NULL);

  sqlite3 *h = nd_store_db_get_handle(store->db);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, "SELECT " EVENT_COLUMNS " FROM events WHERE uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK) {
    nd_store_db_set_sql_error(store->db, error, "prepare event lookup");
    return NULL;
  }
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_TRANSIENT);

  NdCalendarEvent *event = NULL;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    event = event_from_row(stmt, error);
  else if (rc != SQLITE_DONE)
    nd_store_db_set_sql_error(store->db, error, "event lookup");

  sqlite3_finalize(stmt);
  return event;
}

gboolean
nd_calendar_store_remove(NdCalendarStore *store,
                         const gchar     *uid,
                         gboolean        *out_removed,
                         GError         **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(uid != NULL, FALSE);
  return nd_store_db_delete_row(store->db, ND_STORE_COLLECTION_EVENTS,
                                uid, out_removed, error);
}

GPtrArray *
nd_calendar_store_list_all(NdCalendarStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);

  sqlite3 *h = nd_store_db_get_handle(store->db);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, "SELECT " EVENT_COLUMNS " FROM events ORDER BY uid",
                         -1, &stmt, NULL) != SQLITE_OK) {
    nd_store_db_set_sql_error(store->db, error, "prepare event list");
    return NULL;
  }

  GPtrArray *list =
    g_ptr_array_new_with_free_func((GDestroyNotify)nd_calendar_event_free);
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    NdCalendarEvent *event = event_from_row(stmt, error);
    if (event == NULL) {
      g_clear_pointer(&list, g_ptr_array_unref);
      break;
    }
    g_ptr_array_add(list, event);
  }
  if (list != NULL && rc != SQLITE_DONE) {
    nd_store_db_set_sql_error(store->db, error, "event list");
    g_clear_pointer(&list, g_ptr_array_unref);
  }

  sqlite3_finalize(stmt);
  return list;
}

gboolean
nd_calendar_store_count(NdCalendarStore *store,
                        guint           *out_count,
                        GError         **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  return nd_store_db_count_rows(store->db, ND_STORE_COLLECTION_EVENTS,
                                out_count, error);
}

gchar *
nd_calendar_store_get_ctag(NdCalendarStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  return nd_store_db_get_ctag(store->db, ND_STORE_COLLECTION_EVENTS, error);
}
