/* SPDX-License-Identifier: MIT
 *
 * store_leases.c - Credential lease operations.
 */

#include "signet/store_leases.h"
#include "signet/store.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <sqlite3.h>

int signet_store_issue_lease(SignetStore *store,
                             const char *lease_id,
                             const char *secret_id,
                             const char *agent_id,
                             int64_t issued_at,
                             int64_t expires_at,
                             const char *metadata_json) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !lease_id || !secret_id || !agent_id) return -1;

  const char *sql =
    "INSERT INTO leases (lease_id, secret_id, agent_id, issued_at, expires_at, metadata) "
    "VALUES (?, ?, ?, ?, ?, ?);";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_text(stmt, 1, lease_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, secret_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, agent_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, issued_at);
  sqlite3_bind_int64(stmt, 5, expires_at);
  if (metadata_json && metadata_json[0]) {
    sqlite3_bind_text(stmt, 6, metadata_json, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 6);
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int signet_store_revoke_lease(SignetStore *store,
                              const char *lease_id,
                              int64_t now) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !lease_id) return -1;

  const char *sql =
    "UPDATE leases SET revoked_at = ? WHERE lease_id = ? AND revoked_at IS NULL;";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_int64(stmt, 1, now);
  sqlite3_bind_text(stmt, 2, lease_id, -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  int changes = sqlite3_changes(db);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) return -1;
  return (changes > 0) ? 0 : 1;
}

int signet_store_revoke_agent_leases(SignetStore *store,
                                     const char *agent_id,
                                     int64_t now) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !agent_id) return -1;

  const char *sql =
    "UPDATE leases SET revoked_at = ? WHERE agent_id = ? AND revoked_at IS NULL;";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_int64(stmt, 1, now);
  sqlite3_bind_text(stmt, 2, agent_id, -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  int changes = sqlite3_changes(db);
  sqlite3_finalize(stmt);

  return (rc == SQLITE_DONE) ? changes : -1;
}

int signet_store_list_active_leases(SignetStore *store,
                                    const char *agent_id,
                                    int64_t now,
                                    SignetLeaseRecord **out_leases,
                                    size_t *out_count) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !agent_id || !out_leases || !out_count) return -1;

  *out_leases = NULL;
  *out_count = 0;

  const char *sql =
    "SELECT lease_id, secret_id, agent_id, issued_at, expires_at, metadata "
    "FROM leases WHERE agent_id = ? AND revoked_at IS NULL AND expires_at > ? "
    "ORDER BY issued_at;";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_text(stmt, 1, agent_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, now);

  GArray *arr = g_array_new(FALSE, TRUE, sizeof(SignetLeaseRecord));

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    SignetLeaseRecord rec = {0};
    rec.lease_id = g_strdup((const char *)sqlite3_column_text(stmt, 0));
    rec.secret_id = g_strdup((const char *)sqlite3_column_text(stmt, 1));
    rec.agent_id = g_strdup((const char *)sqlite3_column_text(stmt, 2));
    rec.issued_at = sqlite3_column_int64(stmt, 3);
    rec.expires_at = sqlite3_column_int64(stmt, 4);
    const char *meta = (const char *)sqlite3_column_text(stmt, 5);
    rec.metadata = meta ? g_strdup(meta) : NULL;
    g_array_append_val(arr, rec);
  }
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    for (guint i = 0; i < arr->len; i++)
      signet_lease_record_clear(&g_array_index(arr, SignetLeaseRecord, i));
    g_array_free(arr, TRUE);
    return -1;
  }

  *out_count = arr->len;
  *out_leases = (SignetLeaseRecord *)g_array_free(arr, FALSE);
  return 0;
}

int signet_store_count_active_leases(SignetStore *store, int64_t now) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db) return -1;

  const char *sql =
    "SELECT COUNT(*) FROM leases WHERE revoked_at IS NULL AND expires_at > ?;";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_int64(stmt, 1, now);
  rc = sqlite3_step(stmt);
  int count = (rc == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  return count;
}

int signet_store_cleanup_expired_leases(SignetStore *store, int64_t cutoff) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db) return -1;

  const char *sql = "DELETE FROM leases WHERE expires_at < ?;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_int64(stmt, 1, cutoff);
  rc = sqlite3_step(stmt);
  int changes = sqlite3_changes(db);
  sqlite3_finalize(stmt);

  return (rc == SQLITE_DONE) ? changes : -1;
}

void signet_lease_record_clear(SignetLeaseRecord *rec) {
  if (!rec) return;
  g_free(rec->lease_id);
  g_free(rec->secret_id);
  g_free(rec->agent_id);
  g_free(rec->metadata);
  memset(rec, 0, sizeof(*rec));
}

void signet_lease_list_free(SignetLeaseRecord *leases, size_t count) {
  if (!leases) return;
  for (size_t i = 0; i < count; i++)
    signet_lease_record_clear(&leases[i]);
  g_free(leases);
}
