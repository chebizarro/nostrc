/* SPDX-License-Identifier: MIT
 *
 * test_backup_restore.c - Item 4: encrypted backup/restore, restart
 * durability, wrong-DB-key and tamper rejection.
 *
 * SQLCipher-dependent cases SKIP on plain-SQLite builds (mirroring
 * test_db_migration.c); restart durability and envelope-tamper cases run
 * everywhere because the envelope layer is independent of SQLCipher.
 */

#include "signet/store.h"
#include "signet/store_audit.h"
#include "signet/store_secrets.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <sodium.h>
#include <sqlite3.h>

#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define BACKUP_KEY "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"
#define WRONG_KEY  "1111111111111111111111111111111111111111111111111111111111111111"

#define NOW 2000000000

static char *temp_path(const char *tag) {
  char *tmpl = g_strdup_printf("/tmp/signet-backup-%s-XXXXXX.db", tag);
  int fd = mkstemps(tmpl, 3);
  assert(fd >= 0);
  close(fd);
  unlink(tmpl);
  return tmpl;
}

static void remove_sidecars(const char *db_path) {
  const char *sfx[] = { "-wal", "-shm", ".pre-restore", ".restoring" };
  for (size_t i = 0; i < G_N_ELEMENTS(sfx); i++) {
    char *p = g_strdup_printf("%s%s", db_path, sfx[i]);
    unlink(p);
    g_free(p);
  }
}

static SignetStore *open_store(const char *db_path, const char *key) {
  SignetStoreConfig cfg = { .db_path = db_path, .master_key = key };
  return signet_store_open(&cfg);
}

/* Populate: one agent, credential "alpha" rotated once (v1 archived),
 * credential "beta" revoked, two hash-chained audit entries. */
static void populate(SignetStore *store,
                     char alpha_id[70], char beta_id[70]) {
  uint8_t sk[32];
  randombytes_buf(sk, sizeof(sk));
  uint8_t pkraw[32];
  randombytes_buf(pkraw, sizeof(pkraw));
  char pubkey[65];
  sodium_bin2hex(pubkey, sizeof(pubkey), pkraw, sizeof(pkraw));
  assert(signet_store_put_agent_ex(store, "owner", sk, sizeof(sk),
                                   "connect-secret", pubkey,
                                   "provisioned", NOW) == 0);
  sodium_memzero(sk, sizeof(sk));

  SignetSecretMetadata md;
  memset(&md, 0, sizeof(md));
  assert(signet_store_create_secret(
      store, "owner", SIGNET_SECRET_API_TOKEN, "alpha",
      (const uint8_t *)"alpha-token-v1", strlen("alpha-token-v1"),
      "least-privilege", 0, "created", NULL, NOW, &md) == SIGNET_SECRET_OK);
  assert(strlen(md.id) < 70);
  memcpy(alpha_id, md.id, strlen(md.id) + 1);
  signet_secret_metadata_clear(&md);

  assert(signet_store_rotate_secret_ex(
      store, alpha_id, (const uint8_t *)"alpha-token-v2",
      strlen("alpha-token-v2"), false, 0, NOW + 1, NULL) == SIGNET_SECRET_OK);
  assert(signet_store_secret_history_count(store, alpha_id) == 1);

  assert(signet_store_create_secret(
      store, "owner", SIGNET_SECRET_CREDENTIAL, "beta",
      (const uint8_t *)"beta-value", strlen("beta-value"),
      NULL, 0, "imported", NULL, NOW, &md) == SIGNET_SECRET_OK);
  memcpy(beta_id, md.id, strlen(md.id) + 1);
  signet_secret_metadata_clear(&md);
  assert(signet_store_revoke_secret(store, beta_id, NOW + 2, NULL) ==
         SIGNET_SECRET_OK);

  assert(signet_audit_log_append(store, NOW, "owner", "credential_create",
                                 alpha_id, "test", NULL) == 0);
  assert(signet_audit_log_append(store, NOW + 1, "owner", "credential_rotate",
                                 alpha_id, "test", NULL) == 0);
}

/* Assert the fixture state created by populate() (point-in-time: alpha at v2). */
static void verify_populated(SignetStore *store,
                             const char *alpha_id, const char *beta_id) {
  SignetSecretRecord rec;
  memset(&rec, 0, sizeof(rec));
  assert(signet_store_get_secret_at(store, alpha_id, NOW + 3, &rec) ==
         SIGNET_SECRET_OK);
  assert(rec.payload_len == strlen("alpha-token-v2"));
  assert(memcmp(rec.payload, "alpha-token-v2", rec.payload_len) == 0);
  signet_secret_record_clear(&rec);

  /* Archived version survived. */
  assert(signet_store_secret_history_count(store, alpha_id) == 1);

  SignetSecretMetadata md;
  memset(&md, 0, sizeof(md));
  assert(signet_store_get_secret_metadata(store, alpha_id, NOW + 3, &md) ==
         SIGNET_SECRET_OK);
  assert(md.version == 2 && md.active_version == 2);
  assert(strcmp(md.provenance, "created") == 0);
  signet_secret_metadata_clear(&md);

  /* Revocation state survived. */
  assert(signet_store_get_secret_at(store, beta_id, NOW + 3, &rec) ==
         SIGNET_SECRET_REVOKED);
  assert(signet_store_get_secret_metadata(store, beta_id, NOW + 3, &md) ==
         SIGNET_SECRET_OK);
  assert(md.status == SIGNET_SECRET_STATUS_REVOKED);
  assert(strcmp(md.provenance, "imported") == 0);
  signet_secret_metadata_clear(&md);

  /* Audit hash chain is intact. */
  int64_t broken_id = 0;
  assert(signet_audit_verify_chain(store, 0, 0, &broken_id) == 0);
}

/* Restart durability: everything survives close + reopen (no backup involved).
 * Runs on plain and SQLCipher builds alike. */
static void test_restart_durability(void) {
  char *db = temp_path("durability");
  SignetStore *store = open_store(db, MASTER_KEY);
  assert(store != NULL);
  char alpha_id[70], beta_id[70];
  populate(store, alpha_id, beta_id);
  signet_store_close(store);

  store = open_store(db, MASTER_KEY);
  assert(store != NULL);
  verify_populated(store, alpha_id, beta_id);
  signet_store_close(store);

  unlink(db);
  g_free(db);
  printf("test_restart_durability: PASS\n");
}

/* Envelope tamper: a modified ciphertext blob must fail decryption, not
 * return garbage. Independent of SQLCipher. */
static void test_envelope_tamper_rejected(void) {
  char *db = temp_path("envtamper");
  SignetStore *store = open_store(db, MASTER_KEY);
  assert(store != NULL);
  char alpha_id[70], beta_id[70];
  populate(store, alpha_id, beta_id);

  sqlite3 *raw = signet_store_get_db(store);
  assert(raw != NULL);
  char *sql = sqlite3_mprintf(
      "UPDATE secrets SET payload = randomblob(length(payload)) WHERE id = %Q;",
      alpha_id);
  assert(sql != NULL);
  assert(sqlite3_exec(raw, sql, NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_free(sql);

  SignetSecretRecord rec;
  memset(&rec, 0, sizeof(rec));
  assert(signet_store_get_secret_at(store, alpha_id, NOW + 3, &rec) ==
         SIGNET_SECRET_ERROR);

  signet_store_close(store);
  unlink(db);
  g_free(db);
  printf("test_envelope_tamper_rejected: PASS\n");
}

/* Full encrypted backup -> restore roundtrip covering current AND archived
 * secrets, plus point-in-time semantics and restore-over-existing. */
static void test_backup_restore_roundtrip(void) {
  if (!signet_store_sqlcipher_available()) {
    printf("test_backup_restore_roundtrip: SKIP (no SQLCipher)\n");
    return;
  }
  char *db = temp_path("src");
  char *db2 = temp_path("restored");
  char *bak = temp_path("bak");

  SignetStore *store = open_store(db, MASTER_KEY);
  assert(store != NULL);
  assert(signet_store_is_encrypted(store));
  char alpha_id[70], beta_id[70];
  populate(store, alpha_id, beta_id);

  /* A legacy/corrupt custody envelope must not mask later valid credential
   * envelopes when restore authenticates the supplied master key. */
  sqlite3 *raw = signet_store_get_db(store);
  assert(raw != NULL);
  assert(sqlite3_exec(raw,
      "UPDATE agents SET encrypted_nsec = randomblob(length(encrypted_nsec));",
      NULL, NULL, NULL) == SQLITE_OK);

  assert(signet_store_backup(store, bak, BACKUP_KEY) == 0);
  assert(g_file_test(bak, G_FILE_TEST_EXISTS));
  /* The backup must be encrypted at rest and must carry 0600 permissions. */
  assert(!signet_store_file_is_plaintext_sqlite(bak));
  {
    GStatBuf st;
    assert(g_lstat(bak, &st) == 0);
    assert((st.st_mode & 0777) == 0600);
  }

  /* Mutate AFTER the backup: the backup must stay point-in-time (alpha v2). */
  assert(signet_store_rotate_secret_ex(
      store, alpha_id, (const uint8_t *)"alpha-token-v3",
      strlen("alpha-token-v3"), false, 0, NOW + 4, NULL) == SIGNET_SECRET_OK);
  signet_store_close(store);

  /* Restore to a fresh path. */
  assert(signet_store_restore_backup(bak, BACKUP_KEY, db2, MASTER_KEY) == 0);
  store = open_store(db2, MASTER_KEY);
  assert(store != NULL);
  assert(signet_store_is_encrypted(store));
  verify_populated(store, alpha_id, beta_id);
  signet_store_close(store);

  /* Restore OVER the mutated original: previous DB preserved as .pre-restore. */
  assert(signet_store_restore_backup(bak, BACKUP_KEY, db, MASTER_KEY) == 0);
  char *pre = g_strdup_printf("%s.pre-restore", db);
  assert(g_file_test(pre, G_FILE_TEST_EXISTS));
  store = open_store(db, MASTER_KEY);
  assert(store != NULL);
  verify_populated(store, alpha_id, beta_id); /* back to v2, history 1 */
  signet_store_close(store);

  unlink(pre);
  g_free(pre);
  remove_sidecars(db);
  remove_sidecars(db2);
  unlink(db);
  unlink(db2);
  unlink(bak);
  g_free(db);
  g_free(db2);
  g_free(bak);
  printf("test_backup_restore_roundtrip: PASS\n");
}

/* Wrong keys fail closed: restore with a wrong backup key, and open with a
 * wrong master key. */
static void test_wrong_keys_fail_closed(void) {
  if (!signet_store_sqlcipher_available()) {
    printf("test_wrong_keys_fail_closed: SKIP (no SQLCipher)\n");
    return;
  }
  char *db = temp_path("wrongkey");
  char *db2 = temp_path("wrongkey-target");
  char *bak = temp_path("wrongkey-bak");

  SignetStore *store = open_store(db, MASTER_KEY);
  assert(store != NULL);
  char alpha_id[70], beta_id[70];
  populate(store, alpha_id, beta_id);
  assert(signet_store_backup(store, bak, BACKUP_KEY) == 0);
  signet_store_close(store);

  /* Restore with the wrong backup key must fail and leave no target file. */
  assert(signet_store_restore_backup(bak, WRONG_KEY, db2, MASTER_KEY) == -1);
  assert(!g_file_test(db2, G_FILE_TEST_EXISTS));

  /* Restore with a WRONG-BUT-STRONG master key must fail closed: the SQLCipher
   * re-key would succeed, but the envelope blobs inside are bound to the
   * original master's DEK — restore must authenticate one and refuse. */
  assert(signet_store_restore_backup(bak, BACKUP_KEY, db2, WRONG_KEY) == -1);
  assert(!g_file_test(db2, G_FILE_TEST_EXISTS));

  /* backup_path aliasing the restore target (or its working files) is refused
   * — the swap steps would otherwise destroy the backup being restored. */
  assert(signet_store_restore_backup(bak, BACKUP_KEY, bak, MASTER_KEY) == -1);
  assert(g_file_test(bak, G_FILE_TEST_EXISTS));

  /* Opening the encrypted store with the wrong master key must fail. */
  assert(open_store(db, WRONG_KEY) == NULL);
  /* Opening the backup with the master key (not the backup key) must fail. */
  assert(open_store(bak, MASTER_KEY) == NULL);

  remove_sidecars(db);
  unlink(db);
  unlink(bak);
  g_free(db);
  g_free(db2);
  g_free(bak);
  printf("test_wrong_keys_fail_closed: PASS\n");
}

/* Backup refusals: weak backup key, existing target, read-only handle. */
static void test_backup_refusals(void) {
  if (!signet_store_sqlcipher_available()) {
    printf("test_backup_refusals: SKIP (no SQLCipher)\n");
    return;
  }
  char *db = temp_path("refusals");
  char *bak = temp_path("refusals-bak");

  SignetStore *store = open_store(db, MASTER_KEY);
  assert(store != NULL);
  char alpha_id[70], beta_id[70];
  populate(store, alpha_id, beta_id);

  /* Weak key (< 32 bytes of material) is refused. */
  assert(signet_store_backup(store, bak, "short-passphrase") == -1);
  assert(!g_file_test(bak, G_FILE_TEST_EXISTS));

  /* Existing target is never overwritten. */
  FILE *fp = fopen(bak, "wb");
  assert(fp && fputs("sentinel", fp) >= 0);
  fclose(fp);
  assert(signet_store_backup(store, bak, BACKUP_KEY) == -1);
  gchar *contents = NULL;
  assert(g_file_get_contents(bak, &contents, NULL, NULL));
  assert(strcmp(contents, "sentinel") == 0);
  g_free(contents);
  unlink(bak);
  signet_store_close(store);

  /* A read-only handle cannot produce a backup (ATTACH would be read-only). */
  SignetStoreConfig rcfg = { .db_path = db, .master_key = MASTER_KEY,
                             .read_only = true };
  store = signet_store_open(&rcfg);
  assert(store != NULL);
  assert(signet_store_backup(store, bak, BACKUP_KEY) == -1);
  assert(!g_file_test(bak, G_FILE_TEST_EXISTS));
  signet_store_close(store);

  remove_sidecars(db);
  unlink(db);
  g_free(db);
  g_free(bak);
  printf("test_backup_refusals: PASS\n");
}

/* File-level tamper: corrupting the encrypted database body must make the
 * keyed open fail (SQLCipher page MAC), never silently serve data. */
static void test_file_tamper_rejected(void) {
  if (!signet_store_sqlcipher_available()) {
    printf("test_file_tamper_rejected: SKIP (no SQLCipher)\n");
    return;
  }
  char *db = temp_path("filetamper");
  SignetStore *store = open_store(db, MASTER_KEY);
  assert(store != NULL);
  char alpha_id[70], beta_id[70];
  populate(store, alpha_id, beta_id);
  signet_store_close(store);

  /* Flip bytes inside page 1 (past the 16-byte salt) so the very first keyed
   * read fails its MAC/decrypt. */
  FILE *fp = fopen(db, "r+b");
  assert(fp != NULL);
  assert(fseek(fp, 100, SEEK_SET) == 0);
  const unsigned char junk[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xff, 0xff };
  assert(fwrite(junk, 1, sizeof(junk), fp) == sizeof(junk));
  fclose(fp);

  assert(open_store(db, MASTER_KEY) == NULL);

  remove_sidecars(db);
  unlink(db);
  g_free(db);
  printf("test_file_tamper_rejected: PASS\n");
}

int main(void) {
  assert(sodium_init() >= 0);
  test_restart_durability();
  test_envelope_tamper_rejected();
  test_backup_restore_roundtrip();
  test_wrong_keys_fail_closed();
  test_backup_refusals();
  test_file_tamper_rejected();
  printf("backup/restore tests: ALL PASS\n");
  return 0;
}
