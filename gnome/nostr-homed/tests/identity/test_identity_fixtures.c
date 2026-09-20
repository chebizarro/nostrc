#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "nostr_identity.h"
#include <assert.h>
#include "../nh_test.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IDENTITY_FIXTURE_DIR
#error IDENTITY_FIXTURE_DIR must be defined
#endif

static char *read_file(const char *path) {
  FILE *file = fopen(path, "rb");
  long length;
  char *data;
  NH_CHECK(file);
  NH_CHECK(fseek(file, 0, SEEK_END) == 0);
  length = ftell(file); NH_CHECK(length >= 0);
  NH_CHECK(fseek(file, 0, SEEK_SET) == 0);
  data = malloc((size_t)length + 1); NH_CHECK(data);
  NH_CHECK(fread(data, 1, (size_t)length, file) == (size_t)length);
  data[length] = '\0'; fclose(file); return data;
}

static sqlite3 *load_fixture(const char *name) {
  char path[1024];
  char *sql, *error = NULL;
  sqlite3 *db = NULL;
  snprintf(path, sizeof(path), "%s/%s", IDENTITY_FIXTURE_DIR, name);
  sql = read_file(path);
  NH_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  NH_CHECK(sqlite3_exec(db, sql, NULL, NULL, &error) == SQLITE_OK);
  sqlite3_free(error); free(sql); return db;
}

static long long scalar(sqlite3 *db, const char *sql) {
  sqlite3_stmt *statement = NULL;
  long long value;
  NH_CHECK(sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK);
  NH_CHECK(sqlite3_step(statement) == SQLITE_ROW);
  value = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement); return value;
}

int main(void) {
  sqlite3 *projection = load_fixture("projection_v1.sql");
  sqlite3 *legacy = load_fixture("legacy_conflicts.sql");
  NH_CHECK(scalar(projection, "PRAGMA application_id") ==
         NH_IDENTITY_PROJECTION_APPLICATION_ID);
  NH_CHECK(scalar(projection, "PRAGMA user_version") == 1);
  NH_CHECK(scalar(projection, "SELECT count(*) FROM passwd") == 2);
  NH_CHECK(scalar(projection,
    "SELECT count(*) FROM pragma_table_info('passwd') WHERE name IN"
    " ('status','pubkey_hex','account_id','provider_id')") == 0);
  NH_CHECK(scalar(projection,
    "SELECT count(*) FROM passwd WHERE name='n_disabled'") == 1);
  NH_CHECK(scalar(legacy,
    "SELECT count(*) FROM users WHERE npub NOT LIKE 'npub1%'"
    " AND length(npub)<>64") == 1);
  NH_CHECK(scalar(legacy,
    "SELECT count(*) FROM users GROUP BY home HAVING count(*)>1") == 2);
  NH_CHECK(scalar(legacy,
    "SELECT count(*) FROM migration_evidence WHERE kind IN"
    " ('overwritten_uid','mounted_home')") == 2);
  sqlite3_close(projection); sqlite3_close(legacy);
  puts("identity fixtures: PASS");
  return 0;
}
