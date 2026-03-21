/* SPDX-License-Identifier: MIT
 *
 * test_bootstrap_contract.c - Regression tests for authoritative bootstrap/auth state.
 */

#include "signet/store.h"
#include "signet/store_tokens.h"
#include "signet/store_leases.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>
#include <sodium.h>

static void sha256_hex(const char *input, char out_hex[crypto_hash_sha256_BYTES * 2 + 1]) {
  unsigned char hash[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(hash, (const unsigned char *)input, strlen(input));
  for (size_t i = 0; i < crypto_hash_sha256_BYTES; i++) {
    sprintf(out_hex + i * 2, "%02x", hash[i]);
  }
  out_hex[crypto_hash_sha256_BYTES * 2] = '\0';
  sodium_memzero(hash, sizeof(hash));
}

static char *make_temp_db_path(void) {
  char tmpl[] = "/tmp/signet-bootstrap-contract-XXXXXX.db";
  int fd = mkstemps(tmpl, 3);
  assert(fd >= 0);
  close(fd);
  unlink(tmpl);
  return g_strdup(tmpl);
}

static SignetStore *open_test_store(char **out_path) {
  char *db_path = make_temp_db_path();
  SignetStoreConfig cfg = {
    .db_path = db_path,
    .master_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  };
  SignetStore *store = signet_store_open(&cfg);
  assert(store != NULL);
  if (out_path) *out_path = db_path; else g_free(db_path);
  return store;
}

static void test_bootstrap_token_consumed_on_session_establishment(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);
  assert(store != NULL);

  int64_t now = (int64_t)time(NULL);
  const char *raw_token = "bootstrap-token-1";
  const char *connect_secret = "connect-secret-a";
  char token_hash[crypto_hash_sha256_BYTES * 2 + 1];
  int rc = 0;
  sha256_hex(raw_token, token_hash);

  unsigned char secret_key[32];
  memset(secret_key, 0x11, sizeof(secret_key));
  rc = signet_store_put_agent(store, "agent-a", secret_key, sizeof(secret_key), connect_secret, now);
  assert(rc == 0);
  sodium_memzero(secret_key, sizeof(secret_key));

  rc = signet_store_put_bootstrap_token(store, token_hash,
                                        "agent-a", "boot-pubkey-a",
                                        now, now + 600);
  assert(rc == 0);

  /* Handoff verification should leave the token unused. */
  SignetTokenResult vr = signet_store_verify_bootstrap_token(store, token_hash,
                                                             "agent-a", "boot-pubkey-a",
                                                             now);
  assert(vr == SIGNET_TOKEN_OK);

  rc = signet_store_bind_bootstrap_token_handoff(store, token_hash, connect_secret);
  assert(rc == 0);

  /* The real session establishment step consumes the exact bound token and
   * connect secret atomically. */
  char *resolved_agent_id = NULL;
  rc = signet_store_consume_connect_secret_value(store, connect_secret, now + 1, &resolved_agent_id);
  assert(rc == 0);
  assert(resolved_agent_id != NULL && strcmp(resolved_agent_id, "agent-a") == 0);
  g_free(resolved_agent_id);

  vr = signet_store_verify_bootstrap_token(store, token_hash,
                                           "agent-a", "boot-pubkey-a",
                                           now + 2);
  assert(vr == SIGNET_TOKEN_ALREADY_USED);

  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_bootstrap_token_consumed_on_session_establishment: PASS\n");
}

static void test_session_token_lookup_uses_persisted_lease_state(void) {
  char *db_path = NULL;
  SignetStore *store = open_test_store(&db_path);
  assert(store != NULL);

  int64_t now = (int64_t)time(NULL);
  const char *session_token = "session-token-abc";
  char token_hash[crypto_hash_sha256_BYTES * 2 + 1];
  sha256_hex(session_token, token_hash);

  const char *lease_id = "lease-123";
  char *meta = g_strdup_printf(
      "{\"session_token_hash\":\"%s\",\"transport\":\"http\",\"auth_method\":\"keypair\",\"pubkey\":\"pubkey-1\"}",
      token_hash);
  int rc = signet_store_issue_lease(store, lease_id, "session", "agent-a",
                                    now, now + 3600, meta);
  g_free(meta);
  assert(rc == 0);

  SignetLeaseRecord rec;
  memset(&rec, 0, sizeof(rec));
  rc = signet_store_get_active_session_by_token(store, session_token, now + 5, &rec);
  assert(rc == 0);
  assert(rec.agent_id != NULL && strcmp(rec.agent_id, "agent-a") == 0);
  assert(rec.lease_id != NULL && strcmp(rec.lease_id, lease_id) == 0);

  signet_lease_record_clear(&rec);
  signet_store_close(store);
  unlink(db_path);
  g_free(db_path);
  printf("test_session_token_lookup_uses_persisted_lease_state: PASS\n");
}

int main(void) {
  assert(sodium_init() >= 0);
  test_bootstrap_token_consumed_on_session_establishment();
  test_session_token_lookup_uses_persisted_lease_state();
  printf("All bootstrap/auth contract tests passed!\n");
  return 0;
}
