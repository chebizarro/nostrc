/* nd-contact-store.c - Contact store for CardDAV (SQLite-backed)
 *
 * SPDX-License-Identifier: MIT
 *
 * The vCard text is the stored representation (nd_vcard_generate returns
 * the original text when available, so round-trips are lossless);
 * pubkey/npub/created_at live in their own columns.
 */

#include "nd-contact-store.h"

#include <sqlite3.h>

struct _NdContactStore {
  NdStoreDb *db;
};

#define CONTACT_COLUMNS "uid, vcard, pubkey, npub, created_at"

static NdContact *
contact_from_row(sqlite3_stmt *stmt, GError **error)
{
  const gchar *uid   = (const gchar *)sqlite3_column_text(stmt, 0);
  const gchar *vcard = (const gchar *)sqlite3_column_text(stmt, 1);

  GError *parse_err = NULL;
  NdContact *contact = nd_vcard_parse(vcard ? vcard : "", &parse_err);
  if (contact == NULL) {
    g_set_error(error, ND_STORE_DB_ERROR, ND_STORE_DB_ERROR_DATA,
                "Stored contact %s is unreadable: %s",
                uid ? uid : "(null)",
                parse_err ? parse_err->message : "parse failed");
    g_clear_error(&parse_err);
    return NULL;
  }

  g_free(contact->uid);
  contact->uid = g_strdup(uid);
  g_free(contact->pubkey);
  contact->pubkey = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
  g_free(contact->npub);
  contact->npub = g_strdup((const gchar *)sqlite3_column_text(stmt, 3));
  contact->created_at = sqlite3_column_int64(stmt, 4);
  return contact;
}

static void
bind_text_or_null(sqlite3_stmt *stmt, int idx, const gchar *value)
{
  if (value)
    sqlite3_bind_text(stmt, idx, value, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(stmt, idx);
}

NdContactStore *
nd_contact_store_new(NdStoreDb *db)
{
  g_return_val_if_fail(db != NULL, NULL);
  NdContactStore *store = g_new0(NdContactStore, 1);
  store->db = nd_store_db_ref(db);
  return store;
}

void
nd_contact_store_free(NdContactStore *store)
{
  if (store == NULL) return;
  nd_store_db_unref(store->db);
  g_free(store);
}

gboolean
nd_contact_store_put(NdContactStore  *store,
                     const NdContact *contact,
                     gboolean        *out_created,
                     GError         **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(contact != NULL && contact->uid != NULL, FALSE);

  sqlite3 *h = nd_store_db_get_handle(store->db);
  g_autofree gchar *vcard = nd_vcard_generate(contact);
  g_autofree gchar *etag = nd_vcard_compute_etag(contact);
  gboolean existed = FALSE;

  if (!nd_store_db_begin(store->db, error))
    return FALSE;

  if (!nd_store_db_row_exists(store->db, ND_STORE_COLLECTION_CONTACTS,
                              contact->uid, &existed, error))
    goto fail;

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h,
        "INSERT INTO contacts (uid, vcard, etag, pubkey, npub, created_at, updated_at)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"
        " ON CONFLICT(uid) DO UPDATE SET"
        "   vcard = excluded.vcard, etag = excluded.etag,"
        "   pubkey = excluded.pubkey, npub = excluded.npub,"
        "   created_at = excluded.created_at, updated_at = excluded.updated_at",
        -1, &stmt, NULL) != SQLITE_OK) {
    nd_store_db_set_sql_error(store->db, error, "prepare contact upsert");
    goto fail;
  }

  sqlite3_bind_text(stmt, 1, contact->uid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, vcard, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, etag, -1, SQLITE_TRANSIENT);
  bind_text_or_null(stmt, 4, contact->pubkey);
  bind_text_or_null(stmt, 5, contact->npub);
  sqlite3_bind_int64(stmt, 6, contact->created_at);
  sqlite3_bind_int64(stmt, 7, g_get_real_time() / G_USEC_PER_SEC);

  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE)
    nd_store_db_set_sql_error(store->db, error, "contact upsert");
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE)
    goto fail;

  if (!nd_store_db_bump_generation(store->db, ND_STORE_COLLECTION_CONTACTS, error))
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

NdContact *
nd_contact_store_get(NdContactStore *store,
                     const gchar    *uid,
                     GError        **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  g_return_val_if_fail(uid != NULL, NULL);

  sqlite3 *h = nd_store_db_get_handle(store->db);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, "SELECT " CONTACT_COLUMNS " FROM contacts WHERE uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK) {
    nd_store_db_set_sql_error(store->db, error, "prepare contact lookup");
    return NULL;
  }
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_TRANSIENT);

  NdContact *contact = NULL;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    contact = contact_from_row(stmt, error);
  else if (rc != SQLITE_DONE)
    nd_store_db_set_sql_error(store->db, error, "contact lookup");

  sqlite3_finalize(stmt);
  return contact;
}

gboolean
nd_contact_store_remove(NdContactStore *store,
                        const gchar    *uid,
                        gboolean       *out_removed,
                        GError        **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(uid != NULL, FALSE);
  return nd_store_db_delete_row(store->db, ND_STORE_COLLECTION_CONTACTS,
                                uid, out_removed, error);
}

GPtrArray *
nd_contact_store_list_all(NdContactStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);

  sqlite3 *h = nd_store_db_get_handle(store->db);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(h, "SELECT " CONTACT_COLUMNS " FROM contacts ORDER BY uid",
                         -1, &stmt, NULL) != SQLITE_OK) {
    nd_store_db_set_sql_error(store->db, error, "prepare contact list");
    return NULL;
  }

  GPtrArray *list =
    g_ptr_array_new_with_free_func((GDestroyNotify)nd_contact_free);
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    NdContact *contact = contact_from_row(stmt, error);
    if (contact == NULL) {
      g_clear_pointer(&list, g_ptr_array_unref);
      break;
    }
    g_ptr_array_add(list, contact);
  }
  if (list != NULL && rc != SQLITE_DONE) {
    nd_store_db_set_sql_error(store->db, error, "contact list");
    g_clear_pointer(&list, g_ptr_array_unref);
  }

  sqlite3_finalize(stmt);
  return list;
}

gboolean
nd_contact_store_count(NdContactStore *store,
                       guint          *out_count,
                       GError        **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  return nd_store_db_count_rows(store->db, ND_STORE_COLLECTION_CONTACTS,
                                out_count, error);
}

gchar *
nd_contact_store_get_ctag(NdContactStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  return nd_store_db_get_ctag(store->db, ND_STORE_COLLECTION_CONTACTS, error);
}
