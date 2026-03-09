/* SPDX-License-Identifier: MIT
 *
 * store_secrets.c - Extended secret storage operations.
 */

#include "signet/store_secrets.h"
#include "signet/store.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <sqlite3.h>
#include <sodium.h>

#define SIGNET_NONCE_LEN  crypto_secretbox_NONCEBYTES
#define SIGNET_MAC_LEN    crypto_secretbox_MACBYTES

const char *signet_secret_type_to_string(SignetSecretType t) {
  switch (t) {
    case SIGNET_SECRET_NOSTR_NSEC:   return "nostr_nsec";
    case SIGNET_SECRET_SSH_KEY:      return "ssh_key";
    case SIGNET_SECRET_API_TOKEN:    return "api_token";
    case SIGNET_SECRET_CREDENTIAL:   return "credential";
    case SIGNET_SECRET_CERTIFICATE:  return "certificate";
    default:                         return "unknown";
  }
}

SignetSecretType signet_secret_type_from_string(const char *s) {
  if (!s) return SIGNET_SECRET_CREDENTIAL;
  if (strcmp(s, "nostr_nsec") == 0)  return SIGNET_SECRET_NOSTR_NSEC;
  if (strcmp(s, "ssh_key") == 0)     return SIGNET_SECRET_SSH_KEY;
  if (strcmp(s, "api_token") == 0)   return SIGNET_SECRET_API_TOKEN;
  if (strcmp(s, "credential") == 0)  return SIGNET_SECRET_CREDENTIAL;
  if (strcmp(s, "certificate") == 0) return SIGNET_SECRET_CERTIFICATE;
  return SIGNET_SECRET_CREDENTIAL;
}

int signet_store_put_secret(SignetStore *store,
                            const char *id,
                            const char *agent_id,
                            const char *agent_pubkey,
                            SignetSecretType secret_type,
                            const char *label,
                            const uint8_t *payload,
                            size_t payload_len,
                            const char *policy_id,
                            int64_t now) {
  sqlite3 *db = signet_store_get_db(store);
  const uint8_t *dek = signet_store_get_dek(store);
  if (!db || !dek || !id || !agent_id || !agent_pubkey || !payload) return -1;

  /* Encrypt payload with DEK. */
  uint8_t nonce[SIGNET_NONCE_LEN];
  randombytes_buf(nonce, sizeof(nonce));

  size_t ct_len = payload_len + SIGNET_MAC_LEN;
  uint8_t *ciphertext = (uint8_t *)malloc(ct_len);
  if (!ciphertext) return -1;

  if (crypto_secretbox_easy(ciphertext, payload, payload_len, nonce, dek) != 0) {
    free(ciphertext);
    return -1;
  }

  const char *sql =
    "INSERT OR REPLACE INTO secrets "
    "(id, agent_id, agent_pubkey, secret_type, label, payload, nonce, "
    " policy_id, created_at, version, active_version) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 1, 1);";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) { free(ciphertext); return -1; }

  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, agent_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, agent_pubkey, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, signet_secret_type_to_string(secret_type), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 5, label, -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 6, ciphertext, (int)ct_len, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 7, nonce, SIGNET_NONCE_LEN, SQLITE_TRANSIENT);
  if (policy_id && policy_id[0]) {
    sqlite3_bind_text(stmt, 8, policy_id, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 8);
  }
  sqlite3_bind_int64(stmt, 9, now);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  free(ciphertext);

  return (rc == SQLITE_DONE) ? 0 : -1;
}

int signet_store_get_secret(SignetStore *store,
                            const char *id,
                            SignetSecretRecord *out_record) {
  sqlite3 *db = signet_store_get_db(store);
  const uint8_t *dek = signet_store_get_dek(store);
  if (!db || !dek || !id || !out_record) return -1;
  memset(out_record, 0, sizeof(*out_record));

  const char *sql =
    "SELECT agent_id, agent_pubkey, secret_type, label, payload, nonce, "
    "policy_id, created_at, rotated_at, expires_at, version, active_version "
    "FROM secrets WHERE id = ?;";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);

  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 1 : -1;
  }

  const uint8_t *ct = (const uint8_t *)sqlite3_column_blob(stmt, 4);
  int ct_len = sqlite3_column_bytes(stmt, 4);
  const uint8_t *nonce = (const uint8_t *)sqlite3_column_blob(stmt, 5);
  int nonce_len = sqlite3_column_bytes(stmt, 5);

  if (!ct || ct_len < (int)SIGNET_MAC_LEN || !nonce || nonce_len != SIGNET_NONCE_LEN) {
    sqlite3_finalize(stmt);
    return -1;
  }

  size_t pt_len = (size_t)ct_len - SIGNET_MAC_LEN;
  uint8_t *plaintext = (uint8_t *)sodium_malloc(pt_len);
  if (!plaintext) { sqlite3_finalize(stmt); return -1; }

  if (crypto_secretbox_open_easy(plaintext, ct, (size_t)ct_len, nonce, dek) != 0) {
    sodium_free(plaintext);
    sqlite3_finalize(stmt);
    return -1;
  }

  const char *type_str = (const char *)sqlite3_column_text(stmt, 2);

  out_record->id = g_strdup(id);
  out_record->agent_id = g_strdup((const char *)sqlite3_column_text(stmt, 0));
  out_record->agent_pubkey = g_strdup((const char *)sqlite3_column_text(stmt, 1));
  out_record->secret_type = signet_secret_type_from_string(type_str);
  out_record->label = g_strdup((const char *)sqlite3_column_text(stmt, 3));
  out_record->payload = plaintext;
  out_record->payload_len = pt_len;
  out_record->policy_id = g_strdup((const char *)sqlite3_column_text(stmt, 6));
  out_record->created_at = sqlite3_column_int64(stmt, 7);
  out_record->rotated_at = sqlite3_column_int64(stmt, 8);
  out_record->expires_at = sqlite3_column_int64(stmt, 9);
  out_record->version = sqlite3_column_int(stmt, 10);
  out_record->active_version = sqlite3_column_int(stmt, 11);

  sqlite3_finalize(stmt);
  return 0;
}

int signet_store_delete_secret(SignetStore *store, const char *id) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !id) return -1;

  /* Delete versions first (foreign key-like cleanup). */
  const char *ver_sql = "DELETE FROM secret_versions WHERE id = ?;";
  sqlite3_stmt *vs = NULL;
  if (sqlite3_prepare_v2(db, ver_sql, -1, &vs, NULL) == SQLITE_OK) {
    sqlite3_bind_text(vs, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_step(vs);
    sqlite3_finalize(vs);
  }

  const char *sql = "DELETE FROM secrets WHERE id = ?;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  int changes = sqlite3_changes(db);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) return -1;
  return (changes > 0) ? 0 : 1;
}

int signet_store_list_secrets(SignetStore *store,
                              const char *agent_id,
                              char ***out_ids,
                              char ***out_labels,
                              size_t *out_count) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !agent_id || !out_ids || !out_labels || !out_count) return -1;

  *out_ids = NULL;
  *out_labels = NULL;
  *out_count = 0;

  const char *sql = "SELECT id, label FROM secrets WHERE agent_id = ? ORDER BY created_at;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_text(stmt, 1, agent_id, -1, SQLITE_TRANSIENT);

  GPtrArray *ids = g_ptr_array_new();
  GPtrArray *labels = g_ptr_array_new();

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    g_ptr_array_add(ids, g_strdup((const char *)sqlite3_column_text(stmt, 0)));
    g_ptr_array_add(labels, g_strdup((const char *)sqlite3_column_text(stmt, 1)));
  }
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    for (guint i = 0; i < ids->len; i++) { g_free(ids->pdata[i]); g_free(labels->pdata[i]); }
    g_ptr_array_free(ids, TRUE);
    g_ptr_array_free(labels, TRUE);
    return -1;
  }

  size_t count = ids->len;
  *out_ids = (char **)g_new0(char *, count + 1);
  *out_labels = (char **)g_new0(char *, count + 1);
  for (size_t i = 0; i < count; i++) {
    (*out_ids)[i] = (char *)ids->pdata[i];
    (*out_labels)[i] = (char *)labels->pdata[i];
  }
  *out_count = count;

  g_ptr_array_free(ids, TRUE);
  g_ptr_array_free(labels, TRUE);
  return 0;
}

int signet_store_rotate_secret(SignetStore *store,
                               const char *id,
                               const uint8_t *new_payload,
                               size_t new_payload_len,
                               int64_t now) {
  sqlite3 *db = signet_store_get_db(store);
  const uint8_t *dek = signet_store_get_dek(store);
  if (!db || !dek || !id || !new_payload) return -1;

  /* Get current version and payload. */
  const char *ver_sql = "SELECT active_version, payload, nonce FROM secrets WHERE id = ?;";
  sqlite3_stmt *vs = NULL;
  int rc = sqlite3_prepare_v2(db, ver_sql, -1, &vs, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_text(vs, 1, id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(vs);
  if (rc != SQLITE_ROW) { sqlite3_finalize(vs); return -1; }

  int old_version = sqlite3_column_int(vs, 0);
  const void *old_payload = sqlite3_column_blob(vs, 1);
  int old_payload_len = sqlite3_column_bytes(vs, 1);
  const void *old_nonce = sqlite3_column_blob(vs, 2);
  int old_nonce_len = sqlite3_column_bytes(vs, 2);
  int new_version = old_version + 1;

  /* Archive old version. */
  const char *arch_sql =
    "INSERT INTO secret_versions (id, version, payload, nonce, created_at) VALUES (?, ?, ?, ?, ?);";
  sqlite3_stmt *as = NULL;
  if (sqlite3_prepare_v2(db, arch_sql, -1, &as, NULL) == SQLITE_OK) {
    sqlite3_bind_text(as, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(as, 2, old_version);
    sqlite3_bind_blob(as, 3, old_payload, old_payload_len, SQLITE_TRANSIENT);
    sqlite3_bind_blob(as, 4, old_nonce, old_nonce_len, SQLITE_TRANSIENT);
    sqlite3_bind_int64(as, 5, now);
    sqlite3_step(as);
    sqlite3_finalize(as);
  }
  sqlite3_finalize(vs);

  /* Encrypt new payload. */
  uint8_t nonce[SIGNET_NONCE_LEN];
  randombytes_buf(nonce, sizeof(nonce));

  size_t ct_len = new_payload_len + SIGNET_MAC_LEN;
  uint8_t *ciphertext = (uint8_t *)malloc(ct_len);
  if (!ciphertext) return -1;

  if (crypto_secretbox_easy(ciphertext, new_payload, new_payload_len, nonce, dek) != 0) {
    free(ciphertext);
    return -1;
  }

  /* Update secrets table. */
  const char *upd_sql =
    "UPDATE secrets SET payload = ?, nonce = ?, version = ?, active_version = ?, "
    "rotated_at = ? WHERE id = ?;";
  sqlite3_stmt *us = NULL;
  rc = sqlite3_prepare_v2(db, upd_sql, -1, &us, NULL);
  if (rc != SQLITE_OK) { free(ciphertext); return -1; }

  sqlite3_bind_blob(us, 1, ciphertext, (int)ct_len, SQLITE_TRANSIENT);
  sqlite3_bind_blob(us, 2, nonce, SIGNET_NONCE_LEN, SQLITE_TRANSIENT);
  sqlite3_bind_int(us, 3, new_version);
  sqlite3_bind_int(us, 4, new_version);
  sqlite3_bind_int64(us, 5, now);
  sqlite3_bind_text(us, 6, id, -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(us);
  sqlite3_finalize(us);
  free(ciphertext);

  return (rc == SQLITE_DONE) ? 0 : -1;
}

void signet_secret_record_clear(SignetSecretRecord *rec) {
  if (!rec) return;
  g_free(rec->id);
  g_free(rec->agent_id);
  g_free(rec->agent_pubkey);
  g_free(rec->label);
  g_free(rec->policy_id);
  if (rec->payload) {
    sodium_free(rec->payload);
    rec->payload = NULL;
  }
  rec->payload_len = 0;
  memset(rec, 0, sizeof(*rec));
}

void signet_store_free_secret_list(char **ids, char **labels, size_t count) {
  if (ids) {
    for (size_t i = 0; i < count; i++) g_free(ids[i]);
    g_free(ids);
  }
  if (labels) {
    for (size_t i = 0; i < count; i++) g_free(labels[i]);
    g_free(labels);
  }
}
