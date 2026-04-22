/* SPDX-License-Identifier: MIT
 *
 * test_store.c - Tests for SignetStore agent/secret/lease CRUD operations.
 */

#include "signet/store.h"
#include "signet/store_secrets.h"
#include "signet/store_leases.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>
#include <sodium.h>

#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static char *make_temp_db_path(void) {
  char tmpl[] = "/tmp/signet-test-store-XXXXXX.db";
  int fd = mkstemps(tmpl, 3);
  assert(fd >= 0);
  close(fd);
  unlink(tmpl);
  return g_strdup(tmpl);
}

static SignetStore *open_test_store(char **out_path) {
  char *db_path = make_temp_db_path();
  SignetStoreConfig cfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetStore *store = signet_store_open(&cfg);
  assert(store != NULL);
  if (out_path) *out_path = db_path; else g_free(db_path);
  return store;
}

/* ----------------------------- Agent CRUD -------------------------------- */

static void test_agent_put_get(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);

  uint8_t sk[32];
  randombytes_buf(sk, sizeof(sk));
  int64_t now = (int64_t)time(NULL);

  int rc = signet_store_put_agent(store, "test-agent-1", sk, 32, "secret-abc", now);
  assert(rc == 0);

  SignetAgentRecord rec;
  memset(&rec, 0, sizeof(rec));
  rc = signet_store_get_agent(store, "test-agent-1", &rec);
  assert(rc == 0);
  assert(rec.agent_id != NULL);
  assert(strcmp(rec.agent_id, "test-agent-1") == 0);
  assert(rec.secret_key_len == 32);
  assert(memcmp(rec.secret_key, sk, 32) == 0);
  assert(rec.connect_secret != NULL);
  assert(strcmp(rec.connect_secret, "secret-abc") == 0);

  signet_agent_record_clear(&rec);
  sodium_memzero(sk, sizeof(sk));
  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_agent_put_get: PASS\n");
}

static void test_agent_not_found(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);

  SignetAgentRecord rec;
  memset(&rec, 0, sizeof(rec));
  int rc = signet_store_get_agent(store, "nonexistent", &rec);
  assert(rc == 1); /* not found */

  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_agent_not_found: PASS\n");
}

static void test_agent_delete(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);

  uint8_t sk[32];
  randombytes_buf(sk, sizeof(sk));
  int64_t now = (int64_t)time(NULL);
  signet_store_put_agent(store, "del-agent", sk, 32, NULL, now);

  int rc = signet_store_delete_agent(store, "del-agent");
  assert(rc == 0);

  SignetAgentRecord rec;
  memset(&rec, 0, sizeof(rec));
  rc = signet_store_get_agent(store, "del-agent", &rec);
  assert(rc == 1); /* not found after delete */

  /* Delete non-existent returns 1. */
  rc = signet_store_delete_agent(store, "del-agent");
  assert(rc == 1);

  sodium_memzero(sk, sizeof(sk));
  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_agent_delete: PASS\n");
}

static void test_agent_list(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);

  uint8_t sk[32];
  randombytes_buf(sk, sizeof(sk));
  int64_t now = (int64_t)time(NULL);
  signet_store_put_agent(store, "agent-a", sk, 32, NULL, now);
  signet_store_put_agent(store, "agent-b", sk, 32, NULL, now);
  signet_store_put_agent(store, "agent-c", sk, 32, NULL, now);

  char **ids = NULL;
  size_t count = 0;
  int rc = signet_store_list_agents(store, &ids, &count);
  assert(rc == 0);
  assert(count == 3);

  /* Verify all IDs present (order not guaranteed). */
  bool found_a = false, found_b = false, found_c = false;
  for (size_t i = 0; i < count; i++) {
    if (strcmp(ids[i], "agent-a") == 0) found_a = true;
    if (strcmp(ids[i], "agent-b") == 0) found_b = true;
    if (strcmp(ids[i], "agent-c") == 0) found_c = true;
  }
  assert(found_a && found_b && found_c);

  signet_store_free_agent_ids(ids, count);
  sodium_memzero(sk, sizeof(sk));
  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_agent_list: PASS\n");
}

static void test_agent_touch(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);

  uint8_t sk[32];
  randombytes_buf(sk, sizeof(sk));
  int64_t now = 1000000;
  signet_store_put_agent(store, "touch-agent", sk, 32, NULL, now);

  int rc = signet_store_touch_agent(store, "touch-agent", now + 100);
  assert(rc == 0);

  SignetAgentRecord rec;
  memset(&rec, 0, sizeof(rec));
  rc = signet_store_get_agent(store, "touch-agent", &rec);
  assert(rc == 0);
  assert(rec.last_used == now + 100);

  signet_agent_record_clear(&rec);
  sodium_memzero(sk, sizeof(sk));
  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_agent_touch: PASS\n");
}

/* ----------------------------- Secret CRUD ------------------------------- */

static void test_secret_put_get(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);

  /* Must have an agent first for agent_id FK. */
  uint8_t sk[32];
  randombytes_buf(sk, sizeof(sk));
  int64_t now = (int64_t)time(NULL);
  signet_store_put_agent(store, "sec-agent", sk, 32, NULL, now);

  const char *payload = "my-api-token-12345";
  int rc = signet_store_put_secret(store, "api-key-1", "sec-agent",
                                    "aabbccdd00112233aabbccdd00112233aabbccdd00112233aabbccdd00112233",
                                    SIGNET_SECRET_API_TOKEN, "My API Key",
                                    (const uint8_t *)payload, strlen(payload),
                                    NULL, now);
  assert(rc == 0);

  SignetSecretRecord rec;
  memset(&rec, 0, sizeof(rec));
  rc = signet_store_get_secret(store, "api-key-1", &rec);
  assert(rc == 0);
  assert(rec.payload_len == strlen(payload));
  assert(memcmp(rec.payload, payload, rec.payload_len) == 0);
  assert(rec.secret_type == SIGNET_SECRET_API_TOKEN);

  signet_secret_record_clear(&rec);
  sodium_memzero(sk, sizeof(sk));
  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_secret_put_get: PASS\n");
}

static void test_secret_delete(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);

  uint8_t sk[32];
  randombytes_buf(sk, sizeof(sk));
  int64_t now = (int64_t)time(NULL);
  signet_store_put_agent(store, "sec-agent2", sk, 32, NULL, now);

  const char *payload = "secret-data";
  signet_store_put_secret(store, "del-secret", "sec-agent2",
                          "aabbccdd00112233aabbccdd00112233aabbccdd00112233aabbccdd00112233",
                          SIGNET_SECRET_CREDENTIAL, "cred",
                          (const uint8_t *)payload, strlen(payload),
                          NULL, now);

  int rc = signet_store_delete_secret(store, "del-secret");
  assert(rc == 0);

  SignetSecretRecord rec;
  memset(&rec, 0, sizeof(rec));
  rc = signet_store_get_secret(store, "del-secret", &rec);
  assert(rc == 1); /* not found */

  sodium_memzero(sk, sizeof(sk));
  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_secret_delete: PASS\n");
}

static void test_secret_list(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);

  uint8_t sk[32];
  randombytes_buf(sk, sizeof(sk));
  int64_t now = (int64_t)time(NULL);
  signet_store_put_agent(store, "list-agent", sk, 32, NULL, now);

  signet_store_put_secret(store, "key-1", "list-agent",
                          "1111111100112233aabbccdd00112233aabbccdd00112233aabbccdd00112233",
                          SIGNET_SECRET_API_TOKEN, "Key One",
                          (const uint8_t *)"val1", 4, NULL, now);
  signet_store_put_secret(store, "key-2", "list-agent",
                          "2222222200112233aabbccdd00112233aabbccdd00112233aabbccdd00112233",
                          SIGNET_SECRET_SSH_KEY, "Key Two",
                          (const uint8_t *)"val2", 4, NULL, now);

  char **ids = NULL, **labels = NULL;
  size_t count = 0;
  int rc = signet_store_list_secrets(store, "list-agent", &ids, &labels, &count);
  assert(rc == 0);
  assert(count == 2);

  signet_store_free_secret_list(ids, labels, count);
  sodium_memzero(sk, sizeof(sk));
  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_secret_list: PASS\n");
}

static void test_secret_rotate(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);

  uint8_t sk[32];
  randombytes_buf(sk, sizeof(sk));
  int64_t now = (int64_t)time(NULL);
  signet_store_put_agent(store, "rot-agent", sk, 32, NULL, now);

  const char *old_payload = "old-token";
  signet_store_put_secret(store, "rot-key", "rot-agent",
                          "aabbccdd00112233aabbccdd00112233aabbccdd00112233aabbccdd00112233",
                          SIGNET_SECRET_API_TOKEN, "Rotate Me",
                          (const uint8_t *)old_payload, strlen(old_payload),
                          NULL, now);

  const char *new_payload = "new-token-rotated";
  int rc = signet_store_rotate_secret(store, "rot-key",
                                       (const uint8_t *)new_payload,
                                       strlen(new_payload), now + 60);
  assert(rc == 0);

  /* Verify we get the new payload. */
  SignetSecretRecord rec;
  memset(&rec, 0, sizeof(rec));
  rc = signet_store_get_secret(store, "rot-key", &rec);
  assert(rc == 0);
  assert(rec.payload_len == strlen(new_payload));
  assert(memcmp(rec.payload, new_payload, rec.payload_len) == 0);

  signet_secret_record_clear(&rec);
  sodium_memzero(sk, sizeof(sk));
  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_secret_rotate: PASS\n");
}

/* ----------------------------- Lease operations -------------------------- */

static void test_lease_issue_and_revoke(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);

  int64_t now = (int64_t)time(NULL);
  int rc = signet_store_issue_lease(store, "lease-001", "session",
                                    "agent-x", now, now + 3600,
                                    "{\"test\":true}");
  assert(rc == 0);

  /* Count active leases. */
  int active = signet_store_count_active_leases(store, now + 10);
  assert(active >= 1);

  /* Revoke. */
  rc = signet_store_revoke_lease(store, "lease-001", now + 20);
  assert(rc == 0);

  /* After revoke, count should be lower. */
  int after_revoke = signet_store_count_active_leases(store, now + 30);
  assert(after_revoke < active);

  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_lease_issue_and_revoke: PASS\n");
}

static void test_lease_revoke_by_agent(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);

  int64_t now = (int64_t)time(NULL);
  signet_store_issue_lease(store, "l1", "session", "agent-y", now, now + 3600, NULL);
  signet_store_issue_lease(store, "l2", "session", "agent-y", now, now + 3600, NULL);
  signet_store_issue_lease(store, "l3", "session", "agent-z", now, now + 3600, NULL);

  int revoked = signet_store_revoke_agent_leases(store, "agent-y", now + 10);
  assert(revoked == 2);

  /* agent-z lease should still be active. */
  SignetLeaseRecord *leases = NULL;
  size_t lcount = 0;
  int rc = signet_store_list_active_leases(store, "agent-z", now + 20, &leases, &lcount);
  assert(rc == 0);
  assert(lcount == 1);
  signet_lease_list_free(leases, lcount);

  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_lease_revoke_by_agent: PASS\n");
}

static void test_lease_cleanup_expired(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);

  int64_t now = (int64_t)time(NULL);
  /* Issue a lease that expires very quickly. */
  signet_store_issue_lease(store, "exp-l1", "session", "agent-e", now, now + 10, NULL);

  /* Cleanup with a cutoff after expiry should remove it. */
  int cleaned = signet_store_cleanup_expired_leases(store, now + 20);
  assert(cleaned >= 1);

  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_lease_cleanup_expired: PASS\n");
}

/* ----------------------------- Store lifecycle --------------------------- */

static void test_store_open_close(void) {
  char *db_path = make_temp_db_path();
  SignetStoreConfig cfg = { .db_path = db_path, .master_key = MASTER_KEY };

  SignetStore *store = signet_store_open(&cfg);
  assert(store != NULL);
  assert(signet_store_is_open(store));
  assert(signet_store_get_db(store) != NULL);
  assert(signet_store_get_dek(store) != NULL);

  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_store_open_close: PASS\n");
}

static void test_store_null_safety(void) {
  /* Closing NULL should not crash. */
  signet_store_close(NULL);

  /* Opening with NULL config should return NULL. */
  SignetStore *store = signet_store_open(NULL);
  assert(store == NULL);

  /* is_open on NULL should be false. */
  assert(!signet_store_is_open(NULL));

  printf("test_store_null_safety: PASS\n");
}

int main(void) {
  assert(sodium_init() >= 0);

  test_store_open_close();
  test_store_null_safety();

  test_agent_put_get();
  test_agent_not_found();
  test_agent_delete();
  test_agent_list();
  test_agent_touch();

  test_secret_put_get();
  test_secret_delete();
  test_secret_list();
  test_secret_rotate();

  test_lease_issue_and_revoke();
  test_lease_revoke_by_agent();
  test_lease_cleanup_expired();

  printf("All store tests passed!\n");
  return 0;
}
