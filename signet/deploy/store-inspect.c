/* SPDX-License-Identifier: MIT
 * Read-only SQLCipher inspection without shell argv keys or schema migration.
 */
#define SQLITE_HAS_CODEC 1
#include <sqlite3.h>
#include <sodium.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static int emit(void *unused, int n, char **values, char **names) {
  (void)unused; (void)names;
  for (int i = 0; i < n; i++) printf("%s%s", i ? "|" : "", values[i] ? values[i] : "");
  putchar('\n');
  return 0;
}
int main(int argc, char **argv) {
  const char *dbpath, *keypath;
  int rc = 1, fd = -1;
  sqlite3 *db = NULL;
  unsigned char *key = NULL;
  struct stat st;
  if (sodium_init() < 0 || argc < 2) goto out;
  if (strcmp(argv[1], "state") == 0 && argc == 2) {
    dbpath = "/data/signet.db"; keypath = "/run/secrets/signet_db_key";
  } else if (strcmp(argv[1], "backup") == 0 && argc == 3 &&
             strncmp(argv[2], "/backups/", 9) == 0 && !strstr(argv[2], "/../")) {
    dbpath = argv[2]; keypath = "/run/secrets/signet_backup_key";
  } else goto out;
  if (lstat(dbpath, &st) || !S_ISREG(st.st_mode)) goto out;
  fd = open(keypath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 0777) != 0400 || st.st_size < 1 || st.st_size > 4096) goto out;
  key = sodium_malloc(4097);
  if (!key) goto out;
  size_t len = 0;
  for (;;) {
    ssize_t n = read(fd, key + len, 4097 - len);
    if (n < 0 || len + (size_t)n > 4096) goto out;
    if (!n) break;
    len += (size_t)n;
  }
  close(fd); fd = -1;
  if (len && key[len-1] == '\n') {
    len--;
    if (len && key[len-1] == '\r') len--;
  }
  if (!len || memchr(key, 0, len) || memchr(key, '\n', len) || memchr(key, '\r', len)) goto out;
  if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
      sqlite3_key(db, key, (int)len) != SQLITE_OK) goto out;
  sodium_free(key); key = NULL;
  /* Output contract: integrity result, schema cookie, ordered schema DDL. */
  if (sqlite3_exec(db, "PRAGMA integrity_check; PRAGMA schema_version; "
      "SELECT type,name,tbl_name,sql FROM sqlite_master WHERE sql IS NOT NULL ORDER BY type,name;",
      emit, NULL, NULL) != SQLITE_OK) goto out;
  rc = 0;
out:
  if (db) sqlite3_close(db);
  if (key) sodium_free(key);
  if (fd >= 0) close(fd);
  if (rc) fputs("SQLCipher read-only inspection failed (no key material logged)\n", stderr);
  return rc;
}
