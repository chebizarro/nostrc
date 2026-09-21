#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* Portable negative-evidence tests for the read-only projection reader.
 *
 * Covers the A3/A5 acceptance criteria that don't require root:
 *   - missing DB file                            -> UNAVAILABLE
 *   - corrupt/non-SQLite file                    -> UNAVAILABLE
 *   - wrong application_id or user_version       -> UNAVAILABLE
 *   - missing meta keys                          -> UNAVAILABLE
 *   - missing passwd or grp tables               -> UNAVAILABLE
 *   - unknown name/uid                           -> NOT_FOUND
 *   - undersized caller buffer                   -> TOO_SMALL, retry -> FOUND
 *   - a disabled-but-projected user resolves     -> FOUND (the read-only
 *     projection carries no status column, so the reader must return it —
 *     this reinforces that disablement is enforced on the auth side, never
 *     on the NSS-visible identity projection).
 *
 * Read-only side-effect check: after any lookup, no `<db>-wal` / `<db>-shm`
 * / `<db>-journal` sidecars must exist next to the projection.
 *
 * Tests build minimal projection DBs in-process via the sqlite3 API into a
 * unique tempdir, so no root and no shared /run/... paths are required.
 */

#include "nostr_identity.h"
#include "../nh_test.h"

#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
  char dir[512];
  char db[512];
} scratch;

static void scratch_init(scratch *s) {
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !*tmp) tmp = "/tmp";
  snprintf(s->dir, sizeof s->dir, "%s/nh_reader_neg_XXXXXX", tmp);
  NH_CHECK(mkdtemp(s->dir));
  snprintf(s->db, sizeof s->db, "%s/projection.db", s->dir);
}

static void unlink_if_present(const char *path) {
  if (unlink(path) != 0 && errno != ENOENT) NH_CHECK(0);
}

static void scratch_reset(const scratch *s) {
  char sidecar[600];
  unlink_if_present(s->db);
  snprintf(sidecar, sizeof sidecar, "%s-wal", s->db);
  unlink_if_present(sidecar);
  snprintf(sidecar, sizeof sidecar, "%s-shm", s->db);
  unlink_if_present(sidecar);
  snprintf(sidecar, sizeof sidecar, "%s-journal", s->db);
  unlink_if_present(sidecar);
}

static void scratch_release(scratch *s) {
  scratch_reset(s);
  rmdir(s->dir);
}

static void write_file(const char *path, const void *data, size_t length,
                       mode_t mode) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
  ssize_t written;
  NH_CHECK(fd >= 0);
  written = write(fd, data, length);
  NH_CHECK(written == (ssize_t)length);
  NH_CHECK(close(fd) == 0);
}

/* Build a valid projection DB unless caller flips one of the switches. */
typedef struct {
  int wrong_application_id;
  int wrong_user_version;
  int drop_meta_row;    /* omit `built_at` from meta */
  int drop_passwd;
  int drop_grp;
} build_opts;

static void build_projection(const char *path, const build_opts *opts) {
  sqlite3 *db = NULL;
  char sql[2048];
  uint32_t app_id = opts->wrong_application_id
                        ? 0xDEADBEEFu
                        : (uint32_t)NH_IDENTITY_PROJECTION_APPLICATION_ID;
  unsigned user_version =
      opts->wrong_user_version ? 99u : NH_IDENTITY_PROJECTION_SCHEMA_VERSION;
  unlink_if_present(path);
  NH_CHECK(sqlite3_open(path, &db) == SQLITE_OK);
  snprintf(sql, sizeof sql,
           "PRAGMA application_id=%u; PRAGMA user_version=%u;"
           "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
           "INSERT INTO meta VALUES"
           "('authority_id','11111111-1111-4111-8111-111111111111'),"
           "('projection_generation','7'),"
           "('source_authority_generation','12'),"
           "('format_minor','0')%s;",
           app_id, user_version,
           opts->drop_meta_row ? "" : ",('built_at','1789900000')");
  NH_CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
  if (!opts->drop_passwd) {
    NH_CHECK(sqlite3_exec(db,
             "CREATE TABLE passwd(name TEXT PRIMARY KEY,uid INTEGER UNIQUE NOT NULL,"
             " gid INTEGER NOT NULL,gecos TEXT NOT NULL,home TEXT NOT NULL,"
             " shell TEXT NOT NULL);"
             "INSERT INTO passwd VALUES"
             "('n_active',200000,200000,'Nostr User','/home/n_active','/bin/bash'),"
             "('n_disabled',200001,200001,'Nostr User','/home/n_disabled','/bin/bash');",
             NULL, NULL, NULL) == SQLITE_OK);
  }
  if (!opts->drop_grp) {
    NH_CHECK(sqlite3_exec(db,
             "CREATE TABLE grp(name TEXT PRIMARY KEY,gid INTEGER UNIQUE NOT NULL);"
             "INSERT INTO grp VALUES('n_active',200000),('n_disabled',200001);",
             NULL, NULL, NULL) == SQLITE_OK);
  }
  NH_CHECK(sqlite3_close(db) == SQLITE_OK);
}

static void expect_no_sidecars(const char *db) {
  char sidecar[600];
  snprintf(sidecar, sizeof sidecar, "%s-wal", db);
  NH_CHECK(access(sidecar, F_OK) != 0);
  snprintf(sidecar, sizeof sidecar, "%s-shm", db);
  NH_CHECK(access(sidecar, F_OK) != 0);
  snprintf(sidecar, sizeof sidecar, "%s-journal", db);
  NH_CHECK(access(sidecar, F_OK) != 0);
}

int main(void) {
  scratch s;
  nh_identity_reader *reader = NULL;
  nh_identity_passwd_record pw;
  nh_identity_group_record gr;
  char buffer[NH_IDENTITY_READER_BUF_MAX];
  size_t required = 0;
  nh_identity_reader_result r;

  scratch_init(&s);

  /* --- 1. Missing DB file --------------------------------------------- */
  scratch_reset(&s);
  r = nh_identity_reader_open(s.db, &reader);
  NH_CHECK(r == NH_IDENTITY_READER_UNAVAILABLE);
  NH_CHECK(reader == NULL);

  /* --- 2. Corrupt / non-SQLite file ----------------------------------- */
  scratch_reset(&s);
  write_file(s.db, "this is not a sqlite database\n", 30, 0600);
  r = nh_identity_reader_open(s.db, &reader);
  NH_CHECK(r == NH_IDENTITY_READER_UNAVAILABLE);
  NH_CHECK(reader == NULL);
  expect_no_sidecars(s.db);

  /* --- 3a. Wrong application_id --------------------------------------- */
  scratch_reset(&s);
  {
    build_opts o = {0};
    o.wrong_application_id = 1;
    build_projection(s.db, &o);
  }
  r = nh_identity_reader_open(s.db, &reader);
  NH_CHECK(r == NH_IDENTITY_READER_UNAVAILABLE);
  NH_CHECK(reader == NULL);
  expect_no_sidecars(s.db);

  /* --- 3b. Wrong user_version ----------------------------------------- */
  scratch_reset(&s);
  {
    build_opts o = {0};
    o.wrong_user_version = 1;
    build_projection(s.db, &o);
  }
  r = nh_identity_reader_open(s.db, &reader);
  NH_CHECK(r == NH_IDENTITY_READER_UNAVAILABLE);
  NH_CHECK(reader == NULL);
  expect_no_sidecars(s.db);

  /* --- 4. Missing meta key -------------------------------------------- */
  scratch_reset(&s);
  {
    build_opts o = {0};
    o.drop_meta_row = 1;
    build_projection(s.db, &o);
  }
  r = nh_identity_reader_open(s.db, &reader);
  NH_CHECK(r == NH_IDENTITY_READER_UNAVAILABLE);
  NH_CHECK(reader == NULL);
  expect_no_sidecars(s.db);

  /* --- 5a. Missing passwd table --------------------------------------- */
  scratch_reset(&s);
  {
    build_opts o = {0};
    o.drop_passwd = 1;
    build_projection(s.db, &o);
  }
  r = nh_identity_reader_open(s.db, &reader);
  NH_CHECK(r == NH_IDENTITY_READER_UNAVAILABLE);
  NH_CHECK(reader == NULL);
  expect_no_sidecars(s.db);

  /* --- 5b. Missing grp table ------------------------------------------ */
  scratch_reset(&s);
  {
    build_opts o = {0};
    o.drop_grp = 1;
    build_projection(s.db, &o);
  }
  r = nh_identity_reader_open(s.db, &reader);
  NH_CHECK(r == NH_IDENTITY_READER_UNAVAILABLE);
  NH_CHECK(reader == NULL);
  expect_no_sidecars(s.db);

  /* --- 6. Valid projection: unknown name/uid -> NOT_FOUND ------------- */
  scratch_reset(&s);
  {
    build_opts o = {0};
    build_projection(s.db, &o);
  }
  r = nh_identity_reader_open(s.db, &reader);
  NH_CHECK(r == NH_IDENTITY_READER_FOUND);
  NH_CHECK(reader != NULL);
  expect_no_sidecars(s.db);

  required = 12345;
  r = nh_identity_reader_getpwnam(reader, "n_missing_user", &pw, buffer,
                                  sizeof buffer, &required);
  NH_CHECK(r == NH_IDENTITY_READER_NOT_FOUND);
  expect_no_sidecars(s.db);

  required = 12345;
  r = nh_identity_reader_getpwuid(reader, 999999, &pw, buffer, sizeof buffer,
                                  &required);
  NH_CHECK(r == NH_IDENTITY_READER_NOT_FOUND);

  required = 12345;
  r = nh_identity_reader_getgrnam(reader, "n_missing_group", &gr, buffer,
                                  sizeof buffer, &required);
  NH_CHECK(r == NH_IDENTITY_READER_NOT_FOUND);

  required = 12345;
  r = nh_identity_reader_getgrgid(reader, 999999, &gr, buffer, sizeof buffer,
                                  &required);
  NH_CHECK(r == NH_IDENTITY_READER_NOT_FOUND);

  /* --- 7. Undersized buffer -> TOO_SMALL, retry -> FOUND -------------- */
  required = 0;
  r = nh_identity_reader_getpwnam(reader, "n_active", &pw, buffer, 1,
                                  &required);
  NH_CHECK(r == NH_IDENTITY_READER_TOO_SMALL);
  NH_CHECK(required > 1);
  NH_CHECK(required <= sizeof buffer);
  {
    size_t retry_needed = required;
    size_t retry_out = 0;
    r = nh_identity_reader_getpwnam(reader, "n_active", &pw, buffer,
                                    retry_needed, &retry_out);
    NH_CHECK(r == NH_IDENTITY_READER_FOUND);
    NH_CHECK(retry_out == retry_needed);
    NH_CHECK(pw.uid == 200000);
    NH_CHECK(pw.gid == 200000);
    NH_CHECK(strcmp(pw.name, "n_active") == 0);
    NH_CHECK(strcmp(pw.home, "/home/n_active") == 0);
    NH_CHECK(strcmp(pw.shell, "/bin/bash") == 0);
    NH_CHECK(pw.name >= buffer && pw.name < buffer + sizeof buffer);
  }
  expect_no_sidecars(s.db);

  /* group TOO_SMALL then retry */
  required = 0;
  r = nh_identity_reader_getgrgid(reader, 200000, &gr, buffer, 1, &required);
  NH_CHECK(r == NH_IDENTITY_READER_TOO_SMALL);
  NH_CHECK(required > 1);
  {
    size_t retry_needed = required;
    size_t retry_out = 0;
    r = nh_identity_reader_getgrgid(reader, 200000, &gr, buffer, retry_needed,
                                    &retry_out);
    NH_CHECK(r == NH_IDENTITY_READER_FOUND);
    NH_CHECK(retry_out == retry_needed);
    NH_CHECK(gr.gid == 200000);
    NH_CHECK(strcmp(gr.name, "n_active") == 0);
  }

  /* --- 8. Disabled-but-projected user still resolves ------------------ *
   * The projection carries no status column, so the reader must return
   * FOUND for a "disabled" user just like any other. Disablement is a
   * concern of the authoritative store / auth broker, not of NSS. */
  required = 0;
  r = nh_identity_reader_getpwnam(reader, "n_disabled", &pw, buffer,
                                  sizeof buffer, &required);
  NH_CHECK(r == NH_IDENTITY_READER_FOUND);
  NH_CHECK(pw.uid == 200001);
  NH_CHECK(strcmp(pw.name, "n_disabled") == 0);
  NH_CHECK(strcmp(pw.home, "/home/n_disabled") == 0);

  required = 0;
  r = nh_identity_reader_getgrnam(reader, "n_disabled", &gr, buffer,
                                  sizeof buffer, &required);
  NH_CHECK(r == NH_IDENTITY_READER_FOUND);
  NH_CHECK(gr.gid == 200001);

  /* Final side-effect check across the whole run. */
  expect_no_sidecars(s.db);

  nh_identity_reader_close(reader);
  scratch_release(&s);
  puts("reader negative: PASS");
  return 0;
}
