#define _GNU_SOURCE
#include "nss_nostr.h"
#include "nostr_identity.h"
#include <assert.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
  const char *root = "/run/nostrc-nss-test";
  sqlite3 *db;
  FILE *config;
  struct passwd pwd;
  struct group grp;
  char buffer[1024];
  int err = 0;
  long start = 0, size = 0;
  gid_t *groups = NULL;
  struct stat before, after;
  if (geteuid() != 0) { fprintf(stderr, "Requires root in disposable Linux container\n"); return 77; }
  assert(mkdir(root, 0755) == 0);
  config = fopen("/run/nostrc-nss-test/nss.conf", "w"); assert(config);
  fputs("projection_path=/run/nostrc-nss-test/nss.db\n", config); assert(fclose(config) == 0);
  assert(_nss_nostr_getpwnam_r("n_alice", &pwd, buffer, sizeof buffer, &err) == NSS_STATUS_UNAVAIL);
  assert(access("/run/nostrc-nss-test/nss.db", F_OK) != 0);
  assert(sqlite3_open("/run/nostrc-nss-test/nss.db", &db) == SQLITE_OK);
  assert(sqlite3_exec(db,
    "PRAGMA application_id=1313361969; PRAGMA user_version=1;"
    "CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT);"
    "INSERT INTO meta VALUES('authority_id','test'),('projection_generation','1'),('source_authority_generation','2'),('format_minor','0'),('built_at','1');"
    "CREATE TABLE passwd(name TEXT,uid INTEGER,gid INTEGER,gecos TEXT,home TEXT,shell TEXT);"
    "INSERT INTO passwd VALUES('n_alice',200000,200000,'Nostr User','/home/n_alice','/bin/zsh');"
    "CREATE TABLE grp(name TEXT,gid INTEGER); INSERT INTO grp VALUES('n_alice',200000);", NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_close(db);
  assert(chmod("/run/nostrc-nss-test/nss.db", 0444) == 0);
  assert(stat("/run/nostrc-nss-test/nss.db", &before) == 0);
  assert(_nss_nostr_getpwnam_r("n_alice", &pwd, buffer, sizeof buffer, &err) == NSS_STATUS_SUCCESS);
  assert(pwd.pw_uid == 200000 && !strcmp(pwd.pw_shell, "/bin/zsh"));
  assert(pwd.pw_name >= buffer && pwd.pw_name < buffer + sizeof buffer);
  assert(_nss_nostr_getpwuid_r(200000, &pwd, buffer, sizeof buffer, &err) == NSS_STATUS_SUCCESS);
  assert(_nss_nostr_getpwnam_r("n_missing", &pwd, buffer, sizeof buffer, &err) == NSS_STATUS_NOTFOUND);
  assert(_nss_nostr_getpwnam_r("n_alice", &pwd, buffer, 1, &err) == NSS_STATUS_TRYAGAIN && err == ERANGE);
  assert(_nss_nostr_getgrnam_r("n_alice", &grp, buffer + 1, sizeof buffer - 1, &err) == NSS_STATUS_SUCCESS);
  assert(grp.gr_gid == 200000 && grp.gr_mem[0] == NULL && (uintptr_t)grp.gr_mem % sizeof(char *) == 0);
  assert(_nss_nostr_getgrgid_r(200000, &grp, buffer, 10, &err) == NSS_STATUS_TRYAGAIN && err == ERANGE);
  assert(_nss_nostr_initgroups_dyn("ordinary", 99, &start, &size, &groups, 0, &err) == NSS_STATUS_NOTFOUND && start == 0);
  assert(_nss_nostr_initgroups_dyn("n_alice", 99, &start, &size, &groups, 1, &err) == NSS_STATUS_SUCCESS && start == 1 && size == 1 && groups[0] == 200000);
  assert(_nss_nostr_initgroups_dyn("n_alice", 99, &start, &size, &groups, 1, &err) == NSS_STATUS_SUCCESS && start == 1);
  groups[0] = 7;
  assert(_nss_nostr_initgroups_dyn("n_alice", 99, &start, &size, &groups, 1, &err) == NSS_STATUS_SUCCESS && start == 1 && groups[0] == 7);
  start = 0;
  assert(_nss_nostr_initgroups_dyn("n_alice", 200000, &start, &size, &groups, 0, &err) == NSS_STATUS_SUCCESS && start == 0);
  free(groups);
  pid_t child = fork(); assert(child >= 0);
  if (!child) { assert(setuid(65534) == 0); _exit(_nss_nostr_getpwuid_r(200000, &pwd, buffer, sizeof buffer, &err) == NSS_STATUS_SUCCESS ? 0 : 1); }
  int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(stat("/run/nostrc-nss-test/nss.db", &after) == 0 && before.st_mtime == after.st_mtime && before.st_size == after.st_size);
  assert(access("/run/nostrc-nss-test/nss.db-wal", F_OK) != 0 && access("/run/nostrc-nss-test/nss.db-shm", F_OK) != 0);
  assert(chmod("/run/nostrc-nss-test/nss.db", 0666) == 0);
  assert(_nss_nostr_getpwuid_r(200000, &pwd, buffer, sizeof buffer, &err) == NSS_STATUS_UNAVAIL);
  assert(unlink("/run/nostrc-nss-test/nss.db") == 0);
  assert(symlink("/etc/passwd", "/run/nostrc-nss-test/nss.db") == 0);
  assert(_nss_nostr_getpwuid_r(200000, &pwd, buffer, sizeof buffer, &err) == NSS_STATUS_UNAVAIL);
  puts("NSS projection: read-only ABI and failure semantics passed"); return 0;
}
