/* SPDX-License-Identifier: MIT
 *
 * test_key_store.c - Tests for SignetKeyStore: provisioning, loading,
 *                     rotation, revocation, pubkey derivation.
 */

#include "signet/key_store.h"
#include "test_custody_key.h"
#include "signet/store.h"
#include "signet/audit_logger.h"

#include "test_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <sodium.h>
#include <sqlite3.h>

#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static char *make_temp_db_path(void) {
  char tmpl[] = "/tmp/signet-test-ks-XXXXXX.db";
  int fd = mkstemps(tmpl, 3);
  CHECK(fd >= 0);
  close(fd);
  unlink(tmpl);
  return g_strdup(tmpl);
}

static SignetKeyStore *open_test_ks(char **out_path) {
  char *db_path = make_temp_db_path();
  SignetAuditLoggerConfig alc = { .path = NULL, .to_stdout = false, .flush_each_write = false };
  SignetAuditLogger *audit = signet_audit_logger_new(&alc);

  SignetKeyStoreConfig cfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetKeyStore *ks = signet_key_store_new(audit, &cfg);
  CHECK(ks != NULL);
  if (out_path) *out_path = db_path; else g_free(db_path);
  /* Note: audit logger ownership not transferred; we leak it intentionally
   * for test brevity. */
  return ks;
}

/* ----------------------------- Provisioning ------------------------------ */

static void test_provision_and_load(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_test_ks(&db_path);

  char pubkey_hex[65] = {0};
  int rc = signet_key_store_provision_agent(ks, "agent-alpha",
                                             NULL, NULL, 0,
                                             pubkey_hex, sizeof(pubkey_hex),
                                             NULL);
  CHECK(rc == 0);
  CHECK(strlen(pubkey_hex) == 64);

  /* Load the key back. */
  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  bool ok = test_load_key_via_custody(ks, "agent-alpha", &lk);
  CHECK(ok);
  CHECK(lk.secret_key != NULL);
  CHECK(lk.secret_key_len == 32);

  /* Verify pubkey matches what we got from provision. */
  char pub2[65] = {0};
  ok = signet_key_store_get_agent_pubkey(ks, "agent-alpha", pub2, sizeof(pub2));
  CHECK(ok);
  CHECK(strcmp(pubkey_hex, pub2) == 0);

  signet_loaded_key_clear(&lk);
  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_provision_and_load: PASS\n");
}

static void test_load_nonexistent(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_test_ks(&db_path);

  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  bool ok = signet_key_store_load_agent_key(ks, "no-such-agent", &lk);
  CHECK(!ok);

  char pub[65] = {0};
  ok = signet_key_store_get_agent_pubkey(ks, "no-such-agent", pub, sizeof(pub));
  CHECK(!ok);

  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_load_nonexistent: PASS\n");
}

/* ----------------------------- Revocation -------------------------------- */

static void test_revoke_agent(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_test_ks(&db_path);

  char pub[65] = {0};
  signet_key_store_provision_agent(ks, "revoke-me", NULL, NULL, 0, pub, sizeof(pub), NULL);

  int rc = signet_key_store_revoke_agent(ks, "revoke-me");
  CHECK(rc == 0);

  /* Should no longer be loadable. */
  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  bool ok = signet_key_store_load_agent_key(ks, "revoke-me", &lk);
  CHECK(!ok);

  /* Revoking again should return 1 (not found). */
  rc = signet_key_store_revoke_agent(ks, "revoke-me");
  CHECK(rc == 1);

  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_revoke_agent: PASS\n");
}

/* ----------------------------- Rotation ---------------------------------- */

static void test_rotate_agent(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_test_ks(&db_path);

  char old_pub[65] = {0};
  signet_key_store_provision_agent(ks, "rotate-me", NULL, NULL, 0, old_pub, sizeof(old_pub), NULL);

  /* Capture old key. */
  SignetLoadedKey old_lk;
  memset(&old_lk, 0, sizeof(old_lk));
  test_load_key_via_custody(ks, "rotate-me", &old_lk);
  uint8_t old_sk[32];
  memcpy(old_sk, old_lk.secret_key, 32);
  signet_loaded_key_clear(&old_lk);

  /* Rotate. */
  char new_pub[65] = {0};
  int rc = signet_key_store_rotate_agent(ks, "rotate-me", new_pub, sizeof(new_pub));
  CHECK(rc == 0);
  CHECK(strlen(new_pub) == 64);

  /* Pubkey should differ. */
  CHECK(strcmp(old_pub, new_pub) != 0);

  /* New key should be different. */
  SignetLoadedKey new_lk;
  memset(&new_lk, 0, sizeof(new_lk));
  test_load_key_via_custody(ks, "rotate-me", &new_lk);
  CHECK(memcmp(old_sk, new_lk.secret_key, 32) != 0);
  signet_loaded_key_clear(&new_lk);

  /* Rotate non-existent agent should return 1. */
  rc = signet_key_store_rotate_agent(ks, "no-agent", new_pub, sizeof(new_pub));
  CHECK(rc == 1);

  sodium_memzero(old_sk, sizeof(old_sk));
  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_rotate_agent: PASS\n");
}

/* ----------------------------- List & Count ------------------------------- */

static void test_list_agents(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_test_ks(&db_path);

  char pub[65];
  signet_key_store_provision_agent(ks, "list-a", NULL, NULL, 0, pub, sizeof(pub), NULL);
  signet_key_store_provision_agent(ks, "list-b", NULL, NULL, 0, pub, sizeof(pub), NULL);

  char **ids = NULL;
  size_t count = 0;
  int rc = signet_key_store_list_agents(ks, &ids, &count);
  CHECK(rc == 0);
  CHECK(count == 2);

  g_strfreev(ids);

  uint32_t cc = signet_key_store_cache_count(ks);
  CHECK(cc == 2);

  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_list_agents: PASS\n");
}

/* ----------------------------- Null safety -------------------------------- */

static void test_null_safety(void) {
  signet_key_store_free(NULL);
  signet_loaded_key_clear(NULL);

  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  signet_loaded_key_clear(&lk); /* empty key, should be safe */

  CHECK(!signet_key_store_is_open(NULL));
  CHECK(signet_key_store_cache_count(NULL) == 0);

  printf("test_null_safety: PASS\n");
}

/* ----------------------------- Bunker URI -------------------------------- */

static void test_provision_bunker_uri(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_test_ks(&db_path);

  /* Generate a fake bunker pubkey. */
  unsigned char bunker_sk[crypto_sign_ed25519_SECRETKEYBYTES];
  unsigned char bunker_pk[crypto_sign_ed25519_PUBLICKEYBYTES];
  (void)bunker_sk; (void)bunker_pk;

  /* Use a fixed hex pubkey for the bunker. */
  char bunker_pub[65];
  memset(bunker_pub, 'a', 64);
  bunker_pub[64] = '\0';

  const char *relays[] = { "wss://relay.example.com", "wss://relay2.example.com" };
  char *bunker_uri = NULL;
  char agent_pub[65] = {0};
  int rc = signet_key_store_provision_agent(ks, "bunker-agent",
                                             bunker_pub, relays, 2,
                                             agent_pub, sizeof(agent_pub),
                                             &bunker_uri);
  CHECK(rc == 0);
  CHECK(strlen(agent_pub) == 64);

  if (bunker_uri) {
    /* Should contain "bunker://" prefix. */
    CHECK(strstr(bunker_uri, "bunker://") != NULL);
    g_free(bunker_uri);
  }

  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_provision_bunker_uri: PASS\n");
}

static int count_sign(const uint8_t secret_key[32], void *user_data) {
  CHECK(secret_key != NULL);
  int *count = user_data;
  (*count)++;
  return 0;
}

static int slow_sign_past_expiry(const uint8_t secret_key[32], void *user_data) {
  CHECK(secret_key != NULL);
  (void)user_data;
  sleep(2);
  return 0;
}

static void test_writer_client_cannot_be_provisioner(void) {
  static const char owner_a[] =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  static const char owner_b[] =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  char *path = NULL;
  SignetKeyStore *ks = open_test_ks(&path);
  SignetStore *store = signet_key_store_get_store(ks);
  char pubkey[65];
  CHECK(signet_key_store_provision_agent(ks, "service", NULL, NULL, 0,
                                         pubkey, sizeof(pubkey), NULL) == 0);
  CHECK(signet_store_grant_provisioner(store, owner_a, NULL, 1) == 0);
  int64_t epoch = 0, expiry = 0;
  CHECK(signet_key_store_writer_acquire(ks, "service", owner_a, 300,
                                        &epoch, &expiry) != 0);
  CHECK(signet_store_writer_is_fenced(store, "service") == 0);
  CHECK(signet_store_revoke_provisioner(store, owner_a) == 0);
  CHECK(signet_key_store_writer_acquire(ks, "service", owner_a, 300,
                                        &epoch, &expiry) == 0);
  CHECK(epoch == 1);
  CHECK(signet_store_grant_provisioner(store, owner_a, NULL, 2) != 0);
  CHECK(!signet_store_is_provisioner(store, owner_a));
  CHECK(signet_store_grant_provisioner(store,
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
      NULL, 2) != 0);
  CHECK(signet_key_store_writer_acquire(ks, "service", owner_b, 300,
                                        &epoch, &expiry) == 0);
  CHECK(epoch == 2);
  CHECK(signet_store_grant_provisioner(store, owner_a, NULL, 3) != 0);
  CHECK(!signet_store_is_provisioner(store, owner_a));
  int64_t revoked = 0;
  CHECK(signet_key_store_writer_revoke(ks, "service", &revoked) == 0);
  CHECK(signet_store_grant_provisioner(store, owner_b, NULL, 4) != 0);
  CHECK(!signet_store_is_provisioner(store, owner_b));
  signet_key_store_free(ks);
  unlink(path);
  g_free(path);
  printf("test_writer_client_cannot_be_provisioner: PASS\n");
}

static void test_fenced_nip44_custody(void) {
  static const char owner_a[] =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  static const char owner_b[] =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  char *path = NULL;
  SignetKeyStore *ks = open_test_ks(&path);
  char pubkey[65];
  CHECK(signet_key_store_provision_agent(ks, "service", NULL, NULL, 0,
                                         pubkey, sizeof(pubkey), NULL) == 0);
  int64_t epoch = 0, expiry = 0;
  CHECK(signet_key_store_writer_acquire(ks, "service", owner_a, 300,
                                        &epoch, &expiry) == 0);
  char *ciphertext = NULL, *plain = NULL;
  CHECK(signet_key_store_crypt_legacy(ks, "service", "nip44_encrypt",
                                      pubkey, "secret", &ciphertext) != 0);
  CHECK(ciphertext == NULL);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_b, epoch,
      "nip44_encrypt", pubkey, "secret", &ciphertext) != 0);
  CHECK(ciphertext == NULL);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch + 1,
      "nip44_encrypt", pubkey, "secret", &ciphertext) != 0);
  CHECK(ciphertext == NULL);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip04_encrypt", pubkey, "secret", &ciphertext) != 0);
  CHECK(ciphertext == NULL);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip44_encrypt", pubkey, "secret", &ciphertext) == 0);
  CHECK(ciphertext && ciphertext[0]);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip44_decrypt", pubkey, ciphertext, &plain) == 0);
  CHECK(strcmp(plain, "secret") == 0);
  g_free(plain);
  g_free(ciphertext);

  const uint8_t binary[] = {0, 0xff, 0x80, 0x01, 0};
  char *encoded = g_base64_encode(binary, sizeof(binary));
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip44_encrypt_b64", pubkey, encoded, &ciphertext) == 0);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip44_decrypt_b64", pubkey, ciphertext, &plain) == 0);
  CHECK(strcmp(plain, encoded) == 0);
  g_free(plain);
  plain = NULL;
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip44_decrypt", pubkey, ciphertext, &plain) == -2);
  CHECK(plain == NULL);
  g_free(encoded);
  g_free(ciphertext);
  ciphertext = NULL;
  const uint8_t invalid_utf8[] = {0xff, 0xfe, 0x80};
  encoded = g_base64_encode(invalid_utf8, sizeof(invalid_utf8));
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip44_encrypt_b64", pubkey, encoded, &ciphertext) == 0);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip44_decrypt", pubkey, ciphertext, &plain) == -2);
  CHECK(plain == NULL);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip44_decrypt_b64", pubkey, ciphertext, &plain) == 0);
  CHECK(strcmp(plain, encoded) == 0);
  g_free(plain);
  g_free(encoded);
  g_free(ciphertext);
  ciphertext = NULL;
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip44_encrypt_b64", pubkey, "not base64!", &ciphertext) != 0);
  CHECK(ciphertext == NULL);

  signet_key_store_free(ks);
  SignetKeyStoreConfig cfg = {.db_path = path, .master_key = MASTER_KEY};
  ks = signet_key_store_new(NULL, &cfg);
  CHECK(ks != NULL);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip44_encrypt", pubkey, "after restart", &ciphertext) == 0);
  sqlite3 *db = signet_store_get_db(signet_key_store_get_store(ks));
  CHECK(sqlite3_exec(db,
      "CREATE TRIGGER fail_crypto_commit BEFORE UPDATE OF observed_at "
      "ON agent_writer_leases BEGIN SELECT RAISE(ABORT,'forced failure'); END;",
      NULL, NULL, NULL) == SQLITE_OK);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip44_decrypt", pubkey, ciphertext, &plain) != 0);
  CHECK(plain == NULL);
  char *failed_ciphertext = NULL;
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip44_encrypt", pubkey, "uncommitted", &failed_ciphertext) != 0);
  CHECK(failed_ciphertext == NULL);
  CHECK(sqlite3_exec(db, "DROP TRIGGER fail_crypto_commit;",
                     NULL, NULL, NULL) == SQLITE_OK);
  g_free(ciphertext);
  ciphertext = NULL;
  CHECK(sqlite3_exec(db, "PRAGMA query_only=ON;", NULL, NULL, NULL) == SQLITE_OK);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch,
      "nip44_encrypt", pubkey, "db error", &ciphertext) != 0);
  CHECK(ciphertext == NULL);
  CHECK(sqlite3_exec(db, "PRAGMA query_only=OFF;", NULL, NULL, NULL) == SQLITE_OK);
  CHECK(signet_key_store_writer_acquire(ks, "service", owner_b, 300,
                                        &epoch, &expiry) == 0);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_a, epoch - 1,
      "nip44_encrypt", pubkey, "stale", &ciphertext) != 0);
  CHECK(ciphertext == NULL);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_b, epoch,
      "nip44_encrypt", pubkey, "current", &ciphertext) == 0);
  g_free(ciphertext);
  ciphertext = NULL;
  CHECK(sqlite3_exec(db,
      "UPDATE agent_writer_leases SET expires_at=1 WHERE agent_id='service';",
      NULL, NULL, NULL) == SQLITE_OK);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_b, epoch,
      "nip44_encrypt", pubkey, "expired", &ciphertext) != 0);
  CHECK(ciphertext == NULL);
  int64_t revoked = 0;
  CHECK(signet_key_store_writer_revoke(ks, "service", &revoked) == 0);
  CHECK(signet_key_store_crypt_nip44(ks, "service", owner_b, epoch,
      "nip44_encrypt", pubkey, "revoked", &ciphertext) != 0);
  CHECK(ciphertext == NULL);
  signet_key_store_free(ks);
  unlink(path);
  g_free(path);
  printf("test_fenced_nip44_custody: PASS\n");
}

static void test_pre_history_writer_db_fails_closed(void) {
  static const char owner[] =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  char *path = NULL;
  SignetKeyStore *ks = open_test_ks(&path);
  char pubkey[65];
  CHECK(signet_key_store_provision_agent(ks, "service", NULL, NULL, 0,
                                         pubkey, sizeof(pubkey), NULL) == 0);
  int64_t epoch = 0, expiry = 0;
  CHECK(signet_key_store_writer_acquire(ks, "service", owner, 300,
                                        &epoch, &expiry) == 0);
  signet_key_store_free(ks);
  sqlite3 *db = NULL;
  CHECK(sqlite3_open(path, &db) == SQLITE_OK);
  CHECK(sqlite3_exec(db, "DROP TABLE writer_client_keys;", NULL, NULL, NULL)
        == SQLITE_OK);
  sqlite3_close(db);
  SignetKeyStoreConfig cfg = {.db_path = path, .master_key = MASTER_KEY};
  CHECK(signet_key_store_new(NULL, &cfg) == NULL);
  CHECK(sqlite3_open(path, &db) == SQLITE_OK);
  sqlite3_stmt *history = NULL;
  CHECK(sqlite3_prepare_v2(db,
      "SELECT 1 FROM sqlite_master WHERE type='table' AND name='writer_client_keys';",
      -1, &history, NULL) == SQLITE_OK);
  CHECK(sqlite3_step(history) == SQLITE_DONE);
  sqlite3_finalize(history);
  sqlite3_close(db);
  CHECK(signet_key_store_new(NULL, &cfg) == NULL);
  unlink(path);
  g_free(path);
  printf("test_pre_history_writer_db_fails_closed: PASS\n");
}

static void test_writer_fence_persists_and_fails_closed(void) {
  static const char owner_a[] =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  static const char owner_b[] =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  char *path = NULL;
  SignetKeyStore *ks = open_test_ks(&path);
  char pubkey[65];
  CHECK(signet_key_store_provision_agent(ks, "service", NULL, NULL, 0,
                                         pubkey, sizeof(pubkey), NULL) == 0);
  SignetLoadedKey raw = {0};
  CHECK(!signet_key_store_load_agent_key(ks, "service", &raw));
  char *ciphertext = NULL;
  CHECK(signet_key_store_crypt_legacy(ks, "service", "nip44_encrypt",
      pubkey, "pre-fence", &ciphertext) == 0);
  g_free(ciphertext);
  int count = 0;
  CHECK(signet_key_store_with_signing_key(ks, "service", NULL, 0,
                                          count_sign, &count) == 0);
  int64_t epoch_a = 0, expiry = 0;
  CHECK(signet_key_store_writer_acquire(ks, "service", owner_a, 300,
                                        &epoch_a, &expiry) == 0);
  CHECK(epoch_a == 1 && expiry > 0);
  CHECK(!signet_key_store_load_agent_key(ks, "service", &raw));
  CHECK(signet_key_store_crypt_legacy(ks, "service", "nip44_encrypt",
      pubkey, "denied", &ciphertext) != 0);
  CHECK(ciphertext == NULL);
  CHECK(signet_key_store_with_signing_key(ks, "service", NULL, 0,
                                          count_sign, &count) != 0);
  CHECK(signet_key_store_with_signing_key(ks, "service", owner_b, epoch_a,
                                          count_sign, &count) != 0);
  CHECK(signet_key_store_with_signing_key(ks, "service", owner_a, epoch_a,
                                          count_sign, &count) == 0);
  CHECK(count == 2);
  CHECK(signet_key_store_writer_renew(ks, "service", owner_b, epoch_a,
                                      300, &expiry) != 0);
  CHECK(signet_key_store_writer_renew(ks, "service", owner_a, epoch_a,
                                      300, &expiry) == 0);

  signet_key_store_free(ks);
  SignetKeyStoreConfig cfg = {.db_path = path, .master_key = MASTER_KEY};
  ks = signet_key_store_new(NULL, &cfg);
  CHECK(ks != NULL);
  CHECK(!signet_key_store_load_agent_key(ks, "service", &raw));
  CHECK(signet_key_store_with_signing_key(ks, "service", owner_a, epoch_a,
                                          count_sign, &count) == 0);
  int64_t epoch_b = 0;
  CHECK(signet_key_store_writer_acquire(ks, "service", owner_b, 300,
                                        &epoch_b, &expiry) == 0);
  CHECK(epoch_b == epoch_a + 1);
  CHECK(signet_key_store_with_signing_key(ks, "service", owner_a, epoch_a,
                                          count_sign, &count) != 0);
  CHECK(signet_key_store_writer_renew(ks, "service", owner_a, epoch_a,
                                      300, &expiry) != 0);
  CHECK(signet_key_store_with_signing_key(ks, "service", owner_b, epoch_b,
                                          count_sign, &count) == 0);
  int64_t revoked_epoch = 0;
  CHECK(signet_key_store_writer_revoke(ks, "service", &revoked_epoch) == 0);
  CHECK(revoked_epoch == epoch_b + 1);
  CHECK(signet_key_store_with_signing_key(ks, "service", owner_b, epoch_b,
                                          count_sign, &count) != 0);
  CHECK(signet_key_store_with_signing_key(ks, "service", NULL, 0,
                                          count_sign, &count) != 0);
  int64_t epoch_c = 0;
  CHECK(signet_key_store_writer_acquire(ks, "service", owner_a, 300,
                                        &epoch_c, &expiry) == 0);
  CHECK(epoch_c == revoked_epoch + 1);

  sqlite3 *db = signet_store_get_db(signet_key_store_get_store(ks));
  CHECK(sqlite3_exec(db,
      "UPDATE agent_writer_leases SET expires_at=1 WHERE agent_id='service';",
      NULL, NULL, NULL) == SQLITE_OK);
  CHECK(signet_key_store_with_signing_key(ks, "service", owner_a, epoch_c,
                                          count_sign, &count) != 0);
  CHECK(signet_key_store_writer_renew(ks, "service", owner_a, epoch_c,
                                      300, &expiry) != 0);
  CHECK(signet_key_store_writer_acquire(ks, "service", owner_a, 1,
                                        &epoch_c, &expiry) == 0);
  CHECK(signet_key_store_with_signing_key(ks, "service", owner_a, epoch_c,
                                          slow_sign_past_expiry, NULL) != 0);
  CHECK(signet_key_store_writer_acquire(ks, "service", owner_a, 300,
                                        &epoch_c, &expiry) == 0);
  CHECK(sqlite3_exec(db,
      "UPDATE agent_writer_leases SET observed_at=strftime('%s','now')+3600 WHERE agent_id='service';",
      NULL, NULL, NULL) == SQLITE_OK);
  signet_key_store_free(ks);
  ks = signet_key_store_new(NULL, &cfg);
  CHECK(ks != NULL);
  db = signet_store_get_db(signet_key_store_get_store(ks));
  CHECK(signet_key_store_with_signing_key(ks, "service", owner_a, epoch_c,
                                          count_sign, &count) != 0);
  CHECK(signet_key_store_writer_renew(ks, "service", owner_a, epoch_c,
                                      300, &expiry) != 0);
  CHECK(signet_key_store_writer_acquire(ks, "service", owner_b, 300,
                                        &epoch_c, &expiry) != 0);
  CHECK(signet_key_store_writer_revoke(ks, "service", &revoked_epoch) == 0);
  CHECK(sqlite3_exec(db, "DROP TABLE agent_writer_leases;",
                     NULL, NULL, NULL) == SQLITE_OK);
  CHECK(signet_key_store_with_signing_key(ks, "service", NULL, 0,
                                          count_sign, &count) != 0);
  CHECK(!signet_key_store_load_agent_key(ks, "service", &raw));
  signet_key_store_free(ks);
  ks = signet_key_store_new(NULL, &cfg);
  CHECK(ks != NULL);
  CHECK(signet_key_store_with_signing_key(ks, "service", NULL, 0,
                                          count_sign, &count) != 0);
  CHECK(signet_key_store_writer_acquire(ks, "service", owner_a, 300,
                                        &epoch_c, &expiry) != 0);
  signet_key_store_free(ks);
  unlink(path);
  g_free(path);
  printf("test_writer_fence_persists_and_fails_closed: PASS\n");
}

typedef struct {
  SignetKeyStore *signer;
  SignetKeyStore *transfer;
  GMutex mu;
  GCond cond;
  bool signing;
  bool release_sign;
  int sign_rc;
  int acquire_rc;
  int64_t new_epoch;
} WriterRace;

static int held_sign(const uint8_t secret_key[32], void *user_data) {
  WriterRace *race = user_data;
  CHECK(secret_key != NULL);
  g_mutex_lock(&race->mu);
  race->signing = true;
  g_cond_broadcast(&race->cond);
  while (!race->release_sign) g_cond_wait(&race->cond, &race->mu);
  g_mutex_unlock(&race->mu);
  return 0;
}

static gpointer run_held_sign(gpointer user_data) {
  WriterRace *race = user_data;
  static const char owner[] =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  race->sign_rc = signet_key_store_with_signing_key(race->signer,
      "service", owner, 1, held_sign, race);
  return NULL;
}

static gpointer run_transfer(gpointer user_data) {
  WriterRace *race = user_data;
  static const char owner[] =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  int64_t expiry = 0;
  race->acquire_rc = signet_key_store_writer_acquire(race->transfer,
      "service", owner, 300, &race->new_epoch, &expiry);
  return NULL;
}

static void test_writer_transfer_serializes_with_sign(void) {
  static const char owner_a[] =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  char *path = NULL;
  SignetKeyStore *first = open_test_ks(&path);
  char pubkey[65];
  CHECK(signet_key_store_provision_agent(first, "service", NULL, NULL, 0,
                                         pubkey, sizeof(pubkey), NULL) == 0);
  int64_t epoch = 0, expiry = 0;
  CHECK(signet_key_store_writer_acquire(first, "service", owner_a, 300,
                                        &epoch, &expiry) == 0);
  CHECK(epoch == 1);
  SignetKeyStoreConfig cfg = {.db_path = path, .master_key = MASTER_KEY};
  SignetKeyStore *second = signet_key_store_new(NULL, &cfg);
  CHECK(second != NULL);
  WriterRace race = {.signer = first, .transfer = second};
  g_mutex_init(&race.mu);
  g_cond_init(&race.cond);
  GThread *sign_thread = g_thread_new("held-sign", run_held_sign, &race);
  g_mutex_lock(&race.mu);
  while (!race.signing) g_cond_wait(&race.cond, &race.mu);
  g_mutex_unlock(&race.mu);
  GThread *transfer_thread = g_thread_new("transfer", run_transfer, &race);
  /* The callback is still inside the write transaction. Release it, then
   * observe that transfer advances exactly one epoch and stales owner A. */
  g_mutex_lock(&race.mu);
  race.release_sign = true;
  g_cond_broadcast(&race.cond);
  g_mutex_unlock(&race.mu);
  g_thread_join(sign_thread);
  g_thread_join(transfer_thread);
  CHECK(race.sign_rc == 0 && race.acquire_rc == 0);
  CHECK(race.new_epoch == epoch + 1);
  int count = 0;
  CHECK(signet_key_store_with_signing_key(first, "service", owner_a, epoch,
                                          count_sign, &count) != 0);
  CHECK(count == 0);
  g_cond_clear(&race.cond);
  g_mutex_clear(&race.mu);
  signet_key_store_free(second);
  signet_key_store_free(first);
  unlink(path);
  g_free(path);
  printf("test_writer_transfer_serializes_with_sign: PASS\n");
}

typedef struct {
  SignetKeyStore *crypto_store;
  SignetKeyStore *transfer_store;
  const char *peer;
  GMutex mu;
  GCond cond;
  bool hold_once;
  bool in_commit;
  bool release_commit;
  bool transfer_started;
  bool transfer_done;
  int crypto_rc;
  int transfer_rc;
  int64_t transfer_epoch;
} CryptoRace;

static void hold_nip44_commit(sqlite3_context *ctx, int argc,
                              sqlite3_value **argv) {
  (void)argc; (void)argv;
  CryptoRace *race = sqlite3_user_data(ctx);
  g_mutex_lock(&race->mu);
  if (race->hold_once) {
    race->hold_once = false;
    race->in_commit = true;
    g_cond_broadcast(&race->cond);
    while (!race->release_commit) g_cond_wait(&race->cond, &race->mu);
  }
  g_mutex_unlock(&race->mu);
  sqlite3_result_int(ctx, 1);
}

static gpointer run_fenced_crypto(gpointer data) {
  CryptoRace *race = data;
  static const char owner[] =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  char *result = NULL;
  race->crypto_rc = signet_key_store_crypt_nip44(race->crypto_store,
      "service", owner, 1, "nip44_encrypt", race->peer,
      "transaction race", &result);
  g_free(result);
  return NULL;
}

static gpointer run_crypto_transfer(gpointer data) {
  CryptoRace *race = data;
  static const char owner[] =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  g_mutex_lock(&race->mu);
  race->transfer_started = true;
  g_cond_broadcast(&race->cond);
  g_mutex_unlock(&race->mu);
  int64_t expiry = 0;
  race->transfer_rc = signet_key_store_writer_acquire(race->transfer_store,
      "service", owner, 300, &race->transfer_epoch, &expiry);
  g_mutex_lock(&race->mu);
  race->transfer_done = true;
  g_cond_broadcast(&race->cond);
  g_mutex_unlock(&race->mu);
  return NULL;
}

static void test_fenced_nip44_transfer_serializes(void) {
  static const char owner[] =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  char *path = NULL;
  SignetKeyStore *first = open_test_ks(&path);
  char pubkey[65];
  CHECK(signet_key_store_provision_agent(first, "service", NULL, NULL, 0,
                                         pubkey, sizeof(pubkey), NULL) == 0);
  int64_t epoch = 0, expiry = 0;
  CHECK(signet_key_store_writer_acquire(first, "service", owner, 300,
                                        &epoch, &expiry) == 0);
  SignetKeyStoreConfig cfg = {.db_path = path, .master_key = MASTER_KEY};
  SignetKeyStore *second = signet_key_store_new(NULL, &cfg);
  CHECK(second != NULL);
  CryptoRace race = {.crypto_store = first, .transfer_store = second,
                     .peer = pubkey, .hold_once = true};
  g_mutex_init(&race.mu);
  g_cond_init(&race.cond);
  sqlite3 *first_db = signet_store_get_db(signet_key_store_get_store(first));
  sqlite3 *second_db = signet_store_get_db(signet_key_store_get_store(second));
  CHECK(sqlite3_create_function(first_db, "hold_nip44", 0, SQLITE_UTF8,
      &race, hold_nip44_commit, NULL, NULL) == SQLITE_OK);
  CHECK(sqlite3_create_function(second_db, "hold_nip44", 0, SQLITE_UTF8,
      &race, hold_nip44_commit, NULL, NULL) == SQLITE_OK);
  CHECK(sqlite3_exec(first_db,
      "CREATE TRIGGER hold_nip44_observed BEFORE UPDATE OF observed_at "
      "ON agent_writer_leases BEGIN SELECT hold_nip44(); END;",
      NULL, NULL, NULL) == SQLITE_OK);
  GThread *crypto = g_thread_new("fenced-nip44", run_fenced_crypto, &race);
  g_mutex_lock(&race.mu);
  while (!race.in_commit) g_cond_wait(&race.cond, &race.mu);
  g_mutex_unlock(&race.mu);
  GThread *transfer = g_thread_new("nip44-transfer", run_crypto_transfer, &race);
  g_mutex_lock(&race.mu);
  while (!race.transfer_started) g_cond_wait(&race.cond, &race.mu);
  CHECK(!race.transfer_done);
  race.release_commit = true;
  g_cond_broadcast(&race.cond);
  g_mutex_unlock(&race.mu);
  g_thread_join(crypto);
  g_thread_join(transfer);
  CHECK(race.crypto_rc == 0);
  CHECK(race.transfer_rc == 0 && race.transfer_epoch == epoch + 1);
  char *result = NULL;
  CHECK(signet_key_store_crypt_nip44(first, "service", owner, epoch,
      "nip44_encrypt", pubkey, "stale", &result) != 0);
  CHECK(result == NULL);
  CHECK(sqlite3_exec(first_db, "DROP TRIGGER hold_nip44_observed;",
                     NULL, NULL, NULL) == SQLITE_OK);
  g_cond_clear(&race.cond);
  g_mutex_clear(&race.mu);
  signet_key_store_free(second);
  signet_key_store_free(first);
  unlink(path);
  g_free(path);
  printf("test_fenced_nip44_transfer_serializes: PASS\n");
}

int main(void) {
  CHECK(sodium_init() >= 0);

  test_null_safety();
  test_provision_and_load();
  test_load_nonexistent();
  test_revoke_agent();
  test_rotate_agent();
  test_list_agents();
  test_provision_bunker_uri();
  test_writer_client_cannot_be_provisioner();
  test_fenced_nip44_custody();
  test_pre_history_writer_db_fails_closed();
  test_writer_fence_persists_and_fails_closed();
  test_writer_transfer_serializes_with_sign();
  test_fenced_nip44_transfer_serializes();

  printf("All key store tests passed!\n");
  return 0;
}
