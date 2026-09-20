#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "identity_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int reader_open_db(const nh_identity_reader *reader, sqlite3 **out) {
  int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;
  *out = NULL;
  return sqlite3_open_v2(reader->path, out, flags, NULL);
}

static int pragma_value(sqlite3 *db, const char *sql, sqlite3_int64 *out) {
  sqlite3_stmt *statement = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  if (rc == SQLITE_ROW) *out = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  return rc == SQLITE_ROW ? 0 : -1;
}

static int validate_projection(sqlite3 *db) {
  sqlite3_stmt *statement = NULL;
  sqlite3_int64 application_id, user_version;
  int rc, count;
  if (pragma_value(db, "PRAGMA application_id", &application_id) != 0 ||
      pragma_value(db, "PRAGMA user_version", &user_version) != 0 ||
      application_id != NH_IDENTITY_PROJECTION_APPLICATION_ID ||
      user_version != NH_IDENTITY_PROJECTION_SCHEMA_VERSION)
    return -1;
  rc = sqlite3_prepare_v2(db,
    "SELECT count(*) FROM meta WHERE key IN('authority_id',"
    "'projection_generation','source_authority_generation','format_minor','built_at')",
    -1, &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  count = rc == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
  sqlite3_finalize(statement);
  if (count != 5) return -1;
  rc = sqlite3_prepare_v2(db,
    "SELECT name,uid,gid,gecos,home,shell FROM passwd LIMIT 0", -1,
    &statement, NULL);
  sqlite3_finalize(statement);
  if (rc != SQLITE_OK) return -1;
  rc = sqlite3_prepare_v2(db, "SELECT name,gid FROM grp LIMIT 0", -1,
                          &statement, NULL);
  sqlite3_finalize(statement);
  return rc == SQLITE_OK ? 0 : -1;
}

nh_identity_reader_result nh_identity_reader_open(
    const char *path, nh_identity_reader **out) {
  nh_identity_reader *reader;
  struct stat st;
  sqlite3 *db = NULL;
  size_t length;
  if (!path || !out || path[0] != '/') return NH_IDENTITY_READER_UNAVAILABLE;
  *out = NULL;
  length = strlen(path);
  if (length == 0 || length >= NH_IDENTITY_HOME_CAP ||
      lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
    return NH_IDENTITY_READER_UNAVAILABLE;
  reader = calloc(1, sizeof(*reader));
  if (!reader) return NH_IDENTITY_READER_UNAVAILABLE;
  memcpy(reader->path, path, length + 1);
  if (reader_open_db(reader, &db) != SQLITE_OK || !db ||
      validate_projection(db) != 0) {
    if (db) sqlite3_close(db);
    free(reader);
    return NH_IDENTITY_READER_UNAVAILABLE;
  }
  sqlite3_close(db);
  *out = reader;
  return NH_IDENTITY_READER_FOUND;
}

void nh_identity_reader_close(nh_identity_reader *reader) {
  if (!reader) return;
  memset(reader, 0, sizeof(*reader));
  free(reader);
}

static int valid_field(const char *value, size_t max, bool require_absolute) {
  size_t length;
  if (!value) return -1;
  length = strlen(value);
  if (length == 0 || length > max || strchr(value, ':') || strchr(value, '\n') ||
      strchr(value, '\r') || (require_absolute && value[0] != '/'))
    return -1;
  return 0;
}

static char *buffer_copy(char **cursor, const char *value) {
  size_t length = strlen(value) + 1;
  char *start = *cursor;
  memcpy(start, value, length);
  *cursor += length;
  return start;
}

static nh_identity_reader_result passwd_lookup(nh_identity_reader *reader,
    const char *sql, const char *name, uint32_t uid,
    nh_identity_passwd_record *out, char *buffer, size_t buffer_len,
    size_t *required_len) {
  sqlite3 *db = NULL;
  sqlite3_stmt *statement = NULL;
  const char *row_name, *gecos, *home, *shell;
  sqlite3_int64 row_uid, row_gid;
  size_t required;
  char *cursor;
  int rc;
  nh_identity_reader_result result = NH_IDENTITY_READER_UNAVAILABLE;
  if (!reader || !out || !buffer || !required_len) return result;
  *required_len = 0;
  if (reader_open_db(reader, &db) != SQLITE_OK || !db ||
      validate_projection(db) != 0) goto done;
  rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
  if (rc == SQLITE_OK && name)
    rc = sqlite3_bind_text(statement, 1, name, -1, SQLITE_STATIC);
  else if (rc == SQLITE_OK)
    rc = sqlite3_bind_int64(statement, 1, (sqlite3_int64)uid);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  if (rc == SQLITE_DONE) { result = NH_IDENTITY_READER_NOT_FOUND; goto done; }
  if (rc != SQLITE_ROW) goto done;
  row_name = (const char *)sqlite3_column_text(statement, 0);
  row_uid = sqlite3_column_int64(statement, 1);
  row_gid = sqlite3_column_int64(statement, 2);
  gecos = (const char *)sqlite3_column_text(statement, 3);
  home = (const char *)sqlite3_column_text(statement, 4);
  shell = (const char *)sqlite3_column_text(statement, 5);
  if (!nh_identity_username_is_valid(row_name) || row_uid <= 0 ||
      row_uid > UINT32_MAX || row_gid <= 0 || row_gid > UINT32_MAX ||
      valid_field(gecos, 127, false) || valid_field(home, NH_IDENTITY_HOME_MAX, true) ||
      valid_field(shell, NH_IDENTITY_SHELL_MAX, true)) goto done;
  required = strlen(row_name) + 1 + 2 + strlen(gecos) + 1 +
             strlen(home) + 1 + strlen(shell) + 1;
  *required_len = required;
  if (buffer_len < required) { result = NH_IDENTITY_READER_TOO_SMALL; goto done; }
  memset(out, 0, sizeof(*out));
  cursor = buffer;
  out->name = buffer_copy(&cursor, row_name);
  out->passwd = buffer_copy(&cursor, "x");
  out->gecos = buffer_copy(&cursor, gecos);
  out->home = buffer_copy(&cursor, home);
  out->shell = buffer_copy(&cursor, shell);
  out->uid = (uint32_t)row_uid;
  out->gid = (uint32_t)row_gid;
  result = NH_IDENTITY_READER_FOUND;
done:
  sqlite3_finalize(statement);
  if (db) sqlite3_close(db);
  return result;
}

nh_identity_reader_result nh_identity_reader_getpwnam(
    nh_identity_reader *reader, const char *name,
    nh_identity_passwd_record *out, char *buffer, size_t buffer_len,
    size_t *required_len) {
  if (!nh_identity_username_is_valid(name)) {
    if (required_len) *required_len = 0;
    return NH_IDENTITY_READER_NOT_FOUND;
  }
  return passwd_lookup(reader,
    "SELECT name,uid,gid,gecos,home,shell FROM passwd WHERE name=?",
    name, 0, out, buffer, buffer_len, required_len);
}

nh_identity_reader_result nh_identity_reader_getpwuid(
    nh_identity_reader *reader, uint32_t uid,
    nh_identity_passwd_record *out, char *buffer, size_t buffer_len,
    size_t *required_len) {
  return passwd_lookup(reader,
    "SELECT name,uid,gid,gecos,home,shell FROM passwd WHERE uid=?",
    NULL, uid, out, buffer, buffer_len, required_len);
}

static nh_identity_reader_result group_lookup(nh_identity_reader *reader,
    const char *sql, const char *name, uint32_t gid,
    nh_identity_group_record *out, char *buffer, size_t buffer_len,
    size_t *required_len) {
  sqlite3 *db = NULL;
  sqlite3_stmt *statement = NULL;
  const char *row_name;
  sqlite3_int64 row_gid;
  size_t required;
  char *cursor;
  int rc;
  nh_identity_reader_result result = NH_IDENTITY_READER_UNAVAILABLE;
  if (!reader || !out || !buffer || !required_len) return result;
  *required_len = 0;
  if (reader_open_db(reader, &db) != SQLITE_OK || !db ||
      validate_projection(db) != 0) goto done;
  rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
  if (rc == SQLITE_OK && name)
    rc = sqlite3_bind_text(statement, 1, name, -1, SQLITE_STATIC);
  else if (rc == SQLITE_OK)
    rc = sqlite3_bind_int64(statement, 1, (sqlite3_int64)gid);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  if (rc == SQLITE_DONE) { result = NH_IDENTITY_READER_NOT_FOUND; goto done; }
  if (rc != SQLITE_ROW) goto done;
  row_name = (const char *)sqlite3_column_text(statement, 0);
  row_gid = sqlite3_column_int64(statement, 1);
  if (!nh_identity_username_is_valid(row_name) || row_gid <= 0 ||
      row_gid > UINT32_MAX) goto done;
  required = strlen(row_name) + 1 + 2;
  *required_len = required;
  if (buffer_len < required) { result = NH_IDENTITY_READER_TOO_SMALL; goto done; }
  memset(out, 0, sizeof(*out));
  cursor = buffer;
  out->name = buffer_copy(&cursor, row_name);
  out->passwd = buffer_copy(&cursor, "x");
  out->gid = (uint32_t)row_gid;
  result = NH_IDENTITY_READER_FOUND;
done:
  sqlite3_finalize(statement);
  if (db) sqlite3_close(db);
  return result;
}

nh_identity_reader_result nh_identity_reader_getgrnam(
    nh_identity_reader *reader, const char *name,
    nh_identity_group_record *out, char *buffer, size_t buffer_len,
    size_t *required_len) {
  if (!nh_identity_username_is_valid(name)) {
    if (required_len) *required_len = 0;
    return NH_IDENTITY_READER_NOT_FOUND;
  }
  return group_lookup(reader, "SELECT name,gid FROM grp WHERE name=?", name, 0,
                      out, buffer, buffer_len, required_len);
}

nh_identity_reader_result nh_identity_reader_getgrgid(
    nh_identity_reader *reader, uint32_t gid,
    nh_identity_group_record *out, char *buffer, size_t buffer_len,
    size_t *required_len) {
  return group_lookup(reader, "SELECT name,gid FROM grp WHERE gid=?", NULL, gid,
                      out, buffer, buffer_len, required_len);
}

static int parse_metadata_u64(sqlite3 *db, const char *key, uint64_t *out) {
  sqlite3_stmt *statement = NULL;
  const unsigned char *text;
  char *end = NULL;
  unsigned long long value;
  int rc = sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key=?", -1,
                              &statement, NULL);
  if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, key, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) rc = sqlite3_step(statement);
  if (rc != SQLITE_ROW) { sqlite3_finalize(statement); return -1; }
  text = sqlite3_column_text(statement, 0);
  if (!text || !*text || text[0] == '-') { sqlite3_finalize(statement); return -1; }
  errno = 0; value = strtoull((const char *)text, &end, 10);
  if (errno || !end || *end) { sqlite3_finalize(statement); return -1; }
  *out = (uint64_t)value;
  sqlite3_finalize(statement); return 0;
}

nh_identity_reader_result nh_identity_reader_get_generation(
    nh_identity_reader *reader, uint64_t *projection_generation_out,
    uint64_t *source_authority_generation_out) {
  sqlite3 *db = NULL;
  nh_identity_reader_result result = NH_IDENTITY_READER_UNAVAILABLE;
  if (!reader || !projection_generation_out || !source_authority_generation_out)
    return result;
  if (reader_open_db(reader, &db) != SQLITE_OK || !db ||
      validate_projection(db) != 0 ||
      parse_metadata_u64(db, "projection_generation", projection_generation_out) ||
      parse_metadata_u64(db, "source_authority_generation",
                         source_authority_generation_out)) goto done;
  result = NH_IDENTITY_READER_FOUND;
done:
  if (db) sqlite3_close(db);
  return result;
}
