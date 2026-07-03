/* SPDX-License-Identifier: MIT
 *
 * test_store_lifecycle.c - End-to-end store behavior that guards the P0/P1
 * fixes in this branch:
 *   1. Audit hash-chain integrity + tamper detection (signet_audit_verify_chain).
 *   2. Bootstrap token single-use (put -> verify -> consume -> ALREADY_USED).
 *   3. Revocation deny-list precedence (signet_revoke_agent adds the pubkey to
 *      the deny list and wipes the key).
 */

#include "signet/store.h"
#include "signet/store_audit.h"
#include "signet/store_tokens.h"
#include "signet/revocation.h"
#include "signet/key_store.h"
#include "signet/audit_logger.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <sodium.h>
#include <sqlite3.h>

#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static char *make_temp_db_path(void) {
  char tmpl[] = "/tmp/signet-test-life-XXXXXX.db";
  int fd = mkstemps(tmpl, 3);
  assert(fd >= 0);
  close(fd);
  unlink(tmpl);
  return g_strdup(tmpl);
}

static void cleanup_db(char *db) {
  unlink(db);
  char *wal = g_strdup_printf("%s-wal", db); unlink(wal); g_free(wal);
  char *shm = g_strdup_printf("%s-shm", db); unlink(shm); g_free(shm);
  g_free(db);
}

/* 1. Audit hash chain: intact chain verifies; tampering a row breaks it. */
static void test_audit_hash_chain(void) {
  char *db = make_temp_db_path();
  SignetStoreConfig cfg = { .db_path = db, .master_key = MASTER_KEY };
  SignetStore *store = signet_store_open(&cfg);
  assert(store);

  assert(signet_audit_log_append(store, 1000, "agent-1", "provision", NULL, "test", "{\"x\":1}") == 0);
  assert(signet_audit_log_append(store, 1001, "agent-1", "sign",      NULL, "test", "{\"x\":2}") == 0);
  assert(signet_audit_log_append(store, 1002, "agent-2", "revoke",    NULL, "test", "{\"x\":3}") == 0);
  assert(signet_audit_log_count(store) == 3);

  int64_t broken = -1;
  assert(signet_audit_verify_chain(store, 0, 0, &broken) == 0); /* intact */

  /* Tamper a committed row's detail directly. The stored entry_hash was
   * computed over the original detail, so recomputation must now diverge. */
  sqlite3 *db2 = signet_store_get_db(store);
  assert(sqlite3_exec(db2, "UPDATE audit_log SET detail='{\"x\":99}' WHERE id=2;",
                      NULL, NULL, NULL) == SQLITE_OK);

  broken = -1;
  int rc = signet_audit_verify_chain(store, 0, 0, &broken);
  assert(rc == 1);        /* chain reported broken */
  assert(broken >= 2);    /* at/after the tampered row */

  signet_store_close(store);
  cleanup_db(db);
  printf("test_audit_hash_chain: PASS\n");
}

/* 2. Bootstrap token single-use (guards nostrc-ufh). */
static void test_bootstrap_token_single_use(void) {
  char *db = make_temp_db_path();
  SignetStoreConfig cfg = { .db_path = db, .master_key = MASTER_KEY };
  SignetStore *store = signet_store_open(&cfg);
  assert(store);

  const char *th  = "aa11bb22cc33dd44ee55ff66aa11bb22cc33dd44ee55ff66aa11bb22cc33dd44";
  const char *pub = "11223344556677889900aabbccddeeff11223344556677889900aabbccddeeff";
  assert(signet_store_put_bootstrap_token(store, th, "agent-b", pub, 1000, 1000 + 3600) == 0);

  /* Valid before use. */
  assert(signet_store_verify_bootstrap_token(store, th, "agent-b", pub, 1100) == SIGNET_TOKEN_OK);

  /* Consume marks it used (this is what POST /bootstrap now does). */
  assert(signet_store_consume_bootstrap_token(store, th, 1100) == 0);

  /* Replay is now rejected as already-used. */
  assert(signet_store_verify_bootstrap_token(store, th, "agent-b", pub, 1100) == SIGNET_TOKEN_ALREADY_USED);

  /* Consuming a second time cannot transition again. */
  assert(signet_store_consume_bootstrap_token(store, th, 1100) == -1);

  signet_store_close(store);
  cleanup_db(db);
  printf("test_bootstrap_token_single_use: PASS\n");
}

/* 3. Revocation deny-list precedence (guards nostrc-sd3). */
static void test_revoke_deny_precedence(void) {
  char *db = make_temp_db_path();
  SignetAuditLoggerConfig alc = { .path = NULL, .to_stdout = false, .flush_each_write = false };
  SignetAuditLogger *audit = signet_audit_logger_new(&alc);
  SignetKeyStoreConfig kcfg = { .db_path = db, .master_key = MASTER_KEY };
  SignetKeyStore *ks = signet_key_store_new(audit, &kcfg);
  assert(ks);

  char pub[65] = {0};
  assert(signet_key_store_provision_agent(ks, "victim", NULL, NULL, 0, pub, sizeof(pub), NULL) == 0);

  SignetStore *store = signet_key_store_get_store(ks);
  assert(store);
  SignetDenyList *deny = signet_deny_list_new(store);
  assert(deny);
  assert(!signet_deny_list_contains(deny, pub)); /* not denied yet */

  /* Full revocation must deny-list the pubkey AND wipe the key. */
  int rc = signet_revoke_agent(store, ks, deny, audit, "victim", pub, "test", 2000);
  assert(rc == 0);
  assert(signet_deny_list_contains(deny, pub)); /* deny precedence now applies */

  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  assert(!signet_key_store_load_agent_key(ks, "victim", &lk)); /* key gone */

  /* Un-deny works too. */
  assert(signet_deny_list_remove(deny, pub) == 0);
  assert(!signet_deny_list_contains(deny, pub));

  signet_deny_list_free(deny);
  signet_key_store_free(ks);
  cleanup_db(db);
  printf("test_revoke_deny_precedence: PASS\n");
}

int main(void) {
  assert(sodium_init() >= 0);
  test_audit_hash_chain();
  test_bootstrap_token_single_use();
  test_revoke_deny_precedence();
  printf("All store lifecycle tests passed!\n");
  return 0;
}
