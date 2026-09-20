#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "identity_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
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

static int projection_parent(const char *path, char *out, size_t capacity) {
  const char *slash = strrchr(path, '/');
  size_t length;
  if (!slash || path[0] != '/') return -1;
  length = slash == path ? 1u : (size_t)(slash - path);
  if (length >= capacity) return -1;
  memcpy(out, path, length); out[length] = '\0'; return 0;
}

static int remove_stale_regular(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
  if (!S_ISREG(st.st_mode) || st.st_uid != geteuid()) return -1;
  return unlink(path);
}

static int projection_exec(sqlite3 *db, const char *sql) {
  char *error = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
  sqlite3_free(error);
  return rc == SQLITE_OK ? 0 : -1;
}

static int bind_meta(sqlite3 *db, const char *key, const char *value) {
  sqlite3_stmt *statement = NULL;
  int rc = sqlite3_prepare_v2(db,
    "INSERT INTO meta(key,value) VALUES(?,?)", -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, key, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, value, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  sqlite3_finalize(statement);
  return rc == SQLITE_DONE ? 0 : -1;
}

static int build_projection(nh_identity_store *store, const char *temporary,
                            uint64_t projection_generation,
                            uint64_t source_generation,
                            const char *authority_id) {
  sqlite3 *db = NULL;
  sqlite3_stmt *source = NULL, *passwd_insert = NULL, *group_insert = NULL;
  char value[32], pragma[128];
  int rc, result = -1;
  rc = sqlite3_open_v2(temporary, &db,
    SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL);
  if (rc != SQLITE_OK || !db) goto done;
  sqlite3_busy_timeout(db, 5000);
  snprintf(pragma, sizeof(pragma),
    "PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL;"
    "PRAGMA application_id=%u;PRAGMA user_version=%u;",
    NH_IDENTITY_PROJECTION_APPLICATION_ID,
    NH_IDENTITY_PROJECTION_SCHEMA_VERSION);
  if (projection_exec(db, pragma) || projection_exec(db,
    "BEGIN IMMEDIATE;"
    "CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE passwd(name TEXT PRIMARY KEY,uid INTEGER UNIQUE NOT NULL,"
    "gid INTEGER NOT NULL,gecos TEXT NOT NULL,home TEXT NOT NULL,"
    "shell TEXT NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE grp(name TEXT PRIMARY KEY,gid INTEGER UNIQUE NOT NULL)"
    " WITHOUT ROWID;")) goto done;
  if (bind_meta(db, "authority_id", authority_id)) goto done;
  snprintf(value, sizeof(value), "%llu",
           (unsigned long long)projection_generation);
  if (bind_meta(db, "projection_generation", value)) goto done;
  snprintf(value, sizeof(value), "%llu", (unsigned long long)source_generation);
  if (bind_meta(db, "source_authority_generation", value) ||
      bind_meta(db, "format_minor", "0")) goto done;
  snprintf(value, sizeof(value), "%lld", (long long)time(NULL));
  if (bind_meta(db, "built_at", value)) goto done;
  rc = sqlite3_prepare_v2(store->db,
    "SELECT username,uid,gid,home,shell FROM accounts"
    " WHERE projectable=1 ORDER BY uid", -1, &source, NULL);
  if (rc != SQLITE_OK) goto done;
  rc = sqlite3_prepare_v2(db,
    "INSERT INTO passwd(name,uid,gid,gecos,home,shell)"
    " VALUES(?,?,?,'Nostr User',?,?)", -1, &passwd_insert, NULL);
  if (rc != SQLITE_OK) goto done;
  rc = sqlite3_prepare_v2(db,
    "INSERT INTO grp(name,gid) VALUES(?,?)", -1, &group_insert, NULL);
  if (rc != SQLITE_OK) goto done;
  while ((rc = sqlite3_step(source)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(source, 0);
    sqlite3_int64 uid = sqlite3_column_int64(source, 1);
    sqlite3_int64 gid = sqlite3_column_int64(source, 2);
    const char *home = (const char *)sqlite3_column_text(source, 3);
    const char *shell = (const char *)sqlite3_column_text(source, 4);
    if (!name || !home || !shell || uid <= 0 || uid > UINT32_MAX ||
        gid <= 0 || gid > UINT32_MAX) goto done;
    sqlite3_reset(passwd_insert); sqlite3_clear_bindings(passwd_insert);
    if (sqlite3_bind_text(passwd_insert, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64(passwd_insert, 2, uid) != SQLITE_OK ||
        sqlite3_bind_int64(passwd_insert, 3, gid) != SQLITE_OK ||
        sqlite3_bind_text(passwd_insert, 4, home, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(passwd_insert, 5, shell, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(passwd_insert) != SQLITE_DONE) goto done;
    sqlite3_reset(group_insert); sqlite3_clear_bindings(group_insert);
    if (sqlite3_bind_text(group_insert, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64(group_insert, 2, gid) != SQLITE_OK ||
        sqlite3_step(group_insert) != SQLITE_DONE) goto done;
  }
  if (rc != SQLITE_DONE || projection_exec(db, "COMMIT") != 0) goto done;
  result = 0;
done:
  sqlite3_finalize(source);
  sqlite3_finalize(passwd_insert);
  sqlite3_finalize(group_insert);
  if (result != 0 && db) (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
  if (db && sqlite3_close(db) != SQLITE_OK) result = -1;
  return result;
}

static int validate_built_projection(nh_identity_store *store,
                                     const char *path,
                                     uint64_t expected_projection,
                                     uint64_t expected_source) {
  nh_identity_reader *reader = NULL;
  nh_identity_passwd_record record;
  sqlite3_stmt *statement = NULL;
  char buffer[NH_IDENTITY_READER_BUF_MAX];
  uint64_t projection, source;
  size_t required;
  int rc, result = -1;
  if (nh_identity_reader_open(path, &reader) != NH_IDENTITY_READER_FOUND ||
      nh_identity_reader_get_generation(reader, &projection, &source) !=
        NH_IDENTITY_READER_FOUND || projection != expected_projection ||
      source != expected_source) goto done;
  rc = sqlite3_prepare_v2(store->db,
    "SELECT username,uid,gid,home,shell FROM accounts WHERE projectable=1",
    -1, &statement, NULL);
  if (rc != SQLITE_OK) goto done;
  while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(statement, 0);
    if (!name || nh_identity_reader_getpwnam(reader, name, &record, buffer,
          sizeof(buffer), &required) != NH_IDENTITY_READER_FOUND ||
        record.uid != (uint32_t)sqlite3_column_int64(statement, 1) ||
        record.gid != (uint32_t)sqlite3_column_int64(statement, 2) ||
        strcmp(record.home, (const char *)sqlite3_column_text(statement, 3)) ||
        strcmp(record.shell, (const char *)sqlite3_column_text(statement, 4)))
      goto done;
  }
  if (rc == SQLITE_DONE) result = 0;
done:
  sqlite3_finalize(statement);
  nh_identity_reader_close(reader);
  return result;
}

nh_identity_rc nh_identity_store_publish_projection(
    nh_identity_store *store, uint64_t *projection_generation_out) {
  nh_identity_store_info info;
  nh_identity_reader *installed = NULL;
  char temporary[NH_IDENTITY_HOME_CAP + 8], journal[NH_IDENTITY_HOME_CAP + 16];
  char directory[NH_IDENTITY_HOME_CAP];
  uint64_t installed_projection = 0, installed_source = 0;
  uint64_t next_projection, source_generation, committed_generation;
  struct stat st;
  int fd = -1, dirfd = -1, written;
  const char *failure_stage = "initialize";
  nh_identity_rc result;
  if (!store) return NH_IDENTITY_INVALID;
  result = nh_identity_store_get_info(store, &info);
  if (result != NH_IDENTITY_OK) return result;
  if (nh_identity_reader_open(store->config.projection_path, &installed) ==
        NH_IDENTITY_READER_FOUND) {
    (void)nh_identity_reader_get_generation(installed, &installed_projection,
                                             &installed_source);
    nh_identity_reader_close(installed); installed = NULL;
  }
  (void)installed_source;
  next_projection = info.projection_generation > installed_projection
    ? info.projection_generation + 1 : installed_projection + 1;
  source_generation = info.authority_generation + 1;
  written = snprintf(temporary, sizeof(temporary), "%s.new",
                     store->config.projection_path);
  if (written < 0 || (size_t)written >= sizeof(temporary) ||
      projection_parent(store->config.projection_path, directory,
                        sizeof(directory)) != 0)
    return NH_IDENTITY_INVALID;
  written = snprintf(journal, sizeof(journal), "%s-journal", temporary);
  if (written < 0 || (size_t)written >= sizeof(journal))
    return NH_IDENTITY_INVALID;
  failure_stage = "remove stale projection";
  if (remove_stale_regular(temporary) != 0 || remove_stale_regular(journal) != 0) {
    nh_identity_set_error(store, "%s", failure_stage);
    return NH_IDENTITY_STORAGE_ERROR;
  }
  fd = open(temporary, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) { nh_identity_set_error(store, "create projection temp: %s", strerror(errno)); return NH_IDENTITY_STORAGE_ERROR; }
  close(fd); fd = -1;
  failure_stage = "build projection";
  if (build_projection(store, temporary, next_projection, source_generation,
                       info.authority_id) != 0 ||
      lstat(journal, &st) == 0) goto fail;
  failure_stage = "validate projection";
  if (validate_built_projection(store, temporary, next_projection,
                                source_generation) != 0) goto fail;
  failure_stage = "chmod projection";
  if (chmod(temporary, 0644) != 0) goto fail;
  failure_stage = "fsync projection";
  fd = open(temporary, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) goto fail;
  if (fsync(fd) != 0) { close(fd); fd = -1; goto fail; }
  if (close(fd) != 0) { fd = -1; goto fail; }
  fd = -1;
  failure_stage = "rename projection";
  if (rename(temporary, store->config.projection_path) != 0) goto fail;
  dirfd = open(directory, O_RDONLY | O_CLOEXEC);
  if (dirfd < 0) return NH_IDENTITY_STORAGE_ERROR;
  if (fsync(dirfd) != 0) { close(dirfd); return NH_IDENTITY_STORAGE_ERROR; }
  if (close(dirfd) != 0) return NH_IDENTITY_STORAGE_ERROR;
  dirfd = -1;
  result = nh_identity_begin(store);
  if (result != NH_IDENTITY_OK) return result;
  {
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(store->db,
      "UPDATE metadata SET value=? WHERE key='projection_generation'", -1,
      &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 1,
                                                  (sqlite3_int64)next_projection);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    result = nh_identity_sqlite_result(store, rc, "record projection generation");
  }
  if (result == NH_IDENTITY_OK)
    result = nh_identity_sqlite_result(store,
      sqlite3_exec(store->db,
        "UPDATE operations SET phase='projected',updated_at=strftime('%s','now')"
        " WHERE phase='installed' AND outcome='pending' AND account_id IN"
        " (SELECT account_id FROM accounts WHERE projectable=1)",
        NULL, NULL, NULL), "advance projected operations");
  if (result == NH_IDENTITY_OK)
    result = nh_identity_bump_generation(store, &committed_generation);
  if (result == NH_IDENTITY_OK && committed_generation != source_generation)
    result = NH_IDENTITY_STORAGE_ERROR;
  if (result == NH_IDENTITY_OK) result = nh_identity_commit(store);
  if (result != NH_IDENTITY_OK) { nh_identity_rollback(store); return result; }
  if (projection_generation_out) *projection_generation_out = next_projection;
  return NH_IDENTITY_OK;
fail:
  nh_identity_set_error(store, "%s: %s", failure_stage, strerror(errno));
  if (fd >= 0) close(fd);
  (void)remove_stale_regular(temporary);
  (void)remove_stale_regular(journal);
  return NH_IDENTITY_STORAGE_ERROR;
}
