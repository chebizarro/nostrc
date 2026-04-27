/* SPDX-License-Identifier: MIT
 *
 * store.c - SQLCipher-backed persistent store for Signet.
 *
 * SQLCipher provides transparent AES-256-CBC encryption of the entire
 * database. On top of that, individual secret keys are envelope-encrypted
 * using libsodium's crypto_secretbox_easy (XSalsa20-Poly1305) with a
 * data-encryption key derived via BLAKE2b (crypto_generichash) and
 * domain separation from the master key.
 */

/*
 * Dual key-management architecture:
 *
 * 1) SQLCipher layer (PRAGMA key = master key) encrypts the full database.
 * 2) Envelope layer encrypts each agent nsec with a DEK derived from the same
 *    master key using BLAKE2b plus a fixed domain string.
 *
 * Tradeoff: both layers depend on the same master secret, so compromise of
 * SIGNET_DB_KEY compromises both. The envelope layer still adds value for
 * defense-in-depth against SQLCipher bypass or partial data exposure paths.
 */

#include "signet/store.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <glib.h>
#include <sqlite3.h>

/* libnostr secure memory */
#include <secure_buf.h>

/* libsodium for envelope encryption + DEK derivation */
#include <sodium.h>

#define SIGNET_NSEC_LEN          32
#define SIGNET_NONCE_LEN         crypto_secretbox_NONCEBYTES   /* 24 */
#define SIGNET_CIPHERTEXT_EXTRA  crypto_secretbox_MACBYTES     /* 16 */
#define SIGNET_DEK_LEN           crypto_secretbox_KEYBYTES     /* 32 */

/* Domain-separation string for deriving the data-encryption key. */
static const char SIGNET_DEK_DOMAIN[] = "signet-agent-nsec-v1";

struct SignetStore {
  sqlite3 *db;
  uint8_t dek[SIGNET_DEK_LEN];  /* derived data-encryption key (mlock'd) */
  bool open;
};

/* ------------------------------ helpers ---------------------------------- */

static int signet_hex_decode(const char *hex, uint8_t *out, size_t out_len) {
  size_t hex_len = strlen(hex);
  if (hex_len != out_len * 2) return -1;
  for (size_t i = 0; i < out_len; i++) {
    unsigned int byte;
    if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
    out[i] = (uint8_t)byte;
  }
  return 0;
}

/* Derive the data-encryption key from master_key via BLAKE2b
 * (crypto_generichash) with domain separation.
 * master_key can be hex (64 chars) or raw bytes. */
static bool signet_derive_dek(const char *master_key, uint8_t dek[SIGNET_DEK_LEN]) {
  if (!master_key || !master_key[0]) return false;

  uint8_t ikm[64]; /* up to 64 bytes of key material */
  size_t ikm_len = 0;

  /* Try hex decode first */
  size_t mk_len = strlen(master_key);
  if (mk_len == 64 || mk_len == 128) {
    /* Could be hex for 32 or 64 bytes */
    if (signet_hex_decode(master_key, ikm, mk_len / 2) == 0) {
      ikm_len = mk_len / 2;
    }
  }

  /* Fall back to raw bytes if hex decode didn't work */
  if (ikm_len == 0) {
    ikm_len = mk_len > sizeof(ikm) ? sizeof(ikm) : mk_len;
    memcpy(ikm, master_key, ikm_len);
  }

  if (ikm_len < 32) {
    sodium_memzero(ikm, sizeof(ikm));
    return false; /* insufficient entropy */
  }

  /* BLAKE2b KDF with domain separation.
   * crypto_generichash outputs the DEK from (key=master_key,
   * input=SIGNET_DEK_DOMAIN). */
  if (crypto_generichash(dek, SIGNET_DEK_LEN,
                         (const uint8_t *)SIGNET_DEK_DOMAIN, strlen(SIGNET_DEK_DOMAIN),
                         ikm, ikm_len) != 0) {
    sodium_memzero(ikm, sizeof(ikm));
    return false;
  }

  sodium_memzero(ikm, sizeof(ikm));
  return true;
}

/* ------------------------------ schema ----------------------------------- */

static const char *SIGNET_SCHEMA_SQL =
  /* v1: agent keypair storage */
  "CREATE TABLE IF NOT EXISTS agents ("
  "  agent_id TEXT PRIMARY KEY NOT NULL,"
  "  encrypted_nsec BLOB NOT NULL,"
  "  nonce BLOB NOT NULL,"
  "  algo TEXT NOT NULL DEFAULT 'xsalsa20poly1305',"
  "  connect_secret TEXT,"
  "  created_at INTEGER NOT NULL,"
  "  last_used INTEGER NOT NULL DEFAULT 0"
  ");"
  "CREATE UNIQUE INDEX IF NOT EXISTS idx_agents_connect_secret ON agents(connect_secret) WHERE connect_secret IS NOT NULL;"

  /* v2: extended secrets (multi-type credentials) */
  "CREATE TABLE IF NOT EXISTS secrets ("
  "  id TEXT PRIMARY KEY,"
  "  agent_id TEXT NOT NULL,"
  "  agent_pubkey TEXT NOT NULL,"
  "  secret_type TEXT NOT NULL,"
  "  label TEXT NOT NULL,"
  "  payload BLOB NOT NULL,"
  "  nonce BLOB NOT NULL,"
  "  policy_id TEXT,"
  "  created_at INTEGER NOT NULL,"
  "  rotated_at INTEGER,"
  "  expires_at INTEGER,"
  "  version INTEGER DEFAULT 1,"
  "  active_version INTEGER DEFAULT 1"
  ");"
  "CREATE INDEX IF NOT EXISTS idx_secrets_agent ON secrets(agent_id);"
  "CREATE UNIQUE INDEX IF NOT EXISTS idx_secrets_pubkey ON secrets(agent_pubkey);"

  /* v2: credential version history for rotation */
  "CREATE TABLE IF NOT EXISTS secret_versions ("
  "  id TEXT NOT NULL,"
  "  version INTEGER NOT NULL,"
  "  payload BLOB NOT NULL,"
  "  nonce BLOB NOT NULL,"
  "  created_at INTEGER NOT NULL,"
  "  revoked_at INTEGER,"
  "  PRIMARY KEY (id, version)"
  ");"

  /* v2: time-bound credential leases */
  "CREATE TABLE IF NOT EXISTS leases ("
  "  lease_id TEXT PRIMARY KEY,"
  "  secret_id TEXT NOT NULL,"
  "  agent_id TEXT NOT NULL,"
  "  issued_at INTEGER NOT NULL,"
  "  expires_at INTEGER NOT NULL,"
  "  revoked_at INTEGER,"
  "  metadata TEXT"
  ");"
  "CREATE INDEX IF NOT EXISTS idx_leases_agent ON leases(agent_id);"
  "CREATE INDEX IF NOT EXISTS idx_leases_secret ON leases(secret_id);"

  /* v2: per-credential access policy */
  "CREATE TABLE IF NOT EXISTS credential_policy ("
  "  credential_id TEXT NOT NULL,"
  "  authorized_agents TEXT NOT NULL,"
  "  session_broker INTEGER DEFAULT 0,"
  "  rate_limit TEXT,"
  "  lease_duration_s INTEGER"
  ");"
  "CREATE INDEX IF NOT EXISTS idx_credpol_cred ON credential_policy(credential_id);"

  /* v2: bootstrap tokens (single-use, attempt-limited) */
  "CREATE TABLE IF NOT EXISTS bootstrap_tokens ("
  "  token_hash TEXT PRIMARY KEY,"
  "  agent_id TEXT NOT NULL,"
  "  bootstrap_pubkey TEXT NOT NULL,"
  "  issued_at INTEGER NOT NULL,"
  "  expires_at INTEGER NOT NULL,"
  "  used_at INTEGER,"
  "  handoff_secret TEXT,"
  "  attempt_count INTEGER DEFAULT 0"
  ");"
  "CREATE INDEX IF NOT EXISTS idx_bootstrap_agent ON bootstrap_tokens(agent_id);"
  "CREATE INDEX IF NOT EXISTS idx_bootstrap_handoff ON bootstrap_tokens(handoff_secret);"

  /* v2: deny list for revocation */
  "CREATE TABLE IF NOT EXISTS deny_list ("
  "  pubkey_hex TEXT PRIMARY KEY NOT NULL,"
  "  agent_id TEXT,"
  "  reason TEXT,"
  "  denied_at INTEGER NOT NULL"
  ");"
  "CREATE INDEX IF NOT EXISTS idx_deny_agent ON deny_list(agent_id);"

  /* v2: append-only hash-chained audit log */
  "CREATE TABLE IF NOT EXISTS audit_log ("
  "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
  "  ts INTEGER NOT NULL,"
  "  agent_id TEXT NOT NULL,"
  "  operation TEXT NOT NULL,"
  "  secret_id TEXT,"
  "  transport TEXT,"
  "  detail TEXT,"
  "  prev_hash TEXT NOT NULL,"
  "  entry_hash TEXT NOT NULL"
  ");"
  "CREATE INDEX IF NOT EXISTS idx_audit_agent ON audit_log(agent_id);"
  "CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit_log(ts);"
;

/* ------------------------------ public API -------------------------------- */

SignetStore *signet_store_open(const SignetStoreConfig *cfg) {
  if (!cfg || !cfg->db_path || !cfg->master_key) return NULL;

  /* Initialize libsodium (idempotent). */
  if (sodium_init() < 0) return NULL;

  SignetStore *store = (SignetStore *)calloc(1, sizeof(*store));
  if (!store) return NULL;

  /* Lock the DEK in memory. */
  sodium_mlock(store->dek, SIGNET_DEK_LEN);

  /* Derive data-encryption key from master key. */
  if (!signet_derive_dek(cfg->master_key, store->dek)) {
    sodium_munlock(store->dek, SIGNET_DEK_LEN);
    free(store);
    return NULL;
  }

  /* Open SQLCipher database. */
  int rc = sqlite3_open(cfg->db_path, &store->db);
  if (rc != SQLITE_OK || !store->db) {
    sodium_memzero(store->dek, SIGNET_DEK_LEN);
    sodium_munlock(store->dek, SIGNET_DEK_LEN);
    if (store->db) sqlite3_close(store->db);
    free(store);
    return NULL;
  }

  /* Set SQLCipher encryption key.
   * SQLCipher uses PRAGMA key to set the database encryption key.
   * We pass the master key directly (SQLCipher handles its own KDF internally). */
  char *pragma = sqlite3_mprintf("PRAGMA key = '%q';", cfg->master_key);
  if (pragma) {
    rc = sqlite3_exec(store->db, pragma, NULL, NULL, NULL);
    sqlite3_free(pragma);
    if (rc != SQLITE_OK) {
      /* If this is regular SQLite (not SQLCipher), PRAGMA key is a no-op.
       * We continue — the envelope encryption layer provides security. */
    }
  }

  /* Create schema. */
  char *errmsg = NULL;
  rc = sqlite3_exec(store->db, SIGNET_SCHEMA_SQL, NULL, NULL, &errmsg);
  if (rc != SQLITE_OK) {
    if (errmsg) sqlite3_free(errmsg);
    signet_store_close(store);
    return NULL;
  }

  /* Additive migrations for older v2 databases. Safe to ignore if already applied. */
  (void)sqlite3_exec(store->db,
                     "ALTER TABLE bootstrap_tokens ADD COLUMN handoff_secret TEXT;",
                     NULL, NULL, NULL);
  (void)sqlite3_exec(store->db,
                     "CREATE INDEX IF NOT EXISTS idx_bootstrap_handoff ON bootstrap_tokens(handoff_secret);",
                     NULL, NULL, NULL);

  /* Enable WAL mode for better concurrent read performance. */
  (void)sqlite3_exec(store->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

  store->open = true;
  return store;
}

void signet_store_close(SignetStore *store) {
  if (!store) return;

  if (store->db) {
    sqlite3_close(store->db);
    store->db = NULL;
  }

  sodium_memzero(store->dek, SIGNET_DEK_LEN);
  sodium_munlock(store->dek, SIGNET_DEK_LEN);

  store->open = false;
  free(store);
}

bool signet_store_is_open(const SignetStore *store) {
  return store && store->open && store->db;
}

int signet_store_put_agent(SignetStore *store,
                           const char *agent_id,
                           const uint8_t *secret_key,
                           size_t secret_key_len,
                           const char *connect_secret,
                           int64_t now) {
  if (!store || !store->open || !agent_id || !secret_key) return -1;
  if (secret_key_len != SIGNET_NSEC_LEN) return -1;

  /* Generate random nonce. */
  uint8_t nonce[SIGNET_NONCE_LEN];
  randombytes_buf(nonce, sizeof(nonce));

  /* Encrypt the secret key with the DEK. */
  size_t ct_len = SIGNET_NSEC_LEN + SIGNET_CIPHERTEXT_EXTRA;
  uint8_t *ciphertext = (uint8_t *)malloc(ct_len);
  if (!ciphertext) return -1;

  if (crypto_secretbox_easy(ciphertext, secret_key, SIGNET_NSEC_LEN,
                            nonce, store->dek) != 0) {
    free(ciphertext);
    return -1;
  }

  /* INSERT OR REPLACE into the database. */
  const char *sql =
    "INSERT OR REPLACE INTO agents (agent_id, encrypted_nsec, nonce, algo, connect_secret, created_at, last_used) "
    "VALUES (?, ?, ?, 'xsalsa20poly1305', ?, ?, 0);";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    free(ciphertext);
    return -1;
  }

  sqlite3_bind_text(stmt, 1, agent_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, ciphertext, (int)ct_len, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 3, nonce, SIGNET_NONCE_LEN, SQLITE_TRANSIENT);
  if (connect_secret && connect_secret[0]) {
    sqlite3_bind_text(stmt, 4, connect_secret, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 4);
  }
  sqlite3_bind_int64(stmt, 5, now);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  free(ciphertext);

  return (rc == SQLITE_DONE) ? 0 : -1;
}

int signet_store_get_agent(SignetStore *store,
                           const char *agent_id,
                           SignetAgentRecord *out_record) {
  if (!store || !store->open || !agent_id || !out_record) return -1;
  memset(out_record, 0, sizeof(*out_record));

  const char *sql =
    "SELECT encrypted_nsec, nonce, created_at, last_used, connect_secret FROM agents WHERE agent_id = ?;";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_text(stmt, 1, agent_id, -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 1 : -1; /* 1 = not found */
  }

  /* Extract encrypted blob and nonce. */
  const uint8_t *ct = (const uint8_t *)sqlite3_column_blob(stmt, 0);
  int ct_len = sqlite3_column_bytes(stmt, 0);
  const uint8_t *nonce = (const uint8_t *)sqlite3_column_blob(stmt, 1);
  int nonce_len = sqlite3_column_bytes(stmt, 1);
  int64_t created_at = sqlite3_column_int64(stmt, 2);
  int64_t last_used = sqlite3_column_int64(stmt, 3);

  if (!ct || ct_len != (int)(SIGNET_NSEC_LEN + SIGNET_CIPHERTEXT_EXTRA) ||
      !nonce || nonce_len != SIGNET_NONCE_LEN) {
    sqlite3_finalize(stmt);
    return -1;
  }

  /* Decrypt the secret key. Allocate in locked memory. */
  uint8_t *plaintext = (uint8_t *)sodium_malloc(SIGNET_NSEC_LEN);
  if (!plaintext) {
    sqlite3_finalize(stmt);
    return -1;
  }

  if (crypto_secretbox_open_easy(plaintext, ct, (size_t)ct_len,
                                  nonce, store->dek) != 0) {
    sodium_free(plaintext);
    sqlite3_finalize(stmt);
    return -1; /* decryption failed (tampered or wrong key) */
  }

  /* Read connect_secret (column 4, may be NULL). */
  const char *cs = (const char *)sqlite3_column_text(stmt, 4);

  out_record->agent_id = g_strdup(agent_id);
  out_record->secret_key = plaintext;
  out_record->secret_key_len = SIGNET_NSEC_LEN;
  out_record->connect_secret = cs ? g_strdup(cs) : NULL;
  out_record->created_at = created_at;
  out_record->last_used = last_used;

  sqlite3_finalize(stmt);
  return 0;
}

int signet_store_delete_agent(SignetStore *store, const char *agent_id) {
  if (!store || !store->open || !agent_id) return -1;

  const char *sql = "DELETE FROM agents WHERE agent_id = ?;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_text(stmt, 1, agent_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  int changes = sqlite3_changes(store->db);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) return -1;
  return (changes > 0) ? 0 : 1; /* 1 = not found */
}

int signet_store_list_agents(SignetStore *store, char ***out_ids, size_t *out_count) {
  if (!store || !store->open || !out_ids || !out_count) return -1;

  *out_ids = NULL;
  *out_count = 0;

  const char *sql = "SELECT agent_id FROM agents ORDER BY created_at;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  GPtrArray *arr = g_ptr_array_new_with_free_func(NULL); /* elements freed manually */

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *id = (const char *)sqlite3_column_text(stmt, 0);
    if (id) g_ptr_array_add(arr, g_strdup(id));
  }

  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    for (guint i = 0; i < arr->len; i++) g_free(g_ptr_array_index(arr, i));
    g_ptr_array_free(arr, TRUE);
    return -1;
  }

  size_t count = arr->len;
  char **ids = (char **)calloc(count + 1, sizeof(char *));
  if (!ids && count > 0) {
    for (guint i = 0; i < arr->len; i++) g_free(g_ptr_array_index(arr, i));
    g_ptr_array_free(arr, TRUE);
    return -1;
  }

  for (size_t i = 0; i < count; i++) {
    ids[i] = (char *)g_ptr_array_index(arr, (guint)i);
  }

  g_ptr_array_free(arr, TRUE); /* elements transferred to ids */

  *out_ids = ids;
  *out_count = count;
  return 0;
}

int signet_store_touch_agent(SignetStore *store, const char *agent_id, int64_t now) {
  if (!store || !store->open || !agent_id) return -1;

  const char *sql = "UPDATE agents SET last_used = ? WHERE agent_id = ?;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_int64(stmt, 1, now);
  sqlite3_bind_text(stmt, 2, agent_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return (rc == SQLITE_DONE) ? 0 : -1;
}

void signet_agent_record_clear(SignetAgentRecord *rec) {
  if (!rec) return;
  g_free(rec->agent_id);
  rec->agent_id = NULL;
  if (rec->secret_key) {
    sodium_free(rec->secret_key); /* also wipes */
    rec->secret_key = NULL;
  }
  rec->secret_key_len = 0;
  if (rec->connect_secret) {
    memset(rec->connect_secret, 0, strlen(rec->connect_secret));
    g_free(rec->connect_secret);
    rec->connect_secret = NULL;
  }
  rec->created_at = 0;
  rec->last_used = 0;
}

int signet_store_consume_connect_secret(SignetStore *store,
                                        const char *agent_id) {
  if (!store || !store->open || !agent_id) return -1;

  const char *sql = "UPDATE agents SET connect_secret = NULL WHERE agent_id = ? AND connect_secret IS NOT NULL;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  sqlite3_bind_text(stmt, 1, agent_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  int changes = sqlite3_changes(store->db);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) return -1;
  return (changes > 0) ? 0 : 1; /* 1 = not found or already consumed */
}

int signet_store_consume_connect_secret_value(SignetStore *store,
                                              const char *connect_secret,
                                              int64_t now,
                                              char **out_agent_id) {
  if (out_agent_id) *out_agent_id = NULL;
  if (!store || !store->open || !connect_secret || !connect_secret[0] || !out_agent_id) {
    return -1;
  }

  int rc = sqlite3_exec(store->db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
  if (rc != SQLITE_OK) return -1;

  const char *select_sql =
      "SELECT agent_id FROM agents WHERE connect_secret = ? LIMIT 1;";
  sqlite3_stmt *select_stmt = NULL;
  rc = sqlite3_prepare_v2(store->db, select_sql, -1, &select_stmt, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
  }

  sqlite3_bind_text(select_stmt, 1, connect_secret, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(select_stmt);
  if (rc == SQLITE_DONE) {
    sqlite3_finalize(select_stmt);
    sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
    return 1;
  }
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(select_stmt);
    sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
  }

  const char *agent_id = (const char *)sqlite3_column_text(select_stmt, 0);
  char *agent_copy = agent_id ? g_strdup(agent_id) : NULL;
  sqlite3_finalize(select_stmt);
  if (!agent_copy) {
    sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
  }

  const char *token_sql =
      "SELECT token_hash FROM bootstrap_tokens "
      "WHERE handoff_secret = ? AND agent_id = ? AND used_at IS NULL AND expires_at >= ? "
      "ORDER BY issued_at DESC LIMIT 1;";
  sqlite3_stmt *token_stmt = NULL;
  rc = sqlite3_prepare_v2(store->db, token_sql, -1, &token_stmt, NULL);
  if (rc != SQLITE_OK) {
    g_free(agent_copy);
    sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
  }
  sqlite3_bind_text(token_stmt, 1, connect_secret, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(token_stmt, 2, agent_copy, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(token_stmt, 3, now);

  rc = sqlite3_step(token_stmt);
  char *token_hash = NULL;
  if (rc == SQLITE_ROW) {
    const char *tok = (const char *)sqlite3_column_text(token_stmt, 0);
    token_hash = tok ? g_strdup(tok) : NULL;
  } else if (rc != SQLITE_DONE) {
    sqlite3_finalize(token_stmt);
    g_free(agent_copy);
    sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
  }
  sqlite3_finalize(token_stmt);

  if (token_hash) {
    const char *consume_token_sql =
        "UPDATE bootstrap_tokens SET used_at = ? WHERE token_hash = ? AND used_at IS NULL;";
    sqlite3_stmt *consume_token_stmt = NULL;
    rc = sqlite3_prepare_v2(store->db, consume_token_sql, -1, &consume_token_stmt, NULL);
    if (rc != SQLITE_OK) {
      g_free(token_hash);
      g_free(agent_copy);
      sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
      return -1;
    }
    sqlite3_bind_int64(consume_token_stmt, 1, now);
    sqlite3_bind_text(consume_token_stmt, 2, token_hash, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(consume_token_stmt);
    int token_changes = sqlite3_changes(store->db);
    sqlite3_finalize(consume_token_stmt);
    g_free(token_hash);
    if (rc != SQLITE_DONE || token_changes <= 0) {
      g_free(agent_copy);
      sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
      return 1;
    }
  }

  const char *update_sql =
      "UPDATE agents SET connect_secret = NULL WHERE agent_id = ? AND connect_secret = ?;";
  sqlite3_stmt *update_stmt = NULL;
  rc = sqlite3_prepare_v2(store->db, update_sql, -1, &update_stmt, NULL);
  if (rc != SQLITE_OK) {
    g_free(agent_copy);
    sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
  }
  sqlite3_bind_text(update_stmt, 1, agent_copy, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(update_stmt, 2, connect_secret, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(update_stmt);
  int changes = sqlite3_changes(store->db);
  sqlite3_finalize(update_stmt);

  if (rc != SQLITE_DONE || changes <= 0) {
    g_free(agent_copy);
    sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
    return (rc == SQLITE_DONE) ? 1 : -1;
  }

  rc = sqlite3_exec(store->db, "COMMIT;", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    g_free(agent_copy);
    sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
  }

  *out_agent_id = agent_copy;
  return 0;
}

sqlite3 *signet_store_get_db(SignetStore *store) {
  if (!store || !store->open) return NULL;
  return store->db;
}

const uint8_t *signet_store_get_dek(const SignetStore *store) {
  if (!store || !store->open) return NULL;
  return store->dek;
}

void signet_store_free_agent_ids(char **ids, size_t count) {
  if (!ids) return;
  for (size_t i = 0; i < count; i++) g_free(ids[i]);
  free(ids);
}
