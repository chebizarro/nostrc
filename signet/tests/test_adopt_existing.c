/* SPDX-License-Identifier: MIT
 *
 * test_adopt_existing.c - Proves BYO-key adoption (agent/adopt-existing) at the
 * key-store layer:
 *   1. adopt succeeds with a valid secret + matching expected pubkey, and the
 *      returned pubkey/bunker_uri are correct;
 *   2. adopt fails on a malformed (invalid) secret;
 *   3. adopt fails when the derived pubkey does not match expected_pubkey;
 *   4. adopt fails when the agent_id already exists;
 *   5. adopt fails when the pubkey is already bound to another agent;
 *   6. an adopted agent produces a valid Schnorr signature whose derived pubkey
 *      matches the adopted (canonical) pubkey;
 *   7. adopt records the secret encrypted at rest (raw secret never on disk).
 */

#include "signet/key_store.h"
#include "signet/store.h"
#include "signet/audit_logger.h"

#include <nostr-event.h>
#include <nostr-keys.h>

#include <assert.h>
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
  char tmpl[] = "/tmp/signet-test-adopt-XXXXXX.db";
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

static void bytes_to_hex(const uint8_t *in, size_t n, char *out) {
  static const char *h = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[i * 2] = h[in[i] >> 4]; out[i * 2 + 1] = h[in[i] & 0xf]; }
  out[n * 2] = '\0';
}

static bool buf_contains(const uint8_t *hay, size_t hlen, const uint8_t *needle, size_t nlen) {
  if (nlen == 0 || hlen < nlen) return false;
  for (size_t i = 0; i + nlen <= hlen; i++)
    if (memcmp(hay + i, needle, nlen) == 0) return true;
  return false;
}

static uint8_t *read_file_bytes(const char *path, size_t *out_len) {
  *out_len = 0;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) { fclose(f); return NULL; }
  uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
  size_t rd = buf ? fread(buf, 1, (size_t)sz, f) : 0;
  fclose(f);
  if (!buf) return NULL;
  *out_len = rd;
  return buf;
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

/* 1 + 7: adopt succeeds with matching expected pubkey; returns pk + bunker_uri. */
static void test_adopt_success(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_ks(&db_path);

  uint8_t sk_raw[32]; char pk_hex[65];
  gen_keypair(sk_raw, pk_hex);

  char out_pk[65] = {0};
  char *uri = NULL;
  SignetAdoptResult r = signet_key_store_adopt_agent(
      ks, "adopted-1", sk_raw, pk_hex, NULL, BUNKER_PK, RELAYS, 1, out_pk, &uri);
  assert(r == SIGNET_ADOPT_OK);
  assert(strcmp(out_pk, pk_hex) == 0);
  assert(uri != NULL);
  assert(strncmp(uri, "bunker://", 9) == 0);
  assert(strstr(uri, BUNKER_PK) != NULL);
  assert(strstr(uri, "secret=") != NULL);

  g_free(uri);
  sodium_memzero(sk_raw, sizeof(sk_raw));
  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_adopt_success: PASS\n");
}

/* 2: malformed / invalid secret is rejected. */
static void test_adopt_invalid_secret(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_ks(&db_path);

  uint8_t sk_zero[32];
  memset(sk_zero, 0, sizeof(sk_zero)); /* 0 is not a valid secp256k1 scalar */
  char out_pk[65] = {0};
  char *uri = NULL;
  SignetAdoptResult r = signet_key_store_adopt_agent(
      ks, "bad", sk_zero, NULL, NULL, BUNKER_PK, RELAYS, 1, out_pk, &uri);
  assert(r == SIGNET_ADOPT_ERR_INVALID_SECRET);
  assert(uri == NULL);

  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_adopt_invalid_secret: PASS\n");
}

/* 3: derived pubkey must match expected_pubkey exactly. */
static void test_adopt_pubkey_mismatch(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_ks(&db_path);

  uint8_t sk_raw[32]; char pk_hex[65];
  gen_keypair(sk_raw, pk_hex);
  /* A different, non-matching expected pubkey. */
  char wrong_pk[65];
  memcpy(wrong_pk, pk_hex, 65);
  wrong_pk[0] = (wrong_pk[0] == 'a') ? 'b' : 'a';

  char out_pk[65] = {0};
  char *uri = NULL;
  SignetAdoptResult r = signet_key_store_adopt_agent(
      ks, "mismatch", sk_raw, wrong_pk, NULL, BUNKER_PK, RELAYS, 1, out_pk, &uri);
  assert(r == SIGNET_ADOPT_ERR_PUBKEY_MISMATCH);
  assert(uri == NULL);

  sodium_memzero(sk_raw, sizeof(sk_raw));
  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_adopt_pubkey_mismatch: PASS\n");
}

/* 4: adopting an already-existing agent_id fails. */
static void test_adopt_agent_exists(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_ks(&db_path);

  uint8_t sk1[32]; char pk1[65];
  gen_keypair(sk1, pk1);
  char out_pk[65] = {0};
  char *uri = NULL;
  assert(signet_key_store_adopt_agent(ks, "dup", sk1, pk1, NULL, BUNKER_PK,
                                      RELAYS, 1, out_pk, &uri) == SIGNET_ADOPT_OK);
  g_free(uri); uri = NULL;

  /* A different key, same agent_id -> agent_exists. */
  uint8_t sk2[32]; char pk2[65];
  gen_keypair(sk2, pk2);
  SignetAdoptResult r = signet_key_store_adopt_agent(
      ks, "dup", sk2, pk2, NULL, BUNKER_PK, RELAYS, 1, out_pk, &uri);
  assert(r == SIGNET_ADOPT_ERR_AGENT_EXISTS);
  assert(uri == NULL);

  sodium_memzero(sk1, sizeof(sk1));
  sodium_memzero(sk2, sizeof(sk2));
  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_adopt_agent_exists: PASS\n");
}

/* 5: adopting a pubkey already bound to another agent fails. */
static void test_adopt_pubkey_exists(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_ks(&db_path);

  uint8_t sk[32]; char pk[65];
  gen_keypair(sk, pk);
  char out_pk[65] = {0};
  char *uri = NULL;
  assert(signet_key_store_adopt_agent(ks, "agent-a", sk, pk, NULL, BUNKER_PK,
                                      RELAYS, 1, out_pk, &uri) == SIGNET_ADOPT_OK);
  g_free(uri); uri = NULL;

  /* Same key, different agent_id -> pubkey_exists. */
  SignetAdoptResult r = signet_key_store_adopt_agent(
      ks, "agent-b", sk, pk, NULL, BUNKER_PK, RELAYS, 1, out_pk, &uri);
  assert(r == SIGNET_ADOPT_ERR_PUBKEY_EXISTS);
  assert(uri == NULL);

  sodium_memzero(sk, sizeof(sk));
  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_adopt_pubkey_exists: PASS\n");
}

/* 6: an adopted agent signs a valid event committing to the canonical pubkey. */
static void test_adopted_agent_signs(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_ks(&db_path);

  uint8_t sk_raw[32]; char pk_hex[65];
  gen_keypair(sk_raw, pk_hex);
  char out_pk[65] = {0};
  char *uri = NULL;
  assert(signet_key_store_adopt_agent(ks, "signer", sk_raw, pk_hex, NULL, BUNKER_PK,
                                      RELAYS, 1, out_pk, &uri) == SIGNET_ADOPT_OK);
  g_free(uri);

  /* get_public_key: the stored pubkey equals the canonical one. */
  char got_pk[65] = {0};
  assert(signet_key_store_get_agent_pubkey(ks, "signer", got_pk, sizeof(got_pk)));
  assert(strcmp(got_pk, pk_hex) == 0);

  /* sign_event: load the key and produce a valid Schnorr signature. */
  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  assert(signet_key_store_load_agent_key(ks, "signer", &lk));
  assert(lk.secret_key && lk.secret_key_len == 32);
  /* The loaded key must equal the secret we supplied. */
  assert(memcmp(lk.secret_key, sk_raw, 32) == 0);

  char sk_hex[65];
  bytes_to_hex(lk.secret_key, 32, sk_hex);
  NostrEvent *evt = nostr_event_new();
  assert(evt);
  nostr_event_set_kind(evt, 1);
  nostr_event_set_created_at(evt, 1700000000);
  nostr_event_set_content(evt, "adopted signer test");
  assert(nostr_event_sign(evt, sk_hex) == 0);
  assert(nostr_event_check_signature(evt));
  const char *evt_pub = nostr_event_get_pubkey(evt);
  assert(evt_pub && strcmp(evt_pub, pk_hex) == 0);
  nostr_event_free(evt);

  sodium_memzero(sk_hex, sizeof(sk_hex));
  sodium_memzero(sk_raw, sizeof(sk_raw));
  signet_loaded_key_clear(&lk);
  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_adopted_agent_signs: PASS\n");
}

/* 7 (at rest): the raw adopted secret is never written to disk in plaintext. */
static void test_adopt_secret_not_on_disk(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_ks(&db_path);

  uint8_t sk_raw[32]; char pk_hex[65];
  gen_keypair(sk_raw, pk_hex);
  char sk_hex[65];
  bytes_to_hex(sk_raw, 32, sk_hex);

  char out_pk[65] = {0};
  char *uri = NULL;
  assert(signet_key_store_adopt_agent(ks, "at-rest", sk_raw, pk_hex, NULL, BUNKER_PK,
                                      RELAYS, 1, out_pk, &uri) == SIGNET_ADOPT_OK);
  g_free(uri);
  signet_key_store_free(ks); /* checkpoints/close */

  bool found = false;
  size_t n = 0;
  uint8_t *b = read_file_bytes(db_path, &n);
  if (b) { found = found || buf_contains(b, n, sk_raw, 32); free(b); }
  b = read_file_bytes(db_path, &n);
  if (b) { found = found || buf_contains(b, n, (const uint8_t *)sk_hex, 64); free(b); }
  char *wal = g_strdup_printf("%s-wal", db_path);
  b = read_file_bytes(wal, &n);
  if (b) { found = found || buf_contains(b, n, sk_raw, 32); free(b); }
  assert(!found);

  unlink(db_path);
  unlink(wal);
  g_free(wal);
  char *shm = g_strdup_printf("%s-shm", db_path);
  unlink(shm);
  g_free(shm);
  sodium_memzero(sk_hex, sizeof(sk_hex));
  sodium_memzero(sk_raw, sizeof(sk_raw));
  g_free(db_path);
  printf("test_adopt_secret_not_on_disk: PASS\n");
}

int main(void) {
  assert(sodium_init() >= 0);
  test_adopt_success();
  test_adopt_invalid_secret();
  test_adopt_pubkey_mismatch();
  test_adopt_agent_exists();
  test_adopt_pubkey_exists();
  test_adopted_agent_signs();
  test_adopt_secret_not_on_disk();
  printf("All adopt-existing tests passed!\n");
  return 0;
}
