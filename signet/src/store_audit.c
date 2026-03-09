/* SPDX-License-Identifier: MIT
 *
 * store_audit.c - Hash-chained audit log operations.
 */

#include "signet/store_audit.h"
#include "signet/store.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <glib.h>
#include <sqlite3.h>
#include <sodium.h>

/* Compute SHA256 hash of concatenated fields: ts || agent_id || operation || detail || prev_hash.
 * Returns heap-allocated hex string (caller frees with g_free). */
static char *signet_audit_compute_hash(int64_t ts,
                                       const char *agent_id,
                                       const char *operation,
                                       const char *detail,
                                       const char *prev_hash) {
  /* Build canonical input string. */
  GString *input = g_string_new(NULL);
  g_string_append_printf(input, "%lld", (long long)ts);
  g_string_append(input, agent_id ? agent_id : "");
  g_string_append(input, operation ? operation : "");
  g_string_append(input, detail ? detail : "");
  g_string_append(input, prev_hash ? prev_hash : "");

  /* SHA-256 via libsodium crypto_hash_sha256. */
  uint8_t hash[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(hash, (const uint8_t *)input->str, input->len);
  g_string_free(input, TRUE);

  /* Hex encode. */
  char *hex = (char *)g_malloc(crypto_hash_sha256_BYTES * 2 + 1);
  for (size_t i = 0; i < crypto_hash_sha256_BYTES; i++) {
    sprintf(hex + i * 2, "%02x", hash[i]);
  }
  hex[crypto_hash_sha256_BYTES * 2] = '\0';
  return hex;
}

int signet_audit_log_append(SignetStore *store,
                            int64_t ts,
                            const char *agent_id,
                            const char *operation,
                            const char *secret_id,
                            const char *transport,
                            const char *detail_json) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !agent_id || !operation) return -1;

  /* Get the hash of the previous entry (chain link). */
  const char *prev_sql = "SELECT entry_hash FROM audit_log ORDER BY id DESC LIMIT 1;";
  sqlite3_stmt *ps = NULL;
  char *prev_hash = NULL;
  if (sqlite3_prepare_v2(db, prev_sql, -1, &ps, NULL) == SQLITE_OK) {
    if (sqlite3_step(ps) == SQLITE_ROW) {
      const char *ph = (const char *)sqlite3_column_text(ps, 0);
      prev_hash = ph ? g_strdup(ph) : g_strdup("genesis");
    } else {
      prev_hash = g_strdup("genesis"); /* first entry */
    }
    sqlite3_finalize(ps);
  } else {
    prev_hash = g_strdup("genesis");
  }

  /* Compute entry hash. */
  char *entry_hash = signet_audit_compute_hash(ts, agent_id, operation,
                                                detail_json, prev_hash);

  /* Insert. */
  const char *sql =
    "INSERT INTO audit_log (ts, agent_id, operation, secret_id, transport, detail, prev_hash, entry_hash) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    g_free(prev_hash);
    g_free(entry_hash);
    return -1;
  }

  sqlite3_bind_int64(stmt, 1, ts);
  sqlite3_bind_text(stmt, 2, agent_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, operation, -1, SQLITE_TRANSIENT);
  if (secret_id && secret_id[0]) {
    sqlite3_bind_text(stmt, 4, secret_id, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 4);
  }
  if (transport && transport[0]) {
    sqlite3_bind_text(stmt, 5, transport, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 5);
  }
  if (detail_json && detail_json[0]) {
    sqlite3_bind_text(stmt, 6, detail_json, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 6);
  }
  sqlite3_bind_text(stmt, 7, prev_hash, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, entry_hash, -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  g_free(prev_hash);
  g_free(entry_hash);

  return (rc == SQLITE_DONE) ? 0 : -1;
}

int signet_audit_verify_chain(SignetStore *store,
                              int64_t from_id,
                              int64_t to_id,
                              int64_t *out_broken_id) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db) return -1;
  if (out_broken_id) *out_broken_id = 0;

  const char *sql;
  if (to_id > 0) {
    sql = "SELECT id, ts, agent_id, operation, detail, prev_hash, entry_hash "
          "FROM audit_log WHERE id >= ? AND id <= ? ORDER BY id;";
  } else {
    sql = "SELECT id, ts, agent_id, operation, detail, prev_hash, entry_hash "
          "FROM audit_log WHERE id >= ? ORDER BY id;";
  }

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_int64(stmt, 1, from_id > 0 ? from_id : 1);
  if (to_id > 0) {
    sqlite3_bind_int64(stmt, 2, to_id);
  }

  char *expected_prev = NULL;

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    int64_t id = sqlite3_column_int64(stmt, 0);
    int64_t ts = sqlite3_column_int64(stmt, 1);
    const char *agent_id = (const char *)sqlite3_column_text(stmt, 2);
    const char *operation = (const char *)sqlite3_column_text(stmt, 3);
    const char *detail = (const char *)sqlite3_column_text(stmt, 4);
    const char *prev_hash = (const char *)sqlite3_column_text(stmt, 5);
    const char *entry_hash = (const char *)sqlite3_column_text(stmt, 6);

    /* Verify prev_hash chain continuity. */
    if (expected_prev && prev_hash) {
      if (strcmp(expected_prev, prev_hash) != 0) {
        if (out_broken_id) *out_broken_id = id;
        g_free(expected_prev);
        sqlite3_finalize(stmt);
        return 1; /* chain broken */
      }
    }

    /* Verify entry_hash. */
    char *computed = signet_audit_compute_hash(ts, agent_id, operation,
                                               detail, prev_hash);
    bool hash_ok = (entry_hash && computed && strcmp(entry_hash, computed) == 0);
    g_free(computed);

    if (!hash_ok) {
      if (out_broken_id) *out_broken_id = id;
      g_free(expected_prev);
      sqlite3_finalize(stmt);
      return 1; /* hash mismatch */
    }

    g_free(expected_prev);
    expected_prev = entry_hash ? g_strdup(entry_hash) : NULL;
  }

  g_free(expected_prev);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
}

int64_t signet_audit_log_count(SignetStore *store) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db) return -1;

  const char *sql = "SELECT COUNT(*) FROM audit_log;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  rc = sqlite3_step(stmt);
  int64_t count = (rc == SQLITE_ROW) ? sqlite3_column_int64(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  return count;
}

void signet_audit_entry_clear(SignetAuditEntry *entry) {
  if (!entry) return;
  g_free(entry->agent_id);
  g_free(entry->operation);
  g_free(entry->secret_id);
  g_free(entry->transport);
  g_free(entry->detail);
  g_free(entry->prev_hash);
  g_free(entry->entry_hash);
  memset(entry, 0, sizeof(*entry));
}
