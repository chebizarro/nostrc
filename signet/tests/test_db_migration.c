/* SPDX-License-Identifier: MIT
 *
 * test_db_migration.c - Legacy plaintext SQLite -> SQLCipher migration.
 *
 * Detection tests run in any build. The end-to-end migration test runs only
 * when the linked sqlite3 is actually SQLCipher (signet_store_sqlcipher_available),
 * so it is a no-op SKIP in the default plain-sqlite build and a real migration
 * in a -Dsignet_use_sqlcipher build.
 */

#include "signet/store.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <sqlite3.h>

#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static char *make_temp_path(void) {
  char tmpl[] = "/tmp/signet-test-mig-XXXXXX.db";
  int fd = mkstemps(tmpl, 3);
  assert(fd >= 0);
  close(fd);
  unlink(tmpl);
  return g_strdup(tmpl);
}

static void cleanup(const char *db) {
  unlink(db);
  const char *sfx[] = { "-wal", "-shm", ".plaintext-backup", ".sqlcipher-migrating" };
  for (size_t i = 0; i < sizeof(sfx) / sizeof(sfx[0]); i++) {
    char *p = g_strdup_printf("%s%s", db, sfx[i]);
    unlink(p);
    g_free(p);
  }
}

/* Create a PLAINTEXT SQLite database (no key) with one known row. Works even in
 * a SQLCipher build because SQLCipher only encrypts once a key is set. */
static void create_plaintext_db(const char *path, const char *k, const char *v) {
  sqlite3 *db = NULL;
  assert(sqlite3_open(path, &db) == SQLITE_OK);
  assert(sqlite3_exec(db, "CREATE TABLE legacy(k TEXT PRIMARY KEY, v TEXT);",
                      NULL, NULL, NULL) == SQLITE_OK);
  char *sql = sqlite3_mprintf("INSERT INTO legacy(k,v) VALUES (%Q,%Q);", k, v);
  assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_free(sql);
  sqlite3_close(db);
}

static void test_detect_plaintext(void) {
  char *db = make_temp_path();
  create_plaintext_db(db, "x", "hello");

  assert(signet_store_file_is_plaintext_sqlite(db) == true);
  assert(signet_store_file_is_plaintext_sqlite("/nonexistent/path.db") == false);

  /* A file of random bytes is not a plaintext SQLite database. */
  char *junk = make_temp_path();
  FILE *f = fopen(junk, "wb");
  assert(f);
  const char noise[32] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
  fwrite(noise, 1, sizeof(noise), f);
  fclose(f);
  assert(signet_store_file_is_plaintext_sqlite(junk) == false);

  cleanup(db); cleanup(junk);
  g_free(db); g_free(junk);
  printf("test_detect_plaintext: PASS\n");
}

static void test_migrate_plaintext_to_sqlcipher(void) {
  if (!signet_store_sqlcipher_available()) {
    printf("test_migrate_plaintext_to_sqlcipher: SKIP (build not linked against SQLCipher)\n");
    return;
  }

  char *db = make_temp_path();
  create_plaintext_db(db, "answer", "42");
  assert(signet_store_file_is_plaintext_sqlite(db) == true);

  /* Migrate. */
  int rc = signet_store_migrate_plaintext_to_sqlcipher(db, MASTER_KEY);
  assert(rc == 0);

  /* The file is now encrypted (no plaintext magic). */
  assert(signet_store_file_is_plaintext_sqlite(db) == false);

  /* A plaintext backup of the original was kept. */
  char *bak = g_strdup_printf("%s.plaintext-backup", db);
  assert(g_file_test(bak, G_FILE_TEST_EXISTS));
  assert(signet_store_file_is_plaintext_sqlite(bak) == true);

  /* Migrating an already-encrypted DB is a no-op (returns 1, nothing to do). */
  assert(signet_store_migrate_plaintext_to_sqlcipher(db, MASTER_KEY) == 1);

  /* Data survived: open the encrypted DB with the key and read the row back. */
  sqlite3 *enc = NULL;
  assert(sqlite3_open(db, &enc) == SQLITE_OK);
  char *kp = sqlite3_mprintf("PRAGMA key = '%q';", MASTER_KEY);
  assert(sqlite3_exec(enc, kp, NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_free(kp);
  sqlite3_stmt *st = NULL;
  assert(sqlite3_prepare_v2(enc, "SELECT v FROM legacy WHERE k='answer';", -1, &st, NULL) == SQLITE_OK);
  assert(sqlite3_step(st) == SQLITE_ROW);
  const unsigned char *val = sqlite3_column_text(st, 0);
  assert(val && strcmp((const char *)val, "42") == 0);
  sqlite3_finalize(st);
  sqlite3_close(enc);

  /* And signet_store_open opens the migrated DB as encrypted. */
  SignetStoreConfig cfg = { .db_path = db, .master_key = MASTER_KEY };
  SignetStore *store = signet_store_open(&cfg);
  assert(store != NULL);
  assert(signet_store_is_encrypted(store) == true);
  signet_store_close(store);

  cleanup(db);
  g_free(bak);
  g_free(db);
  printf("test_migrate_plaintext_to_sqlcipher: PASS\n");
}

/* signet_store_open must auto-migrate a legacy plaintext DB when SQLCipher is
 * available (the default, unless SIGNET_MIGRATE_PLAINTEXT_DB=false). */
static void test_open_auto_migrates(void) {
  if (!signet_store_sqlcipher_available()) {
    printf("test_open_auto_migrates: SKIP (build not linked against SQLCipher)\n");
    return;
  }

  char *db = make_temp_path();
  create_plaintext_db(db, "k", "v");
  assert(signet_store_file_is_plaintext_sqlite(db) == true);

  SignetStoreConfig cfg = { .db_path = db, .master_key = MASTER_KEY };
  SignetStore *store = signet_store_open(&cfg);
  assert(store != NULL);
  assert(signet_store_is_encrypted(store) == true);
  signet_store_close(store);

  /* File was migrated in place; a backup exists. */
  assert(signet_store_file_is_plaintext_sqlite(db) == false);
  char *bak = g_strdup_printf("%s.plaintext-backup", db);
  assert(g_file_test(bak, G_FILE_TEST_EXISTS));

  cleanup(db);
  g_free(bak);
  g_free(db);
  printf("test_open_auto_migrates: PASS\n");
}

int main(void) {
  test_detect_plaintext();
  test_migrate_plaintext_to_sqlcipher();
  test_open_auto_migrates();
  printf("All db migration tests passed!\n");
  return 0;
}
