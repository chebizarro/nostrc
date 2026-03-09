/* SPDX-License-Identifier: MIT
 *
 * store_tokens.c - Bootstrap token store operations.
 */

#include "signet/store_tokens.h"
#include "signet/store.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <sqlite3.h>

#define SIGNET_TOKEN_MAX_ATTEMPTS 3

int signet_store_put_bootstrap_token(SignetStore *store,
                                     const char *token_hash,
                                     const char *agent_id,
                                     const char *bootstrap_pubkey,
                                     int64_t issued_at,
                                     int64_t expires_at) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !token_hash || !agent_id || !bootstrap_pubkey) return -1;

  const char *sql =
    "INSERT INTO bootstrap_tokens (token_hash, agent_id, bootstrap_pubkey, issued_at, expires_at, attempt_count) "
    "VALUES (?, ?, ?, ?, ?, 0);";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_text(stmt, 1, token_hash, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, agent_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, bootstrap_pubkey, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, issued_at);
  sqlite3_bind_int64(stmt, 5, expires_at);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
}

SignetTokenResult signet_store_verify_bootstrap_token(SignetStore *store,
                                                      const char *token_hash,
                                                      const char *agent_id,
                                                      const char *bootstrap_pubkey,
                                                      int64_t now) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !token_hash || !agent_id || !bootstrap_pubkey)
    return SIGNET_TOKEN_ERROR;

  /* Increment attempt_count first (always, even if check fails). */
  const char *inc_sql =
    "UPDATE bootstrap_tokens SET attempt_count = attempt_count + 1 WHERE token_hash = ?;";
  sqlite3_stmt *inc_stmt = NULL;
  if (sqlite3_prepare_v2(db, inc_sql, -1, &inc_stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(inc_stmt, 1, token_hash, -1, SQLITE_TRANSIENT);
    sqlite3_step(inc_stmt);
    sqlite3_finalize(inc_stmt);
  }

  /* Fetch the token record. */
  const char *sql =
    "SELECT agent_id, bootstrap_pubkey, issued_at, expires_at, used_at, attempt_count "
    "FROM bootstrap_tokens WHERE token_hash = ?;";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return SIGNET_TOKEN_ERROR;

  sqlite3_bind_text(stmt, 1, token_hash, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);

  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return SIGNET_TOKEN_NOT_FOUND;
  }

  const char *db_agent = (const char *)sqlite3_column_text(stmt, 0);
  const char *db_pubkey = (const char *)sqlite3_column_text(stmt, 1);
  int64_t expires_at = sqlite3_column_int64(stmt, 3);
  int used_at_null = (sqlite3_column_type(stmt, 4) == SQLITE_NULL);
  int attempt_count = sqlite3_column_int(stmt, 5);

  SignetTokenResult result = SIGNET_TOKEN_OK;

  if (!used_at_null) {
    result = SIGNET_TOKEN_ALREADY_USED;
  } else if (attempt_count > SIGNET_TOKEN_MAX_ATTEMPTS) {
    result = SIGNET_TOKEN_MAX_ATTEMPTS;
  } else if (now > expires_at) {
    result = SIGNET_TOKEN_EXPIRED;
  } else if (!db_agent || g_ascii_strcasecmp(db_agent, agent_id) != 0) {
    result = SIGNET_TOKEN_AGENT_MISMATCH;
  } else if (!db_pubkey || g_ascii_strcasecmp(db_pubkey, bootstrap_pubkey) != 0) {
    result = SIGNET_TOKEN_PUBKEY_MISMATCH;
  }

  sqlite3_finalize(stmt);
  return result;
}

int signet_store_consume_bootstrap_token(SignetStore *store,
                                         const char *token_hash,
                                         int64_t now) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !token_hash) return -1;

  const char *sql =
    "UPDATE bootstrap_tokens SET used_at = ? WHERE token_hash = ? AND used_at IS NULL;";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_int64(stmt, 1, now);
  sqlite3_bind_text(stmt, 2, token_hash, -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  int changes = sqlite3_changes(db);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) return -1;
  return (changes > 0) ? 0 : -1;
}

int signet_store_cleanup_expired_tokens(SignetStore *store, int64_t cutoff) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db) return -1;

  const char *sql = "DELETE FROM bootstrap_tokens WHERE expires_at < ?;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_int64(stmt, 1, cutoff);
  rc = sqlite3_step(stmt);
  int changes = sqlite3_changes(db);
  sqlite3_finalize(stmt);

  return (rc == SQLITE_DONE) ? changes : -1;
}

void signet_bootstrap_token_clear(SignetBootstrapToken *tok) {
  if (!tok) return;
  g_free(tok->token_hash);
  g_free(tok->agent_id);
  g_free(tok->bootstrap_pubkey);
  memset(tok, 0, sizeof(*tok));
}
