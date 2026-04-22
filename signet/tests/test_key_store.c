/* SPDX-License-Identifier: MIT
 *
 * test_key_store.c - Tests for SignetKeyStore: provisioning, loading,
 *                     rotation, revocation, pubkey derivation.
 */

#include "signet/key_store.h"
#include "signet/audit_logger.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <sodium.h>

#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static char *make_temp_db_path(void) {
  char tmpl[] = "/tmp/signet-test-ks-XXXXXX.db";
  int fd = mkstemps(tmpl, 3);
  assert(fd >= 0);
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
  assert(ks != NULL);
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
  assert(rc == 0);
  assert(strlen(pubkey_hex) == 64);

  /* Load the key back. */
  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  bool ok = signet_key_store_load_agent_key(ks, "agent-alpha", &lk);
  assert(ok);
  assert(lk.secret_key != NULL);
  assert(lk.secret_key_len == 32);

  /* Verify pubkey matches what we got from provision. */
  char pub2[65] = {0};
  ok = signet_key_store_get_agent_pubkey(ks, "agent-alpha", pub2, sizeof(pub2));
  assert(ok);
  assert(strcmp(pubkey_hex, pub2) == 0);

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
  assert(!ok);

  char pub[65] = {0};
  ok = signet_key_store_get_agent_pubkey(ks, "no-such-agent", pub, sizeof(pub));
  assert(!ok);

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
  assert(rc == 0);

  /* Should no longer be loadable. */
  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  bool ok = signet_key_store_load_agent_key(ks, "revoke-me", &lk);
  assert(!ok);

  /* Revoking again should return 1 (not found). */
  rc = signet_key_store_revoke_agent(ks, "revoke-me");
  assert(rc == 1);

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
  signet_key_store_load_agent_key(ks, "rotate-me", &old_lk);
  uint8_t old_sk[32];
  memcpy(old_sk, old_lk.secret_key, 32);
  signet_loaded_key_clear(&old_lk);

  /* Rotate. */
  char new_pub[65] = {0};
  int rc = signet_key_store_rotate_agent(ks, "rotate-me", new_pub, sizeof(new_pub));
  assert(rc == 0);
  assert(strlen(new_pub) == 64);

  /* Pubkey should differ. */
  assert(strcmp(old_pub, new_pub) != 0);

  /* New key should be different. */
  SignetLoadedKey new_lk;
  memset(&new_lk, 0, sizeof(new_lk));
  signet_key_store_load_agent_key(ks, "rotate-me", &new_lk);
  assert(memcmp(old_sk, new_lk.secret_key, 32) != 0);
  signet_loaded_key_clear(&new_lk);

  /* Rotate non-existent agent should return 1. */
  rc = signet_key_store_rotate_agent(ks, "no-agent", new_pub, sizeof(new_pub));
  assert(rc == 1);

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
  assert(rc == 0);
  assert(count == 2);

  g_strfreev(ids);

  uint32_t cc = signet_key_store_cache_count(ks);
  assert(cc == 2);

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

  assert(!signet_key_store_is_open(NULL));
  assert(signet_key_store_cache_count(NULL) == 0);

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
  assert(rc == 0);
  assert(strlen(agent_pub) == 64);

  if (bunker_uri) {
    /* Should contain "bunker://" prefix. */
    assert(strstr(bunker_uri, "bunker://") != NULL);
    g_free(bunker_uri);
  }

  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_provision_bunker_uri: PASS\n");
}

int main(void) {
  assert(sodium_init() >= 0);

  test_null_safety();
  test_provision_and_load();
  test_load_nonexistent();
  test_revoke_agent();
  test_rotate_agent();
  test_list_agents();
  test_provision_bunker_uri();

  printf("All key store tests passed!\n");
  return 0;
}
