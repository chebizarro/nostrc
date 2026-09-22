#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "identity_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

static const char authority_schema[] =
  "BEGIN IMMEDIATE;"
  "CREATE TABLE metadata("
  " key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;"
  "CREATE TABLE accounts("
  " account_id TEXT PRIMARY KEY CHECK(length(account_id)=36),"
  " username TEXT UNIQUE NOT NULL CHECK(length(username) BETWEEN 3 AND 32),"
  " uid INTEGER UNIQUE NOT NULL CHECK(uid BETWEEN 1 AND 4294967295),"
  " gid INTEGER UNIQUE NOT NULL CHECK(gid BETWEEN 1 AND 4294967295),"
  " home TEXT UNIQUE NOT NULL CHECK(length(home) BETWEEN 1 AND 255),"
  " shell TEXT NOT NULL CHECK(length(shell) BETWEEN 1 AND 127),"
  " status TEXT NOT NULL CHECK(status IN"
  " ('enrolling','active','disabled','repair_required','retired')),"
  " origin TEXT NOT NULL CHECK(origin IN('enrolled','legacy_import')),"
  " projectable INTEGER NOT NULL DEFAULT 0 CHECK(projectable IN(0,1)),"
  " key_generation INTEGER NOT NULL DEFAULT 1 CHECK(key_generation>=1),"
  " created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
  "CREATE TABLE identities("
  " account_id TEXT UNIQUE NOT NULL REFERENCES accounts(account_id),"
  " pubkey_hex TEXT UNIQUE NOT NULL CHECK(length(pubkey_hex)=64));"
  "CREATE TABLE providers("
  " provider_id TEXT PRIMARY KEY CHECK(length(provider_id)=36),"
  " account_id TEXT NOT NULL REFERENCES accounts(account_id),"
  " type TEXT NOT NULL CHECK(type IN('local_encrypted_key','nip46_bunker','nip46_qr')),"
  " enabled INTEGER NOT NULL DEFAULT 0 CHECK(enabled IN(0,1)),"
  " format_version INTEGER NOT NULL CHECK(format_version>=1),"
  " public_config_json TEXT NOT NULL CHECK(length(public_config_json)<=4096),"
  " secret_blob BLOB NOT NULL CHECK(length(secret_blob)<=4096),"
  " created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
  "CREATE UNIQUE INDEX providers_one_enabled"
  " ON providers(account_id,type) WHERE enabled=1;"
  "CREATE UNIQUE INDEX providers_one_staged"
  " ON providers(account_id,type) WHERE enabled=0;"
  "CREATE TABLE operations("
  " operation_id TEXT PRIMARY KEY CHECK(length(operation_id)=36),"
  " account_id TEXT REFERENCES accounts(account_id),"
  " type TEXT NOT NULL CHECK(type IN('enroll','import','repair_home',"
  " 'provider_stage','provider_activate','provider_discard','set_status',"
  " 'replace_identity','provider_reseal')),"
  " phase TEXT NOT NULL CHECK(phase IN"
  " ('reserved','staged','installed','projected','complete')),"
  " outcome TEXT NOT NULL CHECK(outcome IN"
  " ('pending','done','repair_required','abandoned')),"
  " request_digest BLOB NOT NULL CHECK(length(request_digest)=32),"
  " result_id TEXT NOT NULL DEFAULT '',"
  " details_json TEXT NOT NULL DEFAULT '{}',"
  " reason_token TEXT NOT NULL DEFAULT '',"
  " home_mode TEXT NOT NULL DEFAULT 'create' CHECK(home_mode IN"
  " ('create','adopt_existing')),"
  " staged_dev INTEGER, staged_ino INTEGER,"
  " installed_dev INTEGER, installed_ino INTEGER,"
  " created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
  "CREATE TRIGGER accounts_immutable BEFORE UPDATE OF"
  " account_id,username,uid,gid,home,origin,created_at ON accounts"
  " BEGIN SELECT RAISE(ABORT,'immutable account field'); END;"
  "CREATE TRIGGER accounts_no_delete BEFORE DELETE ON accounts"
  " BEGIN SELECT RAISE(ABORT,'accounts cannot be deleted'); END;"
  "CREATE TRIGGER accounts_projectable_monotonic BEFORE UPDATE ON accounts"
  " WHEN NEW.projectable < OLD.projectable"
  " BEGIN SELECT RAISE(ABORT,'projectable cannot regress'); END;"
  "CREATE TRIGGER accounts_key_generation_monotonic BEFORE UPDATE ON accounts"
  " WHEN NEW.key_generation < OLD.key_generation"
  " BEGIN SELECT RAISE(ABORT,'key generation cannot regress'); END;"
  "CREATE TRIGGER accounts_retired_immutable BEFORE UPDATE ON accounts"
  " WHEN OLD.status='retired'"
  " BEGIN SELECT RAISE(ABORT,'retired account is immutable'); END;"
  "PRAGMA user_version=1;";

static int path_parent(const char *path, char *out, size_t out_size) {
  const char *slash;
  size_t len;
  if (!path || !out || out_size == 0 || path[0] != '/') return -1;
  slash = strrchr(path, '/');
  if (!slash) return -1;
  len = slash == path ? 1u : (size_t)(slash - path);
  if (len >= out_size) return -1;
  memcpy(out, path, len);
  out[len] = '\0';
  return 0;
}

static int secure_directory(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 0077) != 0)
    return -1;
  return 0;
}

static int secure_file(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 0077) != 0)
    return -1;
  return 0;
}

static nh_identity_rc exec_sql(nh_identity_store *store, const char *sql,
                               const char *operation) {
  char *error = NULL;
  int rc = sqlite3_exec(store->db, sql, NULL, NULL, &error);
  if (rc != SQLITE_OK) {
    nh_identity_set_error(store, "%s: %s", operation,
                          error ? error : sqlite3_errmsg(store->db));
    sqlite3_free(error);
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) return NH_IDENTITY_BUSY;
    if (rc == SQLITE_CONSTRAINT) return NH_IDENTITY_CONFLICT;
    return NH_IDENTITY_STORAGE_ERROR;
  }
  return NH_IDENTITY_OK;
}

nh_identity_rc nh_identity_sqlite_result(nh_identity_store *store, int rc,
                                         const char *operation) {
  if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW)
    return NH_IDENTITY_OK;
  nh_identity_set_error(store, "%s: %s", operation,
                        store && store->db ? sqlite3_errmsg(store->db)
                                           : sqlite3_errstr(rc));
  if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) return NH_IDENTITY_BUSY;
  if (rc == SQLITE_CONSTRAINT) return NH_IDENTITY_CONFLICT;
  if (rc == SQLITE_NOMEM) return NH_IDENTITY_NO_MEMORY;
  return NH_IDENTITY_STORAGE_ERROR;
}

nh_identity_rc nh_identity_begin(nh_identity_store *store) {
  return exec_sql(store, "BEGIN IMMEDIATE", "begin transaction");
}

nh_identity_rc nh_identity_commit(nh_identity_store *store) {
  return exec_sql(store, "COMMIT", "commit transaction");
}

void nh_identity_rollback(nh_identity_store *store) {
  if (store && store->db) (void)sqlite3_exec(store->db, "ROLLBACK", NULL, NULL, NULL);
}

static int uuid_generate(char out[NH_IDENTITY_UUID_CAP]) {
  static const char hex[] = "0123456789abcdef";
  unsigned char bytes[16];
  size_t i, position = 0;
  if (!out || RAND_bytes(bytes, (int)sizeof(bytes)) != 1) return -1;
  bytes[6] = (unsigned char)((bytes[6] & 0x0f) | 0x40);
  bytes[8] = (unsigned char)((bytes[8] & 0x3f) | 0x80);
  for (i = 0; i < sizeof(bytes); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[position++] = '-';
    out[position++] = hex[bytes[i] >> 4];
    out[position++] = hex[bytes[i] & 0x0f];
  }
  out[position] = '\0';
  return 0;
}

static nh_identity_rc schema_version(sqlite3 *db, uint32_t *out) {
  sqlite3_stmt *statement = NULL;
  int rc;
  if (!out || sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &statement,
                                 NULL) != SQLITE_OK)
    return NH_IDENTITY_STORAGE_ERROR;
  rc = sqlite3_step(statement);
  if (rc != SQLITE_ROW) { sqlite3_finalize(statement); return NH_IDENTITY_STORAGE_ERROR; }
  *out = (uint32_t)sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  return NH_IDENTITY_OK;
}

static nh_identity_rc insert_metadata(nh_identity_store *store) {
  sqlite3_stmt *statement = NULL;
  char authority_id[NH_IDENTITY_UUID_CAP];
  const char *sql = "INSERT INTO metadata(key,value) VALUES"
    "('authority_id',?),('authority_generation','1'),"
    "('projection_generation','0'),('created_at',CAST(strftime('%s','now') AS TEXT))";
  int rc;
  if (uuid_generate(authority_id) != 0) return NH_IDENTITY_STORAGE_ERROR;
  rc = sqlite3_prepare_v2(store->db, sql, -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, authority_id, -1, SQLITE_TRANSIENT);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  return nh_identity_sqlite_result(store, rc, "initialize metadata");
}

nh_identity_rc nh_identity_metadata_u64(nh_identity_store *store,
                                        const char *key, uint64_t *out) {
  sqlite3_stmt *statement = NULL;
  const unsigned char *text;
  char *end = NULL;
  unsigned long long value;
  int rc;
  if (!store || !store->db || !key || !out) return NH_IDENTITY_INVALID;
  rc = sqlite3_prepare_v2(store->db,
      "SELECT value FROM metadata WHERE key=?", -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, key, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(statement);
    return rc == SQLITE_DONE ? NH_IDENTITY_STORAGE_ERROR
                             : nh_identity_sqlite_result(store, rc, "read metadata");
  }
  text = sqlite3_column_text(statement, 0);
  if (!text || !*text || text[0] == '-') { sqlite3_finalize(statement); return NH_IDENTITY_STORAGE_ERROR; }
  errno = 0;
  value = strtoull((const char *)text, &end, 10);
  if (errno || !end || *end) { sqlite3_finalize(statement); return NH_IDENTITY_STORAGE_ERROR; }
  *out = (uint64_t)value;
  sqlite3_finalize(statement);
  return NH_IDENTITY_OK;
}

nh_identity_rc nh_identity_bump_generation(nh_identity_store *store,
                                           uint64_t *out) {
  nh_identity_rc result = exec_sql(store,
    "UPDATE metadata SET value=CAST(CAST(value AS INTEGER)+1 AS TEXT)"
    " WHERE key='authority_generation' AND CAST(value AS INTEGER)<9223372036854775807",
    "bump authority generation");
  if (result != NH_IDENTITY_OK) return result;
  if (sqlite3_changes(store->db) != 1) return NH_IDENTITY_STORAGE_ERROR;
  return out ? nh_identity_metadata_u64(store, "authority_generation", out)
             : NH_IDENTITY_OK;
}

nh_identity_rc nh_identity_store_open(const nh_identity_store_options *options,
                                      nh_identity_store **out) {
  nh_identity_store *store = NULL;
  char directory[NH_IDENTITY_HOME_CAP];
  struct stat st;
  bool created = false;
  uint32_t version = 0;
  int fd = -1, rc;
  nh_identity_rc result;
  if (!options || !options->config || !out) return NH_IDENTITY_INVALID;
  *out = NULL;
  if (nh_identity_config_validate(options->config) != NH_IDENTITY_OK)
    return NH_IDENTITY_INVALID;
  if (path_parent(options->config->authority_path, directory,
                  sizeof(directory)) != 0 || secure_directory(directory) != 0)
    return NH_IDENTITY_PERMISSION_DENIED;
  store = calloc(1, sizeof(*store));
  if (!store) return NH_IDENTITY_NO_MEMORY;
  store->lock_fd = -1;
  store->config = *options->config;
  store->ownership_probe = options->ownership_probe;
  store->ownership_probe_context = options->ownership_probe_context;
  rc = snprintf(store->lock_path, sizeof(store->lock_path), "%s/authority.lock",
                directory);
  if (rc < 0 || (size_t)rc >= sizeof(store->lock_path)) { result = NH_IDENTITY_INVALID; goto fail; }
  fd = open(store->lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) { result = errno == EACCES ? NH_IDENTITY_PERMISSION_DENIED : NH_IDENTITY_STORAGE_ERROR; goto fail; }
  if (fchmod(fd, 0600) != 0) { result = NH_IDENTITY_PERMISSION_DENIED; goto fail; }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) { result = NH_IDENTITY_BUSY; goto fail; }
  store->lock_fd = fd; fd = -1;

  if (lstat(store->config.authority_path, &st) != 0) {
    if (errno != ENOENT) { result = NH_IDENTITY_STORAGE_ERROR; goto fail; }
    if (!(options->flags & NH_IDENTITY_STORE_CREATE)) {
      result = NH_IDENTITY_NOT_INITIALIZED; goto fail;
    }
    fd = open(store->config.authority_path,
              O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { result = NH_IDENTITY_STORAGE_ERROR; goto fail; }
    if (fchmod(fd, 0600) != 0) {
      close(fd); fd = -1; result = NH_IDENTITY_STORAGE_ERROR; goto fail;
    }
    if (close(fd) != 0) { fd = -1; result = NH_IDENTITY_STORAGE_ERROR; goto fail; }
    fd = -1; created = true;
  } else if (secure_file(store->config.authority_path) != 0) {
    result = NH_IDENTITY_PERMISSION_DENIED; goto fail;
  }

  rc = sqlite3_open_v2(store->config.authority_path, &store->db,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL);
  if (rc != SQLITE_OK) { result = NH_IDENTITY_STORAGE_ERROR; goto fail; }
  sqlite3_busy_timeout(store->db, 5000);
  if (created) {
    result = exec_sql(store, authority_schema, "create authority schema");
    if (result != NH_IDENTITY_OK) goto fail;
    result = insert_metadata(store);
    if (result == NH_IDENTITY_OK)
      result = exec_sql(store, "COMMIT", "commit authority initialization");
    if (result != NH_IDENTITY_OK) {
      nh_identity_rollback(store);
      goto fail;
    }
  }
  result = schema_version(store->db, &version);
  if (result != NH_IDENTITY_OK) goto fail;
  if (version == 0) { result = NH_IDENTITY_NOT_INITIALIZED; goto fail; }
  if (version != NH_IDENTITY_AUTHORITY_SCHEMA_VERSION) {
    result = NH_IDENTITY_SCHEMA_UNSUPPORTED; goto fail;
  }
  result = exec_sql(store,
    "PRAGMA foreign_keys=ON;PRAGMA synchronous=FULL;PRAGMA journal_mode=WAL;",
    "configure authority database");
  if (result != NH_IDENTITY_OK) goto fail;
  {
    nh_identity_store_info info;
    result = nh_identity_store_get_info(store, &info);
    if (result != NH_IDENTITY_OK) goto fail;
  }
  *out = store;
  return NH_IDENTITY_OK;
fail:
  if (fd >= 0) close(fd);
  if (store) {
    if (store->db) sqlite3_close(store->db);
    if (store->lock_fd >= 0) close(store->lock_fd);
    if (created) unlink(store->config.authority_path);
    free(store);
  }
  return result;
}

void nh_identity_store_close(nh_identity_store *store) {
  if (!store) return;
  if (store->db) sqlite3_close(store->db);
  if (store->lock_fd >= 0) close(store->lock_fd);
  memset(store, 0, sizeof(*store));
  free(store);
}

nh_identity_rc nh_identity_store_get_info(nh_identity_store *store,
                                          nh_identity_store_info *out) {
  sqlite3_stmt *statement = NULL;
  const unsigned char *authority_id;
  uint32_t version;
  nh_identity_rc result;
  int rc;
  if (!store || !store->db || !out) return NH_IDENTITY_INVALID;
  memset(out, 0, sizeof(*out));
  result = schema_version(store->db, &version);
  if (result != NH_IDENTITY_OK) return result;
  rc = sqlite3_prepare_v2(store->db,
      "SELECT value FROM metadata WHERE key='authority_id'", -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  if (rc != SQLITE_ROW) { sqlite3_finalize(statement); return NH_IDENTITY_STORAGE_ERROR; }
  authority_id = sqlite3_column_text(statement, 0);
  if (!authority_id || !nh_identity_uuid_is_valid((const char *)authority_id)) {
    sqlite3_finalize(statement); return NH_IDENTITY_STORAGE_ERROR;
  }
  memcpy(out->authority_id, authority_id, NH_IDENTITY_UUID_CAP);
  sqlite3_finalize(statement);
  result = nh_identity_metadata_u64(store, "authority_generation",
                                    &out->authority_generation);
  if (result != NH_IDENTITY_OK) return result;
  result = nh_identity_metadata_u64(store, "projection_generation",
                                    &out->projection_generation);
  if (result != NH_IDENTITY_OK) return result;
  out->schema_version = version;
  return NH_IDENTITY_OK;
}

static const char account_select[] =
  "SELECT a.account_id,a.username,a.uid,a.gid,a.home,a.shell,a.status,a.origin,"
  "a.projectable,a.key_generation,COALESCE(i.pubkey_hex,''),"
  "COALESCE((SELECT SUM(CASE p.type WHEN 'local_encrypted_key' THEN 2"
  " WHEN 'nip46_bunker' THEN 4 WHEN 'nip46_qr' THEN 8 ELSE 0 END) FROM providers p"
  " WHERE p.account_id=a.account_id AND p.enabled=1),0)"
  " FROM accounts a LEFT JOIN identities i ON i.account_id=a.account_id ";

static int copy_column(sqlite3_stmt *statement, int column,
                       char *out, size_t capacity) {
  const unsigned char *value = sqlite3_column_text(statement, column);
  size_t len;
  if (!value || !out || capacity == 0) return -1;
  len = strlen((const char *)value);
  if (len >= capacity) return -1;
  memcpy(out, value, len + 1);
  return 0;
}

static nh_identity_rc fill_account(nh_identity_store *store,
                                   sqlite3_stmt *statement,
                                   nh_identity_account *out) {
  sqlite3_int64 uid, gid, key_generation, providers;
  nh_identity_rc result;
  if (!out) return NH_IDENTITY_INVALID;
  memset(out, 0, sizeof(*out));
  uid = sqlite3_column_int64(statement, 2);
  gid = sqlite3_column_int64(statement, 3);
  key_generation = sqlite3_column_int64(statement, 9);
  providers = sqlite3_column_int64(statement, 11);
  if (uid <= 0 || uid > UINT32_MAX || gid <= 0 || gid > UINT32_MAX ||
      key_generation < 1 || providers < 0 || providers > UINT32_MAX ||
      copy_column(statement, 0, out->account_id, sizeof(out->account_id)) ||
      copy_column(statement, 1, out->username, sizeof(out->username)) ||
      copy_column(statement, 4, out->home, sizeof(out->home)) ||
      copy_column(statement, 5, out->shell, sizeof(out->shell)) ||
      copy_column(statement, 10, out->pubkey_hex, sizeof(out->pubkey_hex)))
    return NH_IDENTITY_STORAGE_ERROR;
  if (!nh_identity_uuid_is_valid(out->account_id) ||
      !nh_identity_username_is_valid(out->username) ||
      (out->pubkey_hex[0] && !nh_identity_pubkey_is_valid(out->pubkey_hex)) ||
      nh_identity_status_parse((const char *)sqlite3_column_text(statement, 6),
                               &out->status) != NH_IDENTITY_OK ||
      nh_identity_origin_from_text((const char *)sqlite3_column_text(statement, 7),
                                   &out->origin) != 0)
    return NH_IDENTITY_STORAGE_ERROR;
  out->uid = (uint32_t)uid;
  out->gid = (uint32_t)gid;
  out->projectable = sqlite3_column_int(statement, 8) == 1;
  out->key_generation = (uint64_t)key_generation;
  out->enabled_providers = (uint32_t)providers;
  result = nh_identity_metadata_u64(store, "authority_generation",
                                    &out->authority_generation);
  return result;
}

static nh_identity_rc lookup_account(nh_identity_store *store,
    const char *where, int bind_kind, const char *text, uint32_t number,
    nh_identity_account *out) {
  sqlite3_stmt *statement = NULL;
  char sql[1024];
  int rc;
  nh_identity_rc result;
  if (!store || !store->db || !where || !out) return NH_IDENTITY_INVALID;
  rc = snprintf(sql, sizeof(sql), "%s WHERE %s", account_select, where);
  if (rc < 0 || (size_t)rc >= sizeof(sql)) return NH_IDENTITY_STORAGE_ERROR;
  rc = sqlite3_prepare_v2(store->db, sql, -1, &statement, NULL);
  if (rc == SQLITE_OK && bind_kind == 1)
    rc = sqlite3_bind_text(statement, 1, text, -1, SQLITE_STATIC);
  else if (rc == SQLITE_OK && bind_kind == 2)
    rc = sqlite3_bind_int64(statement, 1, (sqlite3_int64)number);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  if (rc == SQLITE_DONE) result = NH_IDENTITY_NOT_FOUND;
  else if (rc == SQLITE_ROW) result = fill_account(store, statement, out);
  else result = nh_identity_sqlite_result(store, rc, "lookup account");
  sqlite3_finalize(statement);
  return result;
}

nh_identity_rc nh_identity_store_lookup_by_name(nh_identity_store *store,
                                                const char *username,
                                                nh_identity_account *out) {
  if (!nh_identity_username_is_valid(username)) return NH_IDENTITY_NOT_FOUND;
  return lookup_account(store, "a.username=?", 1, username, 0, out);
}

nh_identity_rc nh_identity_store_lookup_by_uid(nh_identity_store *store,
                                               uint32_t uid,
                                               nh_identity_account *out) {
  return lookup_account(store, "a.uid=?", 2, NULL, uid, out);
}

nh_identity_rc nh_identity_store_lookup_by_id(nh_identity_store *store,
                                              const char *account_id,
                                              nh_identity_account *out) {
  if (!nh_identity_uuid_is_valid(account_id)) return NH_IDENTITY_INVALID;
  return lookup_account(store, "a.account_id=?", 1, account_id, 0, out);
}

nh_identity_rc nh_identity_store_lookup_by_pubkey(nh_identity_store *store,
                                                  const char *pubkey_hex,
                                                  nh_identity_account *out) {
  if (!nh_identity_pubkey_is_valid(pubkey_hex)) return NH_IDENTITY_INVALID;
  return lookup_account(store, "i.pubkey_hex=?", 1, pubkey_hex, 0, out);
}

nh_identity_rc nh_identity_store_recheck(
    nh_identity_store *store, const char *account_id,
    uint64_t expected_key_generation,
    uint64_t expected_authority_generation,
    nh_identity_status *status_out, nh_identity_account *current_out) {
  nh_identity_account current;
  nh_identity_rc result = nh_identity_store_lookup_by_id(store, account_id, &current);
  if (result != NH_IDENTITY_OK) return result;
  if (status_out) *status_out = current.status;
  if (current_out) *current_out = current;
  if (current.status != NH_IDENTITY_STATUS_ACTIVE) return NH_IDENTITY_NOT_ACTIVE;
  if (current.key_generation != expected_key_generation ||
      current.authority_generation != expected_authority_generation)
    return NH_IDENTITY_STALE_GENERATION;
  return NH_IDENTITY_OK;
}

nh_identity_rc nh_identity_store_provider_get(
    nh_identity_store *store, const char *account_id,
    nh_identity_provider_type type, bool enabled,
    nh_identity_provider_record *out) {
  sqlite3_stmt *statement = NULL;
  const char *type_text;
  const void *blob;
  int blob_len, rc;
  nh_identity_rc result = NH_IDENTITY_OK;
  if (!store || !out || !nh_identity_uuid_is_valid(account_id) ||
      nh_identity_provider_type_to_text(type, &type_text) != 0)
    return NH_IDENTITY_INVALID;
  rc = sqlite3_prepare_v2(store->db,
    "SELECT provider_id,account_id,type,enabled,format_version,"
    "public_config_json,secret_blob FROM providers"
    " WHERE account_id=? AND type=? AND enabled=?", -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, account_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, type_text, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(statement, 3, enabled ? 1 : 0);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  if (rc == SQLITE_DONE) result = NH_IDENTITY_NOT_FOUND;
  else if (rc != SQLITE_ROW) result = nh_identity_sqlite_result(store, rc, "get provider");
  else {
    memset(out, 0, sizeof(*out));
    blob = sqlite3_column_blob(statement, 6);
    blob_len = sqlite3_column_bytes(statement, 6);
    if (blob_len < 0 || (size_t)blob_len > sizeof(out->secret_blob) ||
        copy_column(statement, 0, out->provider_id, sizeof(out->provider_id)) ||
        copy_column(statement, 1, out->account_id, sizeof(out->account_id)) ||
        copy_column(statement, 5, out->public_config_json,
                    sizeof(out->public_config_json)) ||
        nh_identity_provider_type_from_text(
          (const char *)sqlite3_column_text(statement, 2), &out->type) != 0)
      result = NH_IDENTITY_STORAGE_ERROR;
    else {
      out->enabled = sqlite3_column_int(statement, 3) == 1;
      out->format_version = (uint32_t)sqlite3_column_int64(statement, 4);
      if (blob_len > 0) memcpy(out->secret_blob, blob, (size_t)blob_len);
      out->secret_blob_len = (size_t)blob_len;
    }
  }
  sqlite3_finalize(statement);
  return result;
}

struct digest_builder {
  unsigned char bytes[12288];
  size_t length;
};

static int digest_add(struct digest_builder *builder, const void *data,
                      size_t length) {
  unsigned char prefix[4];
  if (!builder || length > UINT32_MAX ||
      builder->length + sizeof(prefix) + length > sizeof(builder->bytes))
    return -1;
  prefix[0] = (unsigned char)(length >> 24);
  prefix[1] = (unsigned char)(length >> 16);
  prefix[2] = (unsigned char)(length >> 8);
  prefix[3] = (unsigned char)length;
  memcpy(builder->bytes + builder->length, prefix, sizeof(prefix));
  builder->length += sizeof(prefix);
  if (length) memcpy(builder->bytes + builder->length, data, length);
  builder->length += length;
  return 0;
}

static int digest_text(struct digest_builder *builder, const char *text) {
  return text ? digest_add(builder, text, strlen(text)) : -1;
}

static int digest_u64(struct digest_builder *builder, uint64_t value) {
  unsigned char bytes[8];
  size_t i;
  for (i = 0; i < sizeof(bytes); ++i)
    bytes[sizeof(bytes) - 1 - i] = (unsigned char)(value >> (i * 8));
  return digest_add(builder, bytes, sizeof(bytes));
}

static int digest_finish(struct digest_builder *builder,
                         unsigned char out[SHA256_DIGEST_LENGTH]) {
  return SHA256(builder->bytes, builder->length, out) ? 0 : -1;
}

static nh_identity_rc fill_operation(sqlite3_stmt *statement,
                                     nh_identity_operation_state *out) {
  const unsigned char *account_id;
  if (!out) return NH_IDENTITY_INVALID;
  memset(out, 0, sizeof(*out));
  if (copy_column(statement, 0, out->operation_id, sizeof(out->operation_id)) ||
      !nh_identity_uuid_is_valid(out->operation_id) ||
      nh_identity_operation_type_from_text(
        (const char *)sqlite3_column_text(statement, 2), &out->type) != 0 ||
      nh_identity_phase_from_text((const char *)sqlite3_column_text(statement, 3),
                                  &out->phase) != 0 ||
      nh_identity_outcome_from_text((const char *)sqlite3_column_text(statement, 4),
                                    &out->outcome) != 0 ||
      nh_identity_home_mode_from_text(
        (const char *)sqlite3_column_text(statement, 5), &out->home_mode) != 0)
    return NH_IDENTITY_STORAGE_ERROR;
  account_id = sqlite3_column_text(statement, 1);
  if (account_id) {
    size_t len = strlen((const char *)account_id);
    if (len >= sizeof(out->account_id) ||
        !nh_identity_uuid_is_valid((const char *)account_id))
      return NH_IDENTITY_STORAGE_ERROR;
    memcpy(out->account_id, account_id, len + 1);
  }
  if (sqlite3_column_type(statement, 6) != SQLITE_NULL)
    out->staged_home.filesystem_device =
      (uint64_t)sqlite3_column_int64(statement, 6);
  if (sqlite3_column_type(statement, 7) != SQLITE_NULL)
    out->staged_home.filesystem_inode =
      (uint64_t)sqlite3_column_int64(statement, 7);
  if (sqlite3_column_type(statement, 8) != SQLITE_NULL)
    out->installed_home.filesystem_device =
      (uint64_t)sqlite3_column_int64(statement, 8);
  if (sqlite3_column_type(statement, 9) != SQLITE_NULL)
    out->installed_home.filesystem_inode =
      (uint64_t)sqlite3_column_int64(statement, 9);
  return NH_IDENTITY_OK;
}

static nh_identity_rc operation_query(nh_identity_store *store,
                                      const char *operation_id,
                                      nh_identity_operation_state *out,
                                      unsigned char digest_out[32]) {
  sqlite3_stmt *statement = NULL;
  int rc;
  nh_identity_rc result;
  if (!store || !nh_identity_uuid_is_valid(operation_id) || !out)
    return NH_IDENTITY_INVALID;
  rc = sqlite3_prepare_v2(store->db,
    "SELECT operation_id,account_id,type,phase,outcome,home_mode,"
    "staged_dev,staged_ino,installed_dev,installed_ino,request_digest"
    " FROM operations WHERE operation_id=?", -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, operation_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  if (rc == SQLITE_DONE) result = NH_IDENTITY_NOT_FOUND;
  else if (rc != SQLITE_ROW) result = nh_identity_sqlite_result(store, rc, "get operation");
  else {
    result = fill_operation(statement, out);
    if (result == NH_IDENTITY_OK && digest_out) {
      const void *digest = sqlite3_column_blob(statement, 10);
      int length = sqlite3_column_bytes(statement, 10);
      if (!digest || length != 32) result = NH_IDENTITY_STORAGE_ERROR;
      else memcpy(digest_out, digest, 32);
    }
  }
  sqlite3_finalize(statement);
  return result;
}

nh_identity_rc nh_identity_operation_get(nh_identity_store *store,
                                         const char *operation_id,
                                         nh_identity_operation_state *out) {
  return operation_query(store, operation_id, out, NULL);
}

static nh_identity_rc operation_replay(nh_identity_store *store,
    const char *operation_id, nh_identity_operation_type expected_type,
    const unsigned char expected_digest[32], nh_identity_operation_state *out,
    bool *exists) {
  nh_identity_operation_state current;
  unsigned char actual_digest[32];
  nh_identity_rc result = operation_query(store, operation_id, &current,
                                          actual_digest);
  if (result == NH_IDENTITY_NOT_FOUND) { *exists = false; return NH_IDENTITY_OK; }
  if (result != NH_IDENTITY_OK) return result;
  *exists = true;
  if (current.type != expected_type ||
      memcmp(actual_digest, expected_digest, sizeof(actual_digest)) != 0)
    return NH_IDENTITY_OPERATION_MISMATCH;
  current.replayed = true;
  if (out) *out = current;
  return NH_IDENTITY_OK;
}

static nh_identity_rc insert_operation(nh_identity_store *store,
    const char *operation_id, const char *account_id,
    nh_identity_operation_type type, nh_identity_operation_phase phase,
    nh_identity_operation_outcome outcome, const unsigned char digest[32],
    nh_identity_home_mode home_mode) {
  sqlite3_stmt *statement = NULL;
  const char *type_text, *phase_text, *outcome_text, *home_mode_text;
  int rc;
  if (nh_identity_operation_type_to_text(type, &type_text) != 0 ||
      nh_identity_phase_to_text(phase, &phase_text) != 0 ||
      nh_identity_outcome_to_text(outcome, &outcome_text) != 0 ||
      nh_identity_home_mode_to_text(home_mode, &home_mode_text) != 0)
    return NH_IDENTITY_INVALID;
  rc = sqlite3_prepare_v2(store->db,
    "INSERT INTO operations(operation_id,account_id,type,phase,outcome,"
    "request_digest,details_json,reason_token,home_mode,created_at,updated_at)"
    " VALUES(?,?,?,?,?,?,'{}','',?,strftime('%s','now'),strftime('%s','now'))",
    -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, operation_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK && account_id)
    rc = sqlite3_bind_text(statement, 2, account_id, -1, SQLITE_STATIC);
  else if (rc == SQLITE_OK) rc = sqlite3_bind_null(statement, 2);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 3, type_text, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 4, phase_text, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 5, outcome_text, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_blob(statement, 6, digest, 32, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 7, home_mode_text, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  return nh_identity_sqlite_result(store, rc, "insert operation");
}

static nh_identity_rc find_available_id(nh_identity_store *store,
                                        const char *username,
                                        uint32_t *out) {
  sqlite3_stmt *statement = NULL;
  uint64_t candidate;
  int rc;
  if (!store || !username || !out || !store->ownership_probe)
    return NH_IDENTITY_OWNERSHIP_CHECK_FAILED;
  rc = sqlite3_prepare_v2(store->db,
    "SELECT 1 FROM accounts WHERE uid=? OR gid=? LIMIT 1", -1, &statement, NULL);
  if (rc != SQLITE_OK) return nh_identity_sqlite_result(store, rc, "prepare allocation");
  for (candidate = store->config.uid_min; candidate <= store->config.uid_max;
       ++candidate) {
    nh_identity_ownership_result ownership;
    sqlite3_reset(statement); sqlite3_clear_bindings(statement);
    rc = sqlite3_bind_int64(statement, 1, (sqlite3_int64)candidate);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 2, (sqlite3_int64)candidate);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW) continue;
    if (rc != SQLITE_DONE) { sqlite3_finalize(statement); return nh_identity_sqlite_result(store, rc, "scan allocation"); }
    ownership = store->ownership_probe(store->ownership_probe_context, username,
                                       (uint32_t)candidate, (uint32_t)candidate);
    if (ownership == NH_IDENTITY_OWNERSHIP_ERROR) {
      sqlite3_finalize(statement); return NH_IDENTITY_OWNERSHIP_CHECK_FAILED;
    }
    if (ownership == NH_IDENTITY_OWNERSHIP_FREE) {
      sqlite3_finalize(statement); *out = (uint32_t)candidate; return NH_IDENTITY_OK;
    }
    if (candidate == UINT32_MAX) break;
  }
  sqlite3_finalize(statement);
  return NH_IDENTITY_RANGE_EXHAUSTED;
}

static nh_identity_rc insert_account(nh_identity_store *store,
    const char *account_id, const char *username, uint32_t uid, uint32_t gid,
    const char *home, const char *shell, nh_identity_origin origin,
    const char *pubkey_hex) {
  sqlite3_stmt *statement = NULL;
  const char *origin_text;
  int rc;
  if (nh_identity_origin_to_text(origin, &origin_text) != 0)
    return NH_IDENTITY_INVALID;
  rc = sqlite3_prepare_v2(store->db,
    "INSERT INTO accounts(account_id,username,uid,gid,home,shell,status,origin,"
    "projectable,key_generation,created_at,updated_at)"
    " VALUES(?,?,?,?,?,?,'enrolling',?,0,1,strftime('%s','now'),strftime('%s','now'))",
    -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, account_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, username, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 3, uid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 4, gid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 5, home, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 6, shell, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 7, origin_text, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (rc != SQLITE_DONE) return nh_identity_sqlite_result(store, rc, "insert account");
  rc = sqlite3_prepare_v2(store->db,
    "INSERT INTO identities(account_id,pubkey_hex) VALUES(?,?)", -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, account_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, pubkey_hex, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  return nh_identity_sqlite_result(store, rc, "insert identity");
}

nh_identity_rc nh_identity_operation_begin_enroll(
    nh_identity_store *store, const char *operation_id,
    const nh_identity_enroll_request *request,
    nh_identity_operation_state *out) {
  struct digest_builder builder = {{0}, 0};
  unsigned char digest[32];
  char home[NH_IDENTITY_HOME_CAP], account_id[NH_IDENTITY_UUID_CAP];
  const char *shell, *home_mode;
  uint32_t allocated = 0;
  bool exists = false;
  int written;
  nh_identity_rc result;
  if (!store || !request || !out || !nh_identity_uuid_is_valid(operation_id) ||
      !nh_identity_username_is_valid(request->username) ||
      !nh_identity_pubkey_is_valid(request->pubkey_hex) ||
      nh_identity_home_mode_to_text(request->home_mode, &home_mode) != 0)
    return NH_IDENTITY_INVALID;
  shell = request->shell ? request->shell : store->config.default_shell;
  if (shell[0] != '/' || strlen(shell) > NH_IDENTITY_SHELL_MAX ||
      strchr(shell, ':') || strchr(shell, '\n')) return NH_IDENTITY_INVALID;
  written = snprintf(home, sizeof(home), "%s/%s", store->config.home_root,
                     request->username);
  if (written < 0 || (size_t)written >= sizeof(home)) return NH_IDENTITY_INVALID;
  if (digest_text(&builder, "enroll") || digest_text(&builder, request->username) ||
      digest_text(&builder, request->pubkey_hex) || digest_text(&builder, shell) ||
      digest_text(&builder, home_mode) || digest_finish(&builder, digest))
    return NH_IDENTITY_INVALID;
  result = operation_replay(store, operation_id, NH_IDENTITY_OPERATION_ENROLL,
                            digest, out, &exists);
  if (result != NH_IDENTITY_OK || exists) return result;
  result = find_available_id(store, request->username, &allocated);
  if (result != NH_IDENTITY_OK) return result;
  if (uuid_generate(account_id) != 0) return NH_IDENTITY_STORAGE_ERROR;
  result = nh_identity_begin(store);
  if (result != NH_IDENTITY_OK) return result;
  result = insert_account(store, account_id, request->username, allocated,
                          allocated, home, shell, NH_IDENTITY_ORIGIN_ENROLLED,
                          request->pubkey_hex);
  if (result == NH_IDENTITY_OK)
    result = insert_operation(store, operation_id, account_id,
      NH_IDENTITY_OPERATION_ENROLL, NH_IDENTITY_PHASE_RESERVED,
      NH_IDENTITY_OUTCOME_PENDING, digest, request->home_mode);
  if (result == NH_IDENTITY_OK) result = nh_identity_bump_generation(store, NULL);
  if (result == NH_IDENTITY_OK) result = nh_identity_commit(store);
  if (result != NH_IDENTITY_OK) { nh_identity_rollback(store); return result; }
  result = nh_identity_operation_get(store, operation_id, out);
  if (result == NH_IDENTITY_OK) out->replayed = false;
  return result;
}

nh_identity_rc nh_identity_operation_next_pending(
    nh_identity_store *store, const char *after_operation_id,
    nh_identity_operation_state *out) {
  sqlite3_stmt *statement = NULL;
  int rc;
  nh_identity_rc result;
  if (!store || !out || (after_operation_id &&
      !nh_identity_uuid_is_valid(after_operation_id))) return NH_IDENTITY_INVALID;
  rc = sqlite3_prepare_v2(store->db,
    after_operation_id
      ? "SELECT operation_id,account_id,type,phase,outcome,home_mode,staged_dev,"
        "staged_ino,installed_dev,installed_ino FROM operations"
        " WHERE outcome='pending' AND operation_id>? ORDER BY operation_id LIMIT 1"
      : "SELECT operation_id,account_id,type,phase,outcome,home_mode,staged_dev,"
        "staged_ino,installed_dev,installed_ino FROM operations"
        " WHERE outcome='pending' ORDER BY operation_id LIMIT 1",
    -1, &statement, NULL);
  if (rc == SQLITE_OK && after_operation_id)
    rc = sqlite3_bind_text(statement, 1, after_operation_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  if (rc == SQLITE_DONE) result = NH_IDENTITY_NOT_FOUND;
  else if (rc == SQLITE_ROW) result = fill_operation(statement, out);
  else result = nh_identity_sqlite_result(store, rc, "next pending operation");
  sqlite3_finalize(statement);
  return result;
}

nh_identity_rc nh_identity_operation_advance_home(
    nh_identity_store *store, const char *operation_id,
    nh_identity_operation_phase expected_phase,
    nh_identity_operation_phase next_phase,
    const nh_identity_home_evidence *evidence,
    nh_identity_operation_state *out) {
  nh_identity_operation_state current;
  sqlite3_stmt *statement = NULL;
  const char *next_text;
  int rc;
  nh_identity_rc result;
  if (!store || !evidence || !out || !nh_identity_uuid_is_valid(operation_id) ||
      evidence->filesystem_device == 0 || evidence->filesystem_inode == 0 ||
      !((expected_phase == NH_IDENTITY_PHASE_RESERVED &&
         next_phase == NH_IDENTITY_PHASE_STAGED) ||
        (expected_phase == NH_IDENTITY_PHASE_STAGED &&
         next_phase == NH_IDENTITY_PHASE_INSTALLED)) ||
      nh_identity_phase_to_text(next_phase, &next_text) != 0)
    return NH_IDENTITY_INVALID;
  result = nh_identity_operation_get(store, operation_id, &current);
  if (result != NH_IDENTITY_OK) return result;
  if (current.phase == next_phase) {
    const nh_identity_home_evidence *recorded =
      next_phase == NH_IDENTITY_PHASE_STAGED ? &current.staged_home
                                             : &current.installed_home;
    if (recorded->filesystem_device == evidence->filesystem_device &&
        recorded->filesystem_inode == evidence->filesystem_inode) {
      current.replayed = true; *out = current; return NH_IDENTITY_OK;
    }
    return NH_IDENTITY_BAD_STATE;
  }
  if (current.phase != expected_phase || current.outcome != NH_IDENTITY_OUTCOME_PENDING)
    return NH_IDENTITY_BAD_STATE;
  result = nh_identity_begin(store);
  if (result != NH_IDENTITY_OK) return result;
  rc = sqlite3_prepare_v2(store->db,
    next_phase == NH_IDENTITY_PHASE_STAGED
      ? "UPDATE operations SET phase=?,staged_dev=?,staged_ino=?,updated_at=strftime('%s','now')"
        " WHERE operation_id=? AND phase='reserved' AND outcome='pending'"
      : "UPDATE operations SET phase=?,installed_dev=?,installed_ino=?,updated_at=strftime('%s','now')"
        " WHERE operation_id=? AND phase='staged' AND outcome='pending'",
    -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, next_text, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 2, (sqlite3_int64)evidence->filesystem_device);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 3, (sqlite3_int64)evidence->filesystem_inode);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 4, operation_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  result = nh_identity_sqlite_result(store, rc, "advance home operation");
  if (result == NH_IDENTITY_OK && sqlite3_changes(store->db) != 1)
    result = NH_IDENTITY_BAD_STATE;
  if (result == NH_IDENTITY_OK && next_phase == NH_IDENTITY_PHASE_INSTALLED) {
    rc = sqlite3_prepare_v2(store->db,
      "UPDATE accounts SET projectable=1,updated_at=strftime('%s','now')"
      " WHERE account_id=(SELECT account_id FROM operations WHERE operation_id=?)",
      -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, operation_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    result = nh_identity_sqlite_result(store, rc, "mark account projectable");
  }
  if (result == NH_IDENTITY_OK) result = nh_identity_bump_generation(store, NULL);
  if (result == NH_IDENTITY_OK) result = nh_identity_commit(store);
  if (result != NH_IDENTITY_OK) { nh_identity_rollback(store); return result; }
  result = nh_identity_operation_get(store, operation_id, out);
  if (result == NH_IDENTITY_OK) out->replayed = false;
  return result;
}

nh_identity_rc nh_identity_operation_activate(
    nh_identity_store *store, const char *operation_id,
    nh_identity_operation_state *out) {
  nh_identity_operation_state current;
  nh_identity_account account;
  sqlite3_stmt *statement = NULL;
  int rc;
  nh_identity_rc result;
  if (!store || !out || !nh_identity_uuid_is_valid(operation_id))
    return NH_IDENTITY_INVALID;
  result = nh_identity_operation_get(store, operation_id, &current);
  if (result != NH_IDENTITY_OK) return result;
  if (current.phase == NH_IDENTITY_PHASE_COMPLETE &&
      current.outcome == NH_IDENTITY_OUTCOME_DONE) {
    current.replayed = true; *out = current; return NH_IDENTITY_OK;
  }
  if (current.phase != NH_IDENTITY_PHASE_PROJECTED ||
      current.outcome != NH_IDENTITY_OUTCOME_PENDING)
    return NH_IDENTITY_BAD_STATE;
  result = nh_identity_store_lookup_by_id(store, current.account_id, &account);
  if (result != NH_IDENTITY_OK) return result;
  if (!account.projectable || account.enabled_providers == 0)
    return NH_IDENTITY_BAD_STATE;
  result = nh_identity_begin(store);
  if (result != NH_IDENTITY_OK) return result;
  rc = sqlite3_prepare_v2(store->db,
    "UPDATE accounts SET status='active',updated_at=strftime('%s','now')"
    " WHERE account_id=? AND status IN('enrolling','disabled')", -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, current.account_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  result = nh_identity_sqlite_result(store, rc, "activate account");
  if (result == NH_IDENTITY_OK && sqlite3_changes(store->db) != 1)
    result = NH_IDENTITY_BAD_STATE;
  if (result == NH_IDENTITY_OK) {
    rc = sqlite3_prepare_v2(store->db,
      "UPDATE operations SET phase='complete',outcome='done',updated_at=strftime('%s','now')"
      " WHERE operation_id=?", -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, operation_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    result = nh_identity_sqlite_result(store, rc, "complete operation target");
  }
  if (result == NH_IDENTITY_OK) result = nh_identity_bump_generation(store, NULL);
  if (result == NH_IDENTITY_OK) result = nh_identity_commit(store);
  if (result != NH_IDENTITY_OK) { nh_identity_rollback(store); return result; }
  result = nh_identity_operation_get(store, operation_id, out);
  if (result == NH_IDENTITY_OK) out->replayed = false;
  return result;
}

nh_identity_rc nh_identity_operation_fail(
    nh_identity_store *store, const char *operation_id,
    nh_identity_operation_outcome outcome, const char *reason_token,
    nh_identity_operation_state *out) {
  nh_identity_operation_state current;
  sqlite3_stmt *statement = NULL;
  const char *outcome_text, *status_text;
  int rc;
  nh_identity_rc result;
  if (!store || !out || !reason_token || strlen(reason_token) > 64 ||
      strchr(reason_token, '\n') || !nh_identity_uuid_is_valid(operation_id) ||
      (outcome != NH_IDENTITY_OUTCOME_REPAIR_REQUIRED &&
       outcome != NH_IDENTITY_OUTCOME_ABANDONED) ||
      nh_identity_outcome_to_text(outcome, &outcome_text) != 0)
    return NH_IDENTITY_INVALID;
  result = nh_identity_operation_get(store, operation_id, &current);
  if (result != NH_IDENTITY_OK) return result;
  if (current.outcome == outcome) { current.replayed = true; *out = current; return NH_IDENTITY_OK; }
  if (current.outcome != NH_IDENTITY_OUTCOME_PENDING ||
      (outcome == NH_IDENTITY_OUTCOME_ABANDONED &&
       current.phase != NH_IDENTITY_PHASE_RESERVED)) return NH_IDENTITY_BAD_STATE;
  status_text = outcome == NH_IDENTITY_OUTCOME_ABANDONED ? "retired" : "repair_required";
  result = nh_identity_begin(store);
  if (result != NH_IDENTITY_OK) return result;
  rc = sqlite3_prepare_v2(store->db,
    "UPDATE operations SET outcome=?,reason_token=?,updated_at=strftime('%s','now')"
    " WHERE operation_id=? AND outcome='pending'", -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, outcome_text, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, reason_token, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 3, operation_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  result = nh_identity_sqlite_result(store, rc, "fail operation");
  if (result == NH_IDENTITY_OK) {
    rc = sqlite3_prepare_v2(store->db,
      "UPDATE accounts SET status=?,updated_at=strftime('%s','now')"
      " WHERE account_id=?", -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, status_text, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, current.account_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    result = nh_identity_sqlite_result(store, rc, "mark failed account");
  }
  if (result == NH_IDENTITY_OK && outcome == NH_IDENTITY_OUTCOME_ABANDONED) {
    rc = sqlite3_prepare_v2(store->db, "DELETE FROM providers WHERE account_id=?", -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, current.account_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    result = nh_identity_sqlite_result(store, rc, "remove abandoned providers");
    if (result == NH_IDENTITY_OK) {
      rc = sqlite3_prepare_v2(store->db, "DELETE FROM identities WHERE account_id=?", -1, &statement, NULL);
      if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, current.account_id, -1, SQLITE_STATIC);
      if (rc == SQLITE_OK) rc = sqlite3_step(statement);
      sqlite3_finalize(statement);
      result = nh_identity_sqlite_result(store, rc, "remove abandoned identity");
    }
  }
  if (result == NH_IDENTITY_OK) result = nh_identity_bump_generation(store, NULL);
  if (result == NH_IDENTITY_OK) result = nh_identity_commit(store);
  if (result != NH_IDENTITY_OK) { nh_identity_rollback(store); return result; }
  result = nh_identity_operation_get(store, operation_id, out);
  if (result == NH_IDENTITY_OK) out->replayed = false;
  return result;
}

static int status_transition_allowed(nh_identity_status from,
                                     nh_identity_status to) {
  if (from == NH_IDENTITY_STATUS_ACTIVE && to == NH_IDENTITY_STATUS_DISABLED) return 1;
  if (from == NH_IDENTITY_STATUS_DISABLED && to == NH_IDENTITY_STATUS_ACTIVE) return 1;
  if ((from == NH_IDENTITY_STATUS_ACTIVE || from == NH_IDENTITY_STATUS_DISABLED) &&
      to == NH_IDENTITY_STATUS_REPAIR_REQUIRED) return 1;
  if ((from == NH_IDENTITY_STATUS_ACTIVE || from == NH_IDENTITY_STATUS_DISABLED ||
       from == NH_IDENTITY_STATUS_REPAIR_REQUIRED) &&
      to == NH_IDENTITY_STATUS_RETIRED) return 1;
  return 0;
}

nh_identity_rc nh_identity_account_set_status(
    nh_identity_store *store, const char *operation_id,
    const char *account_id, nh_identity_status target) {
  struct digest_builder builder = {{0}, 0};
  unsigned char digest[32];
  nh_identity_operation_state replay;
  nh_identity_account account;
  const char *target_text;
  bool exists = false;
  sqlite3_stmt *statement = NULL;
  int rc;
  nh_identity_rc result;
  if (!store || !nh_identity_uuid_is_valid(operation_id) ||
      !nh_identity_uuid_is_valid(account_id) ||
      nh_identity_status_to_text(target, &target_text) != 0)
    return NH_IDENTITY_INVALID;
  if (digest_text(&builder, "set_status") || digest_text(&builder, account_id) ||
      digest_text(&builder, target_text) || digest_finish(&builder, digest))
    return NH_IDENTITY_INVALID;
  result = operation_replay(store, operation_id, NH_IDENTITY_OPERATION_SET_STATUS,
                            digest, &replay, &exists);
  if (result != NH_IDENTITY_OK || exists) return result;
  result = nh_identity_store_lookup_by_id(store, account_id, &account);
  if (result != NH_IDENTITY_OK) return result;
  if (!status_transition_allowed(account.status, target)) return NH_IDENTITY_BAD_STATE;
  if (target == NH_IDENTITY_STATUS_ACTIVE &&
      (!account.projectable || account.enabled_providers == 0))
    return NH_IDENTITY_BAD_STATE;
  result = nh_identity_begin(store);
  if (result != NH_IDENTITY_OK) return result;
  rc = sqlite3_prepare_v2(store->db,
    "UPDATE accounts SET status=?,updated_at=strftime('%s','now') WHERE account_id=?",
    -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, target_text, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, account_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  result = nh_identity_sqlite_result(store, rc, "set account status");
  if (result == NH_IDENTITY_OK && target == NH_IDENTITY_STATUS_RETIRED) {
    rc = sqlite3_prepare_v2(store->db, "DELETE FROM providers WHERE account_id=?", -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, account_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    result = nh_identity_sqlite_result(store, rc, "retire providers");
    if (result == NH_IDENTITY_OK) {
      rc = sqlite3_prepare_v2(store->db, "DELETE FROM identities WHERE account_id=?", -1, &statement, NULL);
      if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, account_id, -1, SQLITE_STATIC);
      if (rc == SQLITE_OK) rc = sqlite3_step(statement);
      sqlite3_finalize(statement);
      result = nh_identity_sqlite_result(store, rc, "retire identity");
    }
  }
  if (result == NH_IDENTITY_OK)
    result = insert_operation(store, operation_id, account_id,
      NH_IDENTITY_OPERATION_SET_STATUS, NH_IDENTITY_PHASE_COMPLETE,
      NH_IDENTITY_OUTCOME_DONE, digest, NH_IDENTITY_HOME_CREATE);
  if (result == NH_IDENTITY_OK) result = nh_identity_bump_generation(store, NULL);
  if (result == NH_IDENTITY_OK) result = nh_identity_commit(store);
  if (result != NH_IDENTITY_OK) nh_identity_rollback(store);
  return result;
}

nh_identity_rc nh_identity_account_replace_identity(
    nh_identity_store *store, const char *operation_id,
    const char *account_id, const char *new_pubkey_hex,
    bool administrator_acknowledged) {
  struct digest_builder builder = {{0}, 0};
  unsigned char digest[32];
  nh_identity_operation_state replay;
  nh_identity_account account;
  bool exists = false;
  sqlite3_stmt *statement = NULL;
  int rc;
  nh_identity_rc result;
  if (!store || !administrator_acknowledged ||
      !nh_identity_uuid_is_valid(operation_id) ||
      !nh_identity_uuid_is_valid(account_id) ||
      !nh_identity_pubkey_is_valid(new_pubkey_hex)) return NH_IDENTITY_INVALID;
  if (digest_text(&builder, "replace_identity") || digest_text(&builder, account_id) ||
      digest_text(&builder, new_pubkey_hex) || digest_finish(&builder, digest))
    return NH_IDENTITY_INVALID;
  result = operation_replay(store, operation_id,
    NH_IDENTITY_OPERATION_REPLACE_IDENTITY, digest, &replay, &exists);
  if (result != NH_IDENTITY_OK || exists) return result;
  result = nh_identity_store_lookup_by_id(store, account_id, &account);
  if (result != NH_IDENTITY_OK) return result;
  if (account.status == NH_IDENTITY_STATUS_RETIRED) return NH_IDENTITY_BAD_STATE;
  result = nh_identity_begin(store);
  if (result != NH_IDENTITY_OK) return result;
  rc = sqlite3_prepare_v2(store->db,
    "UPDATE identities SET pubkey_hex=? WHERE account_id=?", -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, new_pubkey_hex, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, account_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  result = nh_identity_sqlite_result(store, rc, "replace identity");
  if (result == NH_IDENTITY_OK && sqlite3_changes(store->db) != 1)
    result = NH_IDENTITY_STORAGE_ERROR;
  if (result == NH_IDENTITY_OK) {
    rc = sqlite3_prepare_v2(store->db,
      "DELETE FROM providers WHERE account_id=?", -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, account_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    result = nh_identity_sqlite_result(store, rc, "clear providers after replacement");
  }
  if (result == NH_IDENTITY_OK) {
    rc = sqlite3_prepare_v2(store->db,
      "UPDATE accounts SET key_generation=key_generation+1,status='disabled',"
      "updated_at=strftime('%s','now') WHERE account_id=?", -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, account_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    result = nh_identity_sqlite_result(store, rc, "advance key generation");
  }
  if (result == NH_IDENTITY_OK)
    result = insert_operation(store, operation_id, account_id,
      NH_IDENTITY_OPERATION_REPLACE_IDENTITY, NH_IDENTITY_PHASE_COMPLETE,
      NH_IDENTITY_OUTCOME_DONE, digest, NH_IDENTITY_HOME_CREATE);
  if (result == NH_IDENTITY_OK) result = nh_identity_bump_generation(store, NULL);
  if (result == NH_IDENTITY_OK) result = nh_identity_commit(store);
  if (result != NH_IDENTITY_OK) nh_identity_rollback(store);
  return result;
}

nh_identity_rc nh_identity_provider_stage(
    nh_identity_store *store, const char *operation_id,
    const char *account_id, nh_identity_provider_type type,
    uint32_t format_version, const char *public_config_json,
    const uint8_t *secret_blob, size_t secret_blob_len,
    char provider_id_out[NH_IDENTITY_UUID_CAP]) {
  struct digest_builder builder = {{0}, 0};
  unsigned char digest[32];
  nh_identity_operation_state replay;
  nh_identity_account account;
  const char *type_text;
  char provider_id[NH_IDENTITY_UUID_CAP];
  bool exists = false;
  sqlite3_stmt *statement = NULL;
  int rc;
  nh_identity_rc result;
  if (!store || !provider_id_out || !nh_identity_uuid_is_valid(operation_id) ||
      !nh_identity_uuid_is_valid(account_id) || format_version == 0 ||
      !public_config_json || strlen(public_config_json) > NH_IDENTITY_PROVIDER_CONFIG_MAX ||
      !secret_blob || secret_blob_len == 0 ||
      secret_blob_len > NH_IDENTITY_PROVIDER_SECRET_MAX ||
      nh_identity_provider_type_to_text(type, &type_text) != 0)
    return NH_IDENTITY_INVALID;
  if (digest_text(&builder, "provider_stage") || digest_text(&builder, account_id) ||
      digest_text(&builder, type_text) ||
      digest_u64(&builder, format_version) ||
      digest_text(&builder, public_config_json) ||
      digest_add(&builder, secret_blob, secret_blob_len) ||
      digest_finish(&builder, digest)) return NH_IDENTITY_INVALID;
  result = operation_replay(store, operation_id,
    NH_IDENTITY_OPERATION_PROVIDER_STAGE, digest, &replay, &exists);
  if (result != NH_IDENTITY_OK) return result;
  if (exists) {
    sqlite3_stmt *replay_statement = NULL;
    rc = sqlite3_prepare_v2(store->db,
      "SELECT result_id FROM operations WHERE operation_id=?",
      -1, &replay_statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(replay_statement, 1, operation_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(replay_statement);
    if (rc != SQLITE_ROW || copy_column(replay_statement, 0, provider_id_out,
                                         NH_IDENTITY_UUID_CAP))
      result = NH_IDENTITY_STORAGE_ERROR;
    sqlite3_finalize(replay_statement);
    return result;
  }
  result = nh_identity_store_lookup_by_id(store, account_id, &account);
  if (result != NH_IDENTITY_OK) return result;
  if (account.status == NH_IDENTITY_STATUS_RETIRED ||
      account.status == NH_IDENTITY_STATUS_REPAIR_REQUIRED)
    return NH_IDENTITY_BAD_STATE;
  if (uuid_generate(provider_id) != 0) return NH_IDENTITY_STORAGE_ERROR;
  result = nh_identity_begin(store);
  if (result != NH_IDENTITY_OK) return result;
  rc = sqlite3_prepare_v2(store->db,
    "INSERT INTO providers(provider_id,account_id,type,enabled,format_version,"
    "public_config_json,secret_blob,created_at,updated_at)"
    " VALUES(?,?,?,0,?,?,?,strftime('%s','now'),strftime('%s','now'))",
    -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, provider_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, account_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 3, type_text, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 4, format_version);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 5, public_config_json, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_blob(statement, 6, secret_blob,
                                               (int)secret_blob_len, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  result = nh_identity_sqlite_result(store, rc, "stage provider");
  if (result == NH_IDENTITY_OK)
    result = insert_operation(store, operation_id, account_id,
      NH_IDENTITY_OPERATION_PROVIDER_STAGE, NH_IDENTITY_PHASE_COMPLETE,
      NH_IDENTITY_OUTCOME_DONE, digest, NH_IDENTITY_HOME_CREATE);
  if (result == NH_IDENTITY_OK) {
    rc = sqlite3_prepare_v2(store->db,
      "UPDATE operations SET result_id=? WHERE operation_id=?", -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, provider_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, operation_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    sqlite3_finalize(statement); statement = NULL;
    result = nh_identity_sqlite_result(store, rc, "record staged provider result");
  }
  if (result == NH_IDENTITY_OK) result = nh_identity_bump_generation(store, NULL);
  if (result == NH_IDENTITY_OK) result = nh_identity_commit(store);
  if (result != NH_IDENTITY_OK) { nh_identity_rollback(store); return result; }
  memcpy(provider_id_out, provider_id, sizeof(provider_id));
  return NH_IDENTITY_OK;
}

nh_identity_rc nh_identity_provider_activate(
    nh_identity_store *store, const char *operation_id,
    const char *provider_id,
    const nh_identity_proof_attestation *attestation) {
  struct digest_builder builder = {{0}, 0};
  unsigned char digest[32];
  nh_identity_operation_state replay;
  nh_identity_account account;
  char account_id[NH_IDENTITY_UUID_CAP], type_text[32];
  bool exists = false;
  sqlite3_stmt *statement = NULL;
  int rc;
  nh_identity_rc result;
  if (!store || !attestation || !nh_identity_uuid_is_valid(operation_id) ||
      !nh_identity_uuid_is_valid(provider_id) ||
      !nh_identity_pubkey_is_valid(attestation->pubkey_hex) ||
      attestation->key_generation == 0) return NH_IDENTITY_INVALID;
  if (digest_text(&builder, "provider_activate") || digest_text(&builder, provider_id) ||
      digest_add(&builder, attestation->proof_event_id,
                 sizeof(attestation->proof_event_id)) ||
      digest_text(&builder, attestation->pubkey_hex) ||
      digest_u64(&builder, attestation->key_generation) ||
      digest_finish(&builder, digest)) return NH_IDENTITY_INVALID;
  result = operation_replay(store, operation_id,
    NH_IDENTITY_OPERATION_PROVIDER_ACTIVATE, digest, &replay, &exists);
  if (result != NH_IDENTITY_OK || exists) return result;
  rc = sqlite3_prepare_v2(store->db,
    "SELECT account_id,type FROM providers WHERE provider_id=? AND enabled=0",
    -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, provider_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  if (rc == SQLITE_DONE) result = NH_IDENTITY_NOT_FOUND;
  else if (rc != SQLITE_ROW ||
           copy_column(statement, 0, account_id, sizeof(account_id)) ||
           copy_column(statement, 1, type_text, sizeof(type_text)))
    result = rc == SQLITE_ROW ? NH_IDENTITY_STORAGE_ERROR
                              : nh_identity_sqlite_result(store, rc, "find staged provider");
  sqlite3_finalize(statement);
  if (result != NH_IDENTITY_OK) return result;
  result = nh_identity_store_lookup_by_id(store, account_id, &account);
  if (result != NH_IDENTITY_OK) return result;
  if (account.status == NH_IDENTITY_STATUS_RETIRED ||
      account.status == NH_IDENTITY_STATUS_REPAIR_REQUIRED ||
      account.key_generation != attestation->key_generation ||
      strcmp(account.pubkey_hex, attestation->pubkey_hex) != 0)
    return NH_IDENTITY_STALE_GENERATION;
  result = nh_identity_begin(store);
  if (result != NH_IDENTITY_OK) return result;
  rc = sqlite3_prepare_v2(store->db,
    "DELETE FROM providers WHERE account_id=? AND type=? AND enabled=1",
    -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, account_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, type_text, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  result = nh_identity_sqlite_result(store, rc, "replace enabled provider");
  if (result == NH_IDENTITY_OK) {
    rc = sqlite3_prepare_v2(store->db,
      "UPDATE providers SET enabled=1,updated_at=strftime('%s','now')"
      " WHERE provider_id=? AND enabled=0", -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, provider_id, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    result = nh_identity_sqlite_result(store, rc, "activate provider");
    if (result == NH_IDENTITY_OK && sqlite3_changes(store->db) != 1)
      result = NH_IDENTITY_BAD_STATE;
  }
  if (result == NH_IDENTITY_OK)
    result = insert_operation(store, operation_id, account_id,
      NH_IDENTITY_OPERATION_PROVIDER_ACTIVATE, NH_IDENTITY_PHASE_COMPLETE,
      NH_IDENTITY_OUTCOME_DONE, digest, NH_IDENTITY_HOME_CREATE);
  if (result == NH_IDENTITY_OK) result = nh_identity_bump_generation(store, NULL);
  if (result == NH_IDENTITY_OK) result = nh_identity_commit(store);
  if (result != NH_IDENTITY_OK) nh_identity_rollback(store);
  return result;
}

nh_identity_rc nh_identity_provider_reseal(
    nh_identity_store *store, const char *operation_id, const char *provider_id,
    const uint8_t *secret_blob, size_t secret_blob_len) {
  struct digest_builder builder = {{0}, 0};
  unsigned char digest[32];
  nh_identity_operation_state replay;
  char account_id[NH_IDENTITY_UUID_CAP];
  bool exists = false;
  sqlite3_stmt *statement = NULL;
  int rc;
  nh_identity_rc result;
  if (!store || !nh_identity_uuid_is_valid(operation_id) ||
      !nh_identity_uuid_is_valid(provider_id) || !secret_blob ||
      secret_blob_len == 0 || secret_blob_len > NH_IDENTITY_PROVIDER_SECRET_MAX)
    return NH_IDENTITY_INVALID;
  if (digest_text(&builder, "provider_reseal") ||
      digest_text(&builder, provider_id) ||
      digest_add(&builder, secret_blob, secret_blob_len) ||
      digest_finish(&builder, digest)) return NH_IDENTITY_INVALID;
  result = operation_replay(store, operation_id,
    NH_IDENTITY_OPERATION_PROVIDER_RESEAL, digest, &replay, &exists);
  if (result != NH_IDENTITY_OK || exists) return result;
  /* Only a staged (enabled=0) provider may be resealed. */
  rc = sqlite3_prepare_v2(store->db,
    "SELECT account_id FROM providers WHERE provider_id=? AND enabled=0",
    -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, provider_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  if (rc == SQLITE_DONE) result = NH_IDENTITY_NOT_FOUND;
  else if (rc != SQLITE_ROW ||
           copy_column(statement, 0, account_id, sizeof(account_id)))
    result = rc == SQLITE_ROW ? NH_IDENTITY_STORAGE_ERROR
                              : nh_identity_sqlite_result(store, rc, "find staged provider");
  sqlite3_finalize(statement);
  if (result != NH_IDENTITY_OK) return result;
  result = nh_identity_begin(store);
  if (result != NH_IDENTITY_OK) return result;
  rc = sqlite3_prepare_v2(store->db,
    "UPDATE providers SET secret_blob=?,updated_at=strftime('%s','now')"
    " WHERE provider_id=? AND enabled=0", -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_blob(statement, 1, secret_blob,
                                              (int)secret_blob_len, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, provider_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  result = nh_identity_sqlite_result(store, rc, "reseal provider");
  if (result == NH_IDENTITY_OK && sqlite3_changes(store->db) != 1)
    result = NH_IDENTITY_BAD_STATE;
  if (result == NH_IDENTITY_OK)
    result = insert_operation(store, operation_id, account_id,
      NH_IDENTITY_OPERATION_PROVIDER_RESEAL, NH_IDENTITY_PHASE_COMPLETE,
      NH_IDENTITY_OUTCOME_DONE, digest, NH_IDENTITY_HOME_CREATE);
  if (result == NH_IDENTITY_OK) result = nh_identity_bump_generation(store, NULL);
  if (result == NH_IDENTITY_OK) result = nh_identity_commit(store);
  if (result != NH_IDENTITY_OK) nh_identity_rollback(store);
  return result;
}

nh_identity_rc nh_identity_provider_discard(
    nh_identity_store *store, const char *operation_id,
    const char *provider_id) {
  struct digest_builder builder = {{0}, 0};
  unsigned char digest[32];
  nh_identity_operation_state replay;
  char account_id[NH_IDENTITY_UUID_CAP];
  bool exists = false;
  sqlite3_stmt *statement = NULL;
  int rc;
  nh_identity_rc result;
  if (!store || !nh_identity_uuid_is_valid(operation_id) ||
      !nh_identity_uuid_is_valid(provider_id)) return NH_IDENTITY_INVALID;
  if (digest_text(&builder, "provider_discard") || digest_text(&builder, provider_id) ||
      digest_finish(&builder, digest)) return NH_IDENTITY_INVALID;
  result = operation_replay(store, operation_id,
    NH_IDENTITY_OPERATION_PROVIDER_DISCARD, digest, &replay, &exists);
  if (result != NH_IDENTITY_OK || exists) return result;
  rc = sqlite3_prepare_v2(store->db,
    "SELECT account_id FROM providers WHERE provider_id=? AND enabled=0",
    -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, provider_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  if (rc == SQLITE_DONE) result = NH_IDENTITY_NOT_FOUND;
  else if (rc != SQLITE_ROW || copy_column(statement, 0, account_id, sizeof(account_id)))
    result = rc == SQLITE_ROW ? NH_IDENTITY_STORAGE_ERROR
                              : nh_identity_sqlite_result(store, rc, "find staged provider");
  sqlite3_finalize(statement);
  if (result != NH_IDENTITY_OK) return result;
  result = nh_identity_begin(store);
  if (result != NH_IDENTITY_OK) return result;
  rc = sqlite3_prepare_v2(store->db,
    "DELETE FROM providers WHERE provider_id=? AND enabled=0", -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, provider_id, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  result = nh_identity_sqlite_result(store, rc, "discard staged provider");
  if (result == NH_IDENTITY_OK && sqlite3_changes(store->db) != 1)
    result = NH_IDENTITY_BAD_STATE;
  if (result == NH_IDENTITY_OK)
    result = insert_operation(store, operation_id, account_id,
      NH_IDENTITY_OPERATION_PROVIDER_DISCARD, NH_IDENTITY_PHASE_COMPLETE,
      NH_IDENTITY_OUTCOME_DONE, digest, NH_IDENTITY_HOME_CREATE);
  if (result == NH_IDENTITY_OK) result = nh_identity_bump_generation(store, NULL);
  if (result == NH_IDENTITY_OK) result = nh_identity_commit(store);
  if (result != NH_IDENTITY_OK) nh_identity_rollback(store);
  return result;
}
