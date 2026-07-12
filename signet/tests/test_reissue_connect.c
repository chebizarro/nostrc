/* SPDX-License-Identifier: MIT
 *
 * test_reissue_connect.c - Proves fresh connect_secret reissue
 * (agent/reissue-connect) at the store/key-store/protocol layers:
 *   1. reissue succeeds for an existing agent: returns a fresh 64-hex secret
 *      (different from the old one), the agent identity pubkey, and a
 *      bunker:// URI embedding the fresh secret;
 *   2. after reissue the OLD secret no longer resolves (single-use model is
 *      preserved), while the FRESH secret resolves and consumes exactly once;
 *   3. reissue works after the original secret was already consumed
 *      (restart-recovery case) and the new secret validates;
 *   4. reissue fails with 1 (not found) for an unknown agent;
 *   5. protocol wiring: kind<->op mapping, agent/reissue-connect requires
 *      agent_id, and unauthorized senders are rejected by the authorization
 *      check used by the management handler.
 */

#include "signet/key_store.h"
#include "signet/mgmt_protocol.h"
#include "signet/store.h"
#include "signet/audit_logger.h"

#include <nostr-keys.h>

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <sodium.h>

#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static const char *const RELAYS[] = { "wss://relay.example" };
static const char *const BUNKER_PK =
    "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef";

static char *make_temp_db_path(void) {
  char tmpl[] = "/tmp/signet-test-reissue-XXXXXX.db";
  int fd = mkstemps(tmpl, 3);
  assert(fd >= 0);
  close(fd);
  unlink(tmpl);
  return g_strdup(tmpl);
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned int b;
    if (sscanf(hex + i * 2, "%2x", &b) != 1) return -1;
    out[i] = (uint8_t)b;
  }
  return 0;
}

/* Generate a fresh keypair: raw secret bytes + lowercase hex pubkey. */
static void gen_keypair(uint8_t sk_raw[32], char pk_hex[65]) {
  char *sk_hex = nostr_key_generate_private();
  assert(sk_hex && strlen(sk_hex) == 64);
  char *pk = nostr_key_get_public(sk_hex);
  assert(pk && strlen(pk) == 64);
  assert(hex_to_bytes(sk_hex, sk_raw, 32) == 0);
  memcpy(pk_hex, pk, 65);
  free(pk);
  sodium_memzero(sk_hex, strlen(sk_hex));
  free(sk_hex);
}

static SignetKeyStore *open_ks(char **out_path) {
  char *db_path = make_temp_db_path();
  SignetAuditLoggerConfig alc = { .path = NULL, .to_stdout = false, .flush_each_write = false };
  SignetAuditLogger *audit = signet_audit_logger_new(&alc);
  SignetKeyStoreConfig cfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetKeyStore *ks = signet_key_store_new(audit, &cfg);
  assert(ks != NULL);
  *out_path = db_path;
  return ks;
}

static bool is_hex64(const char *s) {
  if (!s || strlen(s) != 64) return false;
  for (int i = 0; i < 64; i++)
    if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}

/* 1 + 2: reissue returns a fresh secret, invalidates the old one, and the
 * fresh one resolves+consumes exactly once. */
static void test_reissue_success(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_ks(&db_path);

  uint8_t sk_raw[32]; char pk_hex[65];
  gen_keypair(sk_raw, pk_hex);

  const char *old_secret = "fixed-old-connect-secret";
  char out_pk[65] = {0};
  assert(signet_key_store_adopt_agent(ks, "stew", sk_raw, pk_hex, old_secret,
                                      BUNKER_PK, RELAYS, 1, out_pk, NULL) == SIGNET_ADOPT_OK);

  char re_pk[65] = {0};
  char *fresh = NULL;
  char *uri = NULL;
  int rc = signet_key_store_reissue_connect_secret(ks, "stew", BUNKER_PK, RELAYS, 1,
                                                   re_pk, &fresh, &uri);
  assert(rc == 0);
  assert(fresh != NULL && is_hex64(fresh));
  assert(strcmp(fresh, old_secret) != 0);
  assert(strcmp(re_pk, pk_hex) == 0);
  assert(uri != NULL);
  assert(strncmp(uri, "bunker://", 9) == 0);
  assert(strstr(uri, BUNKER_PK) != NULL);
  assert(strstr(uri, fresh) != NULL);

  /* The OLD secret must no longer resolve. */
  char *agent = NULL;
  assert(signet_key_store_consume_connect_secret(ks, old_secret, 1000, &agent) == 1);
  assert(agent == NULL);

  /* The FRESH secret resolves to the agent and consumes exactly once. */
  assert(signet_key_store_consume_connect_secret(ks, fresh, 1000, &agent) == 0);
  assert(agent && strcmp(agent, "stew") == 0);
  g_free(agent);
  agent = NULL;
  assert(signet_key_store_consume_connect_secret(ks, fresh, 1000, &agent) == 1);

  g_free(uri);
  sodium_memzero(fresh, strlen(fresh));
  g_free(fresh);
  sodium_memzero(sk_raw, sizeof(sk_raw));
  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_reissue_success: PASS\n");
}

/* 3: reissue after the original secret was consumed (restart recovery). */
static void test_reissue_after_consumption(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_ks(&db_path);

  uint8_t sk_raw[32]; char pk_hex[65];
  gen_keypair(sk_raw, pk_hex);

  const char *secret = "consumed-once-secret";
  char out_pk[65] = {0};
  assert(signet_key_store_adopt_agent(ks, "stew", sk_raw, pk_hex, secret,
                                      BUNKER_PK, RELAYS, 1, out_pk, NULL) == SIGNET_ADOPT_OK);

  /* First connect consumes the secret. A replayed consumed secret must not
   * resolve any agent via the consume-by-value path used by NIP-46 connect. */
  assert(signet_key_store_validate_connect_secret(ks, "stew", secret) == 0);
  {
    char *agent = NULL;
    assert(signet_key_store_consume_connect_secret(ks, secret, 1000, &agent) == 1);
  }

  /* Reissue mints a fresh secret that validates again. */
  char re_pk[65] = {0};
  char *fresh = NULL;
  assert(signet_key_store_reissue_connect_secret(ks, "stew", NULL, NULL, 0,
                                                 re_pk, &fresh, NULL) == 0);
  assert(fresh && is_hex64(fresh));
  assert(signet_key_store_validate_connect_secret(ks, "stew", fresh) == 0);

  /* Reissue twice: only the latest secret is valid. */
  char *fresh2 = NULL;
  assert(signet_key_store_reissue_connect_secret(ks, "stew", NULL, NULL, 0,
                                                 re_pk, &fresh2, NULL) == 0);
  char *fresh3 = NULL;
  assert(signet_key_store_reissue_connect_secret(ks, "stew", NULL, NULL, 0,
                                                 re_pk, &fresh3, NULL) == 0);
  assert(strcmp(fresh2, fresh3) != 0);
  {
    char *agent = NULL;
    assert(signet_key_store_consume_connect_secret(ks, fresh2, 1000, &agent) == 1);
    assert(signet_key_store_consume_connect_secret(ks, fresh3, 1000, &agent) == 0);
    assert(agent && strcmp(agent, "stew") == 0);
    g_free(agent);
  }

  sodium_memzero(fresh, strlen(fresh)); g_free(fresh);
  sodium_memzero(fresh2, strlen(fresh2)); g_free(fresh2);
  sodium_memzero(fresh3, strlen(fresh3)); g_free(fresh3);
  sodium_memzero(sk_raw, sizeof(sk_raw));
  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_reissue_after_consumption: PASS\n");
}

/* 4: unknown agent -> 1 (not found). */
static void test_reissue_unknown_agent(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_ks(&db_path);

  char re_pk[65] = {0};
  char *fresh = NULL;
  int rc = signet_key_store_reissue_connect_secret(ks, "nope", BUNKER_PK, RELAYS, 1,
                                                   re_pk, &fresh, NULL);
  assert(rc == 1);
  assert(fresh == NULL);

  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_reissue_unknown_agent: PASS\n");
}

/* 5: protocol wiring — kind/op mapping, required params, authorization. */
static void test_protocol_wiring(void) {
  assert(signet_mgmt_op_from_kind(SIGNET_KIND_REISSUE_CONNECT) == SIGNET_MGMT_OP_REISSUE_CONNECT);
  assert(strcmp(signet_mgmt_op_to_string(SIGNET_MGMT_OP_REISSUE_CONNECT), "reissue_connect") == 0);

  /* agent_id is required. */
  SignetMgmtRequest req;
  char *err = NULL;
  assert(signet_mgmt_request_parse(SIGNET_KIND_REISSUE_CONNECT,
                                   "{\"request_id\":\"r1\"}", &req, &err) != 0);
  assert(err != NULL);
  g_free(err);
  err = NULL;

  assert(signet_mgmt_request_parse(SIGNET_KIND_REISSUE_CONNECT,
                                   "{\"agent_id\":\"stew\",\"request_id\":\"r2\"}",
                                   &req, &err) == 0);
  assert(req.op == SIGNET_MGMT_OP_REISSUE_CONNECT);
  assert(req.agent_id && strcmp(req.agent_id, "stew") == 0);
  assert(req.request_id && strcmp(req.request_id, "r2") == 0);
  signet_mgmt_request_clear(&req);

  /* Only configured provisioner pubkeys are authorized. */
  const char *const provs[] = {
    "1111111111111111111111111111111111111111111111111111111111111111",
  };
  assert(signet_mgmt_is_authorized(provs[0], provs, 1) == true);
  assert(signet_mgmt_is_authorized(
             "2222222222222222222222222222222222222222222222222222222222222222",
             provs, 1) == false);

  printf("test_protocol_wiring: PASS\n");
}

int main(void) {
  if (sodium_init() < 0) {
    fprintf(stderr, "sodium_init failed\n");
    return 1;
  }

  test_reissue_success();
  test_reissue_after_consumption();
  test_reissue_unknown_agent();
  test_protocol_wiring();

  printf("All reissue-connect tests passed.\n");
  return 0;
}
