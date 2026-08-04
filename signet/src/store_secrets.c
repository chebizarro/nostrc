/* SPDX-License-Identifier: MIT
 *
 * store_secrets.c - Extended secret storage operations.
 */

#include "signet/store_secrets.h"
#include "signet/store.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <sqlite3.h>
#include <sodium.h>

#define SIGNET_NONCE_LEN crypto_secretbox_NONCEBYTES
#define SIGNET_MAC_LEN crypto_secretbox_MACBYTES
#define SIGNET_MAX_SECRET_PAYLOAD (1024u * 1024u)

static void signet_derive_agent_key(const uint8_t *dek,
                                    const char *agent_pubkey,
                                    uint8_t out[32]) {
  crypto_generichash(out, 32,
                     (const uint8_t *)agent_pubkey, strlen(agent_pubkey),
                     dek, 32);
}

const char *signet_secret_type_to_string(SignetSecretType t) {
  switch (t) {
    case SIGNET_SECRET_NOSTR_NSEC: return "nostr_nsec";
    case SIGNET_SECRET_SSH_KEY: return "ssh_key";
    case SIGNET_SECRET_API_TOKEN: return "api_token";
    case SIGNET_SECRET_CREDENTIAL: return "credential";
    case SIGNET_SECRET_CERTIFICATE: return "certificate";
    default: return "unknown";
  }
}

bool signet_secret_type_parse(const char *s, SignetSecretType *out_type) {
  if (!s || !out_type) return false;
  if (strcmp(s, "nostr_nsec") == 0) *out_type = SIGNET_SECRET_NOSTR_NSEC;
  else if (strcmp(s, "ssh_key") == 0) *out_type = SIGNET_SECRET_SSH_KEY;
  else if (strcmp(s, "api_token") == 0) *out_type = SIGNET_SECRET_API_TOKEN;
  else if (strcmp(s, "credential") == 0) *out_type = SIGNET_SECRET_CREDENTIAL;
  else if (strcmp(s, "certificate") == 0) *out_type = SIGNET_SECRET_CERTIFICATE;
  else return false;
  return true;
}

SignetSecretType signet_secret_type_from_string(const char *s) {
  SignetSecretType type = SIGNET_SECRET_CREDENTIAL;
  (void)signet_secret_type_parse(s, &type);
  return type;
}

const char *signet_secret_status_to_string(SignetSecretStatus status) {
  switch (status) {
    case SIGNET_SECRET_STATUS_ACTIVE: return "active";
    case SIGNET_SECRET_STATUS_EXPIRED: return "expired";
    case SIGNET_SECRET_STATUS_REVOKED: return "revoked";
    default: return "unknown";
  }
}

static bool signet_secret_type_valid(SignetSecretType type) {
  return type >= SIGNET_SECRET_NOSTR_NSEC &&
         type <= SIGNET_SECRET_CERTIFICATE;
}

static bool signet_secret_text_valid(const char *s, size_t max_len,
                                     bool optional) {
  if (!s) return optional;
  size_t len = strlen(s);
  return (optional || len > 0) && len <= max_len &&
         g_utf8_validate(s, (gssize)len, NULL);
}

static bool signet_secret_hex64(const char *s) {
  if (!s || strlen(s) != 64) return false;
  for (size_t i = 0; i < 64; i++)
    if (!g_ascii_isxdigit(s[i])) return false;
  return true;
}

static void signet_hash_length_prefixed(crypto_generichash_state *state,
                                        const char *value) {
  size_t n = strlen(value);
  uint8_t len_be[4] = {
    (uint8_t)(n >> 24), (uint8_t)(n >> 16),
    (uint8_t)(n >> 8), (uint8_t)n
  };
  crypto_generichash_update(state, len_be, sizeof(len_be));
  crypto_generichash_update(state, (const uint8_t *)value, n);
}

int signet_secret_id_generate(const char *agent_id,
                              SignetSecretType secret_type,
                              const char *label,
                              char out_id[70]) {
  if (!out_id ||
      !signet_secret_text_valid(agent_id, 128, false) ||
      !signet_secret_text_valid(label, 256, false) ||
      !signet_secret_type_valid(secret_type)) return -1;

  static const char domain[] = "signet-credential-id-v1";
  uint8_t digest[32];
  crypto_generichash_state state;
  if (crypto_generichash_init(&state, NULL, 0, sizeof(digest)) != 0) return -1;
  crypto_generichash_update(&state, (const uint8_t *)domain, sizeof(domain) - 1);
  signet_hash_length_prefixed(&state, agent_id);
  signet_hash_length_prefixed(&state, signet_secret_type_to_string(secret_type));
  signet_hash_length_prefixed(&state, label);
  crypto_generichash_final(&state, digest, sizeof(digest));

  memcpy(out_id, "cred_", 5);
  sodium_bin2hex(out_id + 5, 65, digest, sizeof(digest));
  sodium_memzero(digest, sizeof(digest));
  return 0;
}

static int signet_encrypt_payload(const uint8_t *dek,
                                  const char *agent_pubkey,
                                  const uint8_t *payload,
                                  size_t payload_len,
                                  uint8_t nonce[SIGNET_NONCE_LEN],
                                  uint8_t **out_ciphertext,
                                  size_t *out_ciphertext_len) {
  if (!dek || !agent_pubkey || !payload || payload_len == 0 ||
      payload_len > SIGNET_MAX_SECRET_PAYLOAD ||
      !out_ciphertext || !out_ciphertext_len) return -1;

  uint8_t akey[32];
  signet_derive_agent_key(dek, agent_pubkey, akey);
  randombytes_buf(nonce, SIGNET_NONCE_LEN);

  size_t ct_len = payload_len + SIGNET_MAC_LEN;
  uint8_t *ciphertext = malloc(ct_len);
  if (!ciphertext) {
    sodium_memzero(akey, sizeof(akey));
    return -1;
  }
  int rc = crypto_secretbox_easy(ciphertext, payload, payload_len, nonce, akey);
  sodium_memzero(akey, sizeof(akey));
  if (rc != 0) {
    sodium_memzero(ciphertext, ct_len);
    free(ciphertext);
    return -1;
  }
  *out_ciphertext = ciphertext;
  *out_ciphertext_len = ct_len;
  return 0;
}

static SignetSecretStatus signet_secret_derive_status(int64_t revoked_at,
                                                       int64_t expires_at,
                                                       int64_t now) {
  if (revoked_at > 0) return SIGNET_SECRET_STATUS_REVOKED;
  if (expires_at > 0 && now >= expires_at) return SIGNET_SECRET_STATUS_EXPIRED;
  return SIGNET_SECRET_STATUS_ACTIVE;
}

static void signet_secret_metadata_from_stmt(sqlite3_stmt *stmt,
                                             int64_t now,
                                             SignetSecretMetadata *out) {
  memset(out, 0, sizeof(*out));
  out->id = g_strdup((const char *)sqlite3_column_text(stmt, 0));
  out->agent_id = g_strdup((const char *)sqlite3_column_text(stmt, 1));
  out->secret_type = signet_secret_type_from_string(
      (const char *)sqlite3_column_text(stmt, 2));
  out->label = g_strdup((const char *)sqlite3_column_text(stmt, 3));
  out->policy_id = g_strdup((const char *)sqlite3_column_text(stmt, 4));
  out->provenance = g_strdup((const char *)sqlite3_column_text(stmt, 5));
  out->created_by = g_strdup((const char *)sqlite3_column_text(stmt, 6));
  out->created_at = sqlite3_column_int64(stmt, 7);
  out->rotated_at = sqlite3_column_type(stmt, 8) == SQLITE_NULL
                      ? 0 : sqlite3_column_int64(stmt, 8);
  out->expires_at = sqlite3_column_type(stmt, 9) == SQLITE_NULL
                      ? 0 : sqlite3_column_int64(stmt, 9);
  out->revoked_at = sqlite3_column_type(stmt, 10) == SQLITE_NULL
                      ? 0 : sqlite3_column_int64(stmt, 10);
  out->version = sqlite3_column_int(stmt, 11);
  out->active_version = sqlite3_column_int(stmt, 12);
  out->status = signet_secret_derive_status(out->revoked_at,
                                             out->expires_at, now);
}

static const char *SIGNET_SECRET_METADATA_COLUMNS =
    "id, agent_id, secret_type, label, policy_id, provenance, created_by, "
    "created_at, rotated_at, expires_at, revoked_at, version, active_version";

void signet_secret_metadata_clear(SignetSecretMetadata *metadata) {
  if (!metadata) return;
  g_free(metadata->id);
  g_free(metadata->agent_id);
  g_free(metadata->label);
  g_free(metadata->policy_id);
  g_free(metadata->provenance);
  g_free(metadata->created_by);
  memset(metadata, 0, sizeof(*metadata));
}

void signet_secret_metadata_list_free(SignetSecretMetadata *metadata,
                                      size_t count) {
  if (!metadata) return;
  for (size_t i = 0; i < count; i++)
    signet_secret_metadata_clear(&metadata[i]);
  g_free(metadata);
}

static SignetSecretResult signet_store_resolve_agent_pubkey(
    sqlite3 *db, const char *agent_id, char out_pubkey[65]) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db,
      "SELECT pubkey FROM agents WHERE agent_id = ?;", -1, &stmt, NULL) != SQLITE_OK)
    return SIGNET_SECRET_ERROR;
  sqlite3_bind_text(stmt, 1, agent_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? SIGNET_SECRET_NOT_FOUND : SIGNET_SECRET_ERROR;
  }
  const char *pubkey = (const char *)sqlite3_column_text(stmt, 0);
  if (!signet_secret_hex64(pubkey)) {
    sqlite3_finalize(stmt);
    return SIGNET_SECRET_ERROR;
  }
  for (size_t i = 0; i < 64; i++)
    out_pubkey[i] = (char)g_ascii_tolower(pubkey[i]);
  out_pubkey[64] = '\0';
  sqlite3_finalize(stmt);
  return SIGNET_SECRET_OK;
}

SignetSecretResult signet_store_create_secret(
    SignetStore *store,
    const char *agent_id,
    SignetSecretType secret_type,
    const char *label,
    const uint8_t *payload,
    size_t payload_len,
    const char *policy_id,
    int64_t expires_at,
    const char *provenance,
    const char *created_by,
    int64_t now,
    SignetSecretMetadata *out_metadata) {
  sqlite3 *db = signet_store_get_db(store);
  const uint8_t *dek = signet_store_get_dek(store);
  if (!db || !dek ||
      !signet_secret_text_valid(agent_id, 128, false) ||
      !signet_secret_text_valid(label, 256, false) ||
      !signet_secret_text_valid(policy_id, 128, true) ||
      !signet_secret_type_valid(secret_type) ||
      !payload || payload_len == 0 || payload_len > SIGNET_MAX_SECRET_PAYLOAD ||
      (expires_at != 0 && expires_at <= now) ||
      !provenance ||
      (strcmp(provenance, "created") != 0 &&
       strcmp(provenance, "imported") != 0 &&
       strcmp(provenance, "legacy") != 0) ||
      (created_by && !signet_secret_hex64(created_by)))
    return SIGNET_SECRET_ERROR;

  char id[70];
  char agent_pubkey[65];
  if (signet_secret_id_generate(agent_id, secret_type, label, id) != 0)
    return SIGNET_SECRET_ERROR;
  SignetSecretResult owner_rc =
      signet_store_resolve_agent_pubkey(db, agent_id, agent_pubkey);
  if (owner_rc != SIGNET_SECRET_OK) return owner_rc;

  uint8_t nonce[SIGNET_NONCE_LEN];
  uint8_t *ciphertext = NULL;
  size_t ct_len = 0;
  if (signet_encrypt_payload(dek, agent_pubkey, payload, payload_len,
                             nonce, &ciphertext, &ct_len) != 0)
    return SIGNET_SECRET_ERROR;

  char created_by_lower[65] = {0};
  if (created_by) {
    for (size_t i = 0; i < 64; i++)
      created_by_lower[i] = (char)g_ascii_tolower(created_by[i]);
  }

  const char *sql =
      "INSERT INTO secrets "
      "(id, agent_id, agent_pubkey, secret_type, label, payload, nonce, "
      " policy_id, created_at, expires_at, version, active_version, "
      " provenance, created_by, version_created_at) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1, 1, ?, ?, ?);";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, agent_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, agent_pubkey, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, signet_secret_type_to_string(secret_type),
                      -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 6, ciphertext, (int)ct_len, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 7, nonce, SIGNET_NONCE_LEN, SQLITE_TRANSIENT);
    if (policy_id) sqlite3_bind_text(stmt, 8, policy_id, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 8);
    sqlite3_bind_int64(stmt, 9, now);
    if (expires_at) sqlite3_bind_int64(stmt, 10, expires_at);
    else sqlite3_bind_null(stmt, 10);
    sqlite3_bind_text(stmt, 11, provenance, -1, SQLITE_TRANSIENT);
    if (created_by) sqlite3_bind_text(stmt, 12, created_by_lower, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 12);
    sqlite3_bind_int64(stmt, 13, now);
    rc = sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  sodium_memzero(ciphertext, ct_len);
  free(ciphertext);
  sodium_memzero(nonce, sizeof(nonce));

  if (rc != SQLITE_DONE)
    return ((rc & 0xff) == SQLITE_CONSTRAINT)
             ? SIGNET_SECRET_EXISTS : SIGNET_SECRET_ERROR;
  if (out_metadata)
    return signet_store_get_secret_metadata(store, id, now, out_metadata);
  return SIGNET_SECRET_OK;
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
  if (!db || !dek || !id || !agent_id || !agent_pubkey || !agent_pubkey[0] ||
      !label || !payload || payload_len == 0 ||
      payload_len > SIGNET_MAX_SECRET_PAYLOAD ||
      !signet_secret_type_valid(secret_type)) return -1;

  uint8_t nonce[SIGNET_NONCE_LEN];
  uint8_t *ciphertext = NULL;
  size_t ct_len = 0;
  if (signet_encrypt_payload(dek, agent_pubkey, payload, payload_len,
                             nonce, &ciphertext, &ct_len) != 0) return -1;

  const char *sql =
      "INSERT INTO secrets "
      "(id, agent_id, agent_pubkey, secret_type, label, payload, nonce, "
      " policy_id, created_at, version, active_version, provenance, "
      " version_created_at) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 1, 1, 'legacy', ?);";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, agent_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, agent_pubkey, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, signet_secret_type_to_string(secret_type),
                      -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 6, ciphertext, (int)ct_len, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 7, nonce, SIGNET_NONCE_LEN, SQLITE_TRANSIENT);
    if (policy_id) sqlite3_bind_text(stmt, 8, policy_id, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 8);
    sqlite3_bind_int64(stmt, 9, now);
    sqlite3_bind_int64(stmt, 10, now);
    rc = sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  sodium_memzero(ciphertext, ct_len);
  free(ciphertext);
  sodium_memzero(nonce, sizeof(nonce));
  if (rc == SQLITE_DONE) return 0;
  return ((rc & 0xff) == SQLITE_CONSTRAINT) ? 1 : -1;
}

SignetSecretResult signet_store_get_secret_metadata(
    SignetStore *store,
    const char *id,
    int64_t now,
    SignetSecretMetadata *out_metadata) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !id || !out_metadata) return SIGNET_SECRET_ERROR;
  memset(out_metadata, 0, sizeof(*out_metadata));

  char *sql = g_strdup_printf("SELECT %s FROM secrets WHERE id = ?;",
                              SIGNET_SECRET_METADATA_COLUMNS);
  sqlite3_stmt *stmt = NULL;
  int rc = sql ? sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) : SQLITE_NOMEM;
  g_free(sql);
  if (rc != SQLITE_OK) return SIGNET_SECRET_ERROR;
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW)
    signet_secret_metadata_from_stmt(stmt, now, out_metadata);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_ROW) return SIGNET_SECRET_OK;
  return rc == SQLITE_DONE ? SIGNET_SECRET_NOT_FOUND : SIGNET_SECRET_ERROR;
}

SignetSecretResult signet_store_list_secret_metadata(
    SignetStore *store,
    const char *agent_id,
    int64_t now,
    SignetSecretMetadata **out_metadata,
    size_t *out_count) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !out_metadata || !out_count) return SIGNET_SECRET_ERROR;
  *out_metadata = NULL;
  *out_count = 0;

  char *sql = g_strdup_printf(
      agent_id
        ? "SELECT %s FROM secrets WHERE agent_id = ? ORDER BY created_at, id;"
        : "SELECT %s FROM secrets ORDER BY created_at, id;",
      SIGNET_SECRET_METADATA_COLUMNS);
  sqlite3_stmt *stmt = NULL;
  int rc = sql ? sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) : SQLITE_NOMEM;
  g_free(sql);
  if (rc != SQLITE_OK) return SIGNET_SECRET_ERROR;
  if (agent_id) sqlite3_bind_text(stmt, 1, agent_id, -1, SQLITE_TRANSIENT);

  SignetSecretMetadata *items = NULL;
  size_t count = 0;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    SignetSecretMetadata *grown =
        g_try_realloc_n(items, count + 1, sizeof(*items));
    if (!grown) {
      rc = SQLITE_NOMEM;
      break;
    }
    items = grown;
    memset(&items[count], 0, sizeof(items[count]));
    signet_secret_metadata_from_stmt(stmt, now, &items[count]);
    count++;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    signet_secret_metadata_list_free(items, count);
    return SIGNET_SECRET_ERROR;
  }
  *out_metadata = items;
  *out_count = count;
  return SIGNET_SECRET_OK;
}

SignetSecretResult signet_store_get_secret_at(
    SignetStore *store,
    const char *id,
    int64_t now,
    SignetSecretRecord *out_record) {
  sqlite3 *db = signet_store_get_db(store);
  const uint8_t *dek = signet_store_get_dek(store);
  if (!db || !dek || !id || !out_record) return SIGNET_SECRET_ERROR;
  memset(out_record, 0, sizeof(*out_record));

  /* Keep the FULLMUTEX connection locked across the complete statement and
   * decrypt/copy sequence. NIP-46 signing touches agents.last_used on this same
   * handle; per-call SQLite locking alone would allow that write to interleave
   * between step(), column access, and finalize(). */
  sqlite3_mutex *db_mutex = sqlite3_db_mutex(db);
  sqlite3_mutex_enter(db_mutex);

  const char *sql =
      "SELECT agent_id, agent_pubkey, secret_type, label, payload, nonce, "
      "policy_id, created_at, rotated_at, expires_at, version, active_version, "
      "revoked_at FROM secrets WHERE id = ?;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_mutex_leave(db_mutex);
    return SIGNET_SECRET_ERROR;
  }
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    sqlite3_mutex_leave(db_mutex);
    return rc == SQLITE_DONE ? SIGNET_SECRET_NOT_FOUND : SIGNET_SECRET_ERROR;
  }

  int64_t expires_at = sqlite3_column_type(stmt, 9) == SQLITE_NULL
                         ? 0 : sqlite3_column_int64(stmt, 9);
  int64_t revoked_at = sqlite3_column_type(stmt, 12) == SQLITE_NULL
                         ? 0 : sqlite3_column_int64(stmt, 12);
  if (revoked_at > 0) {
    sqlite3_finalize(stmt);
    sqlite3_mutex_leave(db_mutex);
    return SIGNET_SECRET_REVOKED;
  }
  if (expires_at > 0 && now >= expires_at) {
    sqlite3_finalize(stmt);
    sqlite3_mutex_leave(db_mutex);
    return SIGNET_SECRET_EXPIRED;
  }

  const char *agent_pubkey = (const char *)sqlite3_column_text(stmt, 1);
  const uint8_t *ct = sqlite3_column_blob(stmt, 4);
  int ct_len = sqlite3_column_bytes(stmt, 4);
  const uint8_t *nonce = sqlite3_column_blob(stmt, 5);
  int nonce_len = sqlite3_column_bytes(stmt, 5);
  if (!agent_pubkey || !ct || ct_len < (int)SIGNET_MAC_LEN ||
      !nonce || nonce_len != SIGNET_NONCE_LEN) {
    sqlite3_finalize(stmt);
    sqlite3_mutex_leave(db_mutex);
    return SIGNET_SECRET_ERROR;
  }

  uint8_t akey[32];
  signet_derive_agent_key(dek, agent_pubkey, akey);
  size_t pt_len = (size_t)ct_len - SIGNET_MAC_LEN;
  uint8_t *plaintext = sodium_malloc(pt_len);
  if (!plaintext) {
    sodium_memzero(akey, sizeof(akey));
    sqlite3_finalize(stmt);
    sqlite3_mutex_leave(db_mutex);
    return SIGNET_SECRET_ERROR;
  }
  if (crypto_secretbox_open_easy(plaintext, ct, (size_t)ct_len,
                                 nonce, akey) != 0) {
    sodium_memzero(akey, sizeof(akey));
    sodium_free(plaintext);
    sqlite3_finalize(stmt);
    sqlite3_mutex_leave(db_mutex);
    return SIGNET_SECRET_ERROR;
  }
  sodium_memzero(akey, sizeof(akey));

  out_record->id = g_strdup(id);
  out_record->agent_id = g_strdup((const char *)sqlite3_column_text(stmt, 0));
  out_record->agent_pubkey = g_strdup(agent_pubkey);
  out_record->secret_type = signet_secret_type_from_string(
      (const char *)sqlite3_column_text(stmt, 2));
  out_record->label = g_strdup((const char *)sqlite3_column_text(stmt, 3));
  out_record->payload = plaintext;
  out_record->payload_len = pt_len;
  out_record->policy_id = g_strdup((const char *)sqlite3_column_text(stmt, 6));
  out_record->created_at = sqlite3_column_int64(stmt, 7);
  out_record->rotated_at = sqlite3_column_type(stmt, 8) == SQLITE_NULL
                             ? 0 : sqlite3_column_int64(stmt, 8);
  out_record->expires_at = expires_at;
  out_record->version = sqlite3_column_int(stmt, 10);
  out_record->active_version = sqlite3_column_int(stmt, 11);
  sqlite3_finalize(stmt);
  sqlite3_mutex_leave(db_mutex);
  return SIGNET_SECRET_OK;
}

int signet_store_get_secret(SignetStore *store,
                            const char *id,
                            SignetSecretRecord *out_record) {
  SignetSecretResult rc =
      signet_store_get_secret_at(store, id, (int64_t)time(NULL), out_record);
  if (rc == SIGNET_SECRET_OK) return 0;
  if (rc == SIGNET_SECRET_NOT_FOUND ||
      rc == SIGNET_SECRET_EXPIRED ||
      rc == SIGNET_SECRET_REVOKED) return 1;
  return -1;
}

int signet_store_delete_secret(SignetStore *store, const char *id) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !id) return -1;
  sqlite3_mutex *mutex = sqlite3_db_mutex(db);
  sqlite3_mutex_enter(mutex);
  int result = -1;
  if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
    goto done;

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db, "DELETE FROM secret_versions WHERE id = ?;",
                         -1, &stmt, NULL) != SQLITE_OK)
    goto rollback;
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  stmt = NULL;
  if (rc != SQLITE_DONE) goto rollback;

  if (sqlite3_prepare_v2(db, "DELETE FROM secrets WHERE id = ?;",
                         -1, &stmt, NULL) != SQLITE_OK)
    goto rollback;
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  int changes = sqlite3_changes(db);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) goto rollback;
  if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
    goto rollback;
  result = changes > 0 ? 0 : 1;
  goto done;

rollback:
  (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
done:
  sqlite3_mutex_leave(mutex);
  return result;
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

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db,
      "SELECT id, label FROM secrets WHERE agent_id = ? ORDER BY created_at, id;",
      -1, &stmt, NULL);
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
    for (guint i = 0; i < ids->len; i++) {
      g_free(ids->pdata[i]);
      g_free(labels->pdata[i]);
    }
    g_ptr_array_free(ids, TRUE);
    g_ptr_array_free(labels, TRUE);
    return -1;
  }

  size_t count = ids->len;
  *out_ids = g_new0(char *, count + 1);
  *out_labels = g_new0(char *, count + 1);
  for (size_t i = 0; i < count; i++) {
    (*out_ids)[i] = ids->pdata[i];
    (*out_labels)[i] = labels->pdata[i];
  }
  *out_count = count;
  g_ptr_array_free(ids, TRUE);
  g_ptr_array_free(labels, TRUE);
  return 0;
}

SignetSecretResult signet_store_rotate_secret_ex(
    SignetStore *store,
    const char *id,
    const uint8_t *new_payload,
    size_t new_payload_len,
    bool replace_expires_at,
    int64_t expires_at,
    int64_t now,
    SignetSecretMetadata *out_metadata) {
  sqlite3 *db = signet_store_get_db(store);
  const uint8_t *dek = signet_store_get_dek(store);
  if (!db || !dek || !id || !new_payload || new_payload_len == 0 ||
      new_payload_len > SIGNET_MAX_SECRET_PAYLOAD ||
      (replace_expires_at && expires_at != 0 && expires_at <= now))
    return SIGNET_SECRET_ERROR;

  sqlite3_mutex *mutex = sqlite3_db_mutex(db);
  sqlite3_mutex_enter(mutex);
  SignetSecretResult result = SIGNET_SECRET_ERROR;
  sqlite3_stmt *stmt = NULL;
  uint8_t *old_payload = NULL;
  uint8_t *old_nonce = NULL;
  uint8_t *ciphertext = NULL;
  size_t ct_len = 0;
  char *agent_pubkey = NULL;
  char *provenance = NULL;
  uint8_t nonce[SIGNET_NONCE_LEN] = {0};
  int old_payload_len = 0, old_nonce_len = 0;
  int old_version = 0;
  int64_t version_created_at = 0;
  int64_t old_expires_at = 0;

  if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
    goto cleanup;

  const char *select_sql =
      "SELECT active_version, payload, nonce, agent_pubkey, created_at, "
      "rotated_at, expires_at, provenance, revoked_at, version_created_at "
      "FROM secrets WHERE id = ?;";
  if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, NULL) != SQLITE_OK)
    goto rollback;
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    result = rc == SQLITE_DONE ? SIGNET_SECRET_NOT_FOUND : SIGNET_SECRET_ERROR;
    sqlite3_finalize(stmt);
    stmt = NULL;
    goto rollback;
  }

  int64_t revoked_at = sqlite3_column_type(stmt, 8) == SQLITE_NULL
                         ? 0 : sqlite3_column_int64(stmt, 8);
  old_expires_at = sqlite3_column_type(stmt, 6) == SQLITE_NULL
                     ? 0 : sqlite3_column_int64(stmt, 6);
  if (revoked_at > 0) {
    result = SIGNET_SECRET_REVOKED;
    sqlite3_finalize(stmt);
    stmt = NULL;
    goto rollback;
  }
  if (old_expires_at > 0 && now >= old_expires_at && !replace_expires_at) {
    result = SIGNET_SECRET_EXPIRED;
    sqlite3_finalize(stmt);
    stmt = NULL;
    goto rollback;
  }

  old_version = sqlite3_column_int(stmt, 0);
  old_payload_len = sqlite3_column_bytes(stmt, 1);
  old_nonce_len = sqlite3_column_bytes(stmt, 2);
  const void *old_payload_col = sqlite3_column_blob(stmt, 1);
  const void *old_nonce_col = sqlite3_column_blob(stmt, 2);
  const char *pubkey_col = (const char *)sqlite3_column_text(stmt, 3);
  const char *provenance_col = (const char *)sqlite3_column_text(stmt, 7);
  int64_t initial_created_at = sqlite3_column_int64(stmt, 4);
  int64_t rotated_at = sqlite3_column_type(stmt, 5) == SQLITE_NULL
                         ? 0 : sqlite3_column_int64(stmt, 5);
  version_created_at = sqlite3_column_type(stmt, 9) == SQLITE_NULL
                         ? 0 : sqlite3_column_int64(stmt, 9);
  if (version_created_at == 0)
    version_created_at = old_version == 1 ? initial_created_at : rotated_at;

  if (!old_payload_col || old_payload_len < (int)SIGNET_MAC_LEN ||
      !old_nonce_col || old_nonce_len != SIGNET_NONCE_LEN ||
      !signet_secret_hex64(pubkey_col)) {
    sqlite3_finalize(stmt);
    stmt = NULL;
    goto rollback;
  }
  old_payload = malloc((size_t)old_payload_len);
  old_nonce = malloc((size_t)old_nonce_len);
  agent_pubkey = g_strdup(pubkey_col);
  provenance = g_strdup(provenance_col ? provenance_col : "legacy");
  if (!old_payload || !old_nonce || !agent_pubkey || !provenance) {
    sqlite3_finalize(stmt);
    stmt = NULL;
    goto rollback;
  }
  memcpy(old_payload, old_payload_col, (size_t)old_payload_len);
  memcpy(old_nonce, old_nonce_col, (size_t)old_nonce_len);
  sqlite3_finalize(stmt);
  stmt = NULL;

  if (signet_encrypt_payload(dek, agent_pubkey, new_payload, new_payload_len,
                             nonce, &ciphertext, &ct_len) != 0)
    goto rollback;

  const char *archive_sql =
      "INSERT INTO secret_versions "
      "(id, version, payload, nonce, created_at, expires_at, provenance, retired_at) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
  if (sqlite3_prepare_v2(db, archive_sql, -1, &stmt, NULL) != SQLITE_OK)
    goto rollback;
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, old_version);
  sqlite3_bind_blob(stmt, 3, old_payload, old_payload_len, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 4, old_nonce, old_nonce_len, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, version_created_at);
  if (old_expires_at) sqlite3_bind_int64(stmt, 6, old_expires_at);
  else sqlite3_bind_null(stmt, 6);
  sqlite3_bind_text(stmt, 7, provenance, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 8, now);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  stmt = NULL;
  if (rc != SQLITE_DONE) goto rollback;

  const char *update_sql =
      "UPDATE secrets SET payload = ?, nonce = ?, version = ?, active_version = ?, "
      "rotated_at = ?, version_created_at = ?, expires_at = "
      "CASE WHEN ? THEN ? ELSE expires_at END "
      "WHERE id = ? AND active_version = ? AND revoked_at IS NULL;";
  if (sqlite3_prepare_v2(db, update_sql, -1, &stmt, NULL) != SQLITE_OK)
    goto rollback;
  int new_version = old_version + 1;
  sqlite3_bind_blob(stmt, 1, ciphertext, (int)ct_len, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, nonce, SIGNET_NONCE_LEN, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, new_version);
  sqlite3_bind_int(stmt, 4, new_version);
  sqlite3_bind_int64(stmt, 5, now);
  sqlite3_bind_int64(stmt, 6, now);
  sqlite3_bind_int(stmt, 7, replace_expires_at ? 1 : 0);
  if (expires_at) sqlite3_bind_int64(stmt, 8, expires_at);
  else sqlite3_bind_null(stmt, 8);
  sqlite3_bind_text(stmt, 9, id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 10, old_version);
  rc = sqlite3_step(stmt);
  int changes = sqlite3_changes(db);
  sqlite3_finalize(stmt);
  stmt = NULL;
  if (rc != SQLITE_DONE || changes != 1) goto rollback;

  if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
    goto rollback;
  result = SIGNET_SECRET_OK;
  goto cleanup;

rollback:
  (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
cleanup:
  sqlite3_finalize(stmt);
  if (old_payload) {
    sodium_memzero(old_payload, (size_t)old_payload_len);
    free(old_payload);
  }
  if (old_nonce) {
    sodium_memzero(old_nonce, (size_t)old_nonce_len);
    free(old_nonce);
  }
  if (ciphertext) {
    sodium_memzero(ciphertext, ct_len);
    free(ciphertext);
  }
  sodium_memzero(nonce, sizeof(nonce));
  g_free(agent_pubkey);
  g_free(provenance);
  sqlite3_mutex_leave(mutex);

  if (result == SIGNET_SECRET_OK && out_metadata)
    return signet_store_get_secret_metadata(store, id, now, out_metadata);
  return result;
}

int signet_store_rotate_secret(SignetStore *store,
                               const char *id,
                               const uint8_t *new_payload,
                               size_t new_payload_len,
                               int64_t now) {
  return signet_store_rotate_secret_ex(store, id, new_payload, new_payload_len,
                                       false, 0, now, NULL) == SIGNET_SECRET_OK
           ? 0 : -1;
}

SignetSecretResult signet_store_revoke_secret(
    SignetStore *store,
    const char *id,
    int64_t now,
    SignetSecretMetadata *out_metadata) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !id) return SIGNET_SECRET_ERROR;
  sqlite3_mutex *mutex = sqlite3_db_mutex(db);
  sqlite3_mutex_enter(mutex);
  SignetSecretResult result = SIGNET_SECRET_ERROR;

  if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
    goto done;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db,
      "SELECT revoked_at FROM secrets WHERE id = ?;",
      -1, &stmt, NULL) != SQLITE_OK)
    goto rollback;
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    result = rc == SQLITE_DONE ? SIGNET_SECRET_NOT_FOUND : SIGNET_SECRET_ERROR;
    sqlite3_finalize(stmt);
    goto rollback;
  }
  bool already = sqlite3_column_type(stmt, 0) != SQLITE_NULL &&
                 sqlite3_column_int64(stmt, 0) > 0;
  sqlite3_finalize(stmt);
  if (!already) {
    if (sqlite3_prepare_v2(db,
        "UPDATE secrets SET revoked_at = ? WHERE id = ? AND revoked_at IS NULL;",
        -1, &stmt, NULL) != SQLITE_OK)
      goto rollback;
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || changes != 1) goto rollback;
  }
  if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
    goto rollback;
  result = SIGNET_SECRET_OK;
  goto done;

rollback:
  (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
done:
  sqlite3_mutex_leave(mutex);
  if (result == SIGNET_SECRET_OK && out_metadata)
    return signet_store_get_secret_metadata(store, id, now, out_metadata);
  return result;
}

SignetSecretResult signet_store_delete_revoked_secret(
    SignetStore *store,
    const char *id) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !id) return SIGNET_SECRET_ERROR;
  sqlite3_mutex *mutex = sqlite3_db_mutex(db);
  sqlite3_mutex_enter(mutex);
  SignetSecretResult result = SIGNET_SECRET_ERROR;

  if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
    goto done;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db,
      "SELECT revoked_at FROM secrets WHERE id = ?;",
      -1, &stmt, NULL) != SQLITE_OK)
    goto rollback;
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    result = rc == SQLITE_DONE ? SIGNET_SECRET_NOT_FOUND : SIGNET_SECRET_ERROR;
    sqlite3_finalize(stmt);
    goto rollback;
  }
  bool revoked = sqlite3_column_type(stmt, 0) != SQLITE_NULL &&
                 sqlite3_column_int64(stmt, 0) > 0;
  sqlite3_finalize(stmt);
  if (!revoked) {
    result = SIGNET_SECRET_NOT_REVOKED;
    goto rollback;
  }

  if (sqlite3_prepare_v2(db, "DELETE FROM secret_versions WHERE id = ?;",
                         -1, &stmt, NULL) != SQLITE_OK)
    goto rollback;
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) goto rollback;

  if (sqlite3_prepare_v2(db,
      "DELETE FROM secrets WHERE id = ? AND revoked_at IS NOT NULL;",
      -1, &stmt, NULL) != SQLITE_OK)
    goto rollback;
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  int changes = sqlite3_changes(db);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE || changes != 1) goto rollback;

  if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
    goto rollback;
  result = SIGNET_SECRET_OK;
  goto done;

rollback:
  (void)sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
done:
  sqlite3_mutex_leave(mutex);
  return result;
}

int signet_store_secret_history_count(SignetStore *store, const char *id) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !id) return -1;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db,
      "SELECT COUNT(*) FROM secret_versions WHERE id = ?;",
      -1, &stmt, NULL) != SQLITE_OK) return -1;
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  int count = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
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
