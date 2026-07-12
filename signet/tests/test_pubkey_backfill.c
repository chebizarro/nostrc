/* SPDX-License-Identifier: MIT
 *
 * test_pubkey_backfill.c - Proves agents.pubkey backfill for legacy rows
 * (rows created before the v3.1 pubkey column, nostrc-8z0):
 *   1. a legacy row (pubkey NULL) is invisible to
 *      signet_store_pubkey_in_use() until backfilled;
 *   2. signet_key_store_backfill_pubkeys() derives and persists the correct
 *      pubkey for legacy rows, leaves populated rows untouched, and is
 *      idempotent;
 *   3. after backfill, adopt-existing of the same custody key under a new
 *      agent_id is rejected with SIGNET_ADOPT_ERR_PUBKEY_EXISTS;
 *   4. signet_store_set_agent_pubkey() never overwrites a populated pubkey
 *      and rejects malformed input.
 */

#include "signet/key_store.h"
#include "signet/store.h"
#include "signet/audit_logger.h"

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
  char tmpl[] = "/tmp/signet-test-backfill-XXXXXX.db";
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

/* Seed a LEGACY row: signet_store_put_agent (the pre-v3.1 API) persists with
 * pubkey NULL, exactly like rows written before the column existed. */
static void seed_legacy_row(const char *db_path, const char *agent_id,
                            const uint8_t sk_raw[32]) {
  SignetStoreConfig scfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetStore *st = signet_store_open(&scfg);
  assert(st != NULL);
  assert(signet_store_put_agent(st, agent_id, sk_raw, 32, "legacy-secret", 1000) == 0);

  /* Confirm the row really is a legacy row (pubkey missing). */
  SignetAgentMeta meta;
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_get_agent_meta(st, agent_id, &meta) == 0);
  assert(meta.pubkey == NULL || meta.pubkey[0] == '\0');
  signet_agent_meta_clear(&meta);

  signet_store_close(st);
}

/* Seed a legacy row (pubkey NULL) on an already-open store handle. */
static void seed_via_store(SignetStore *st, const char *agent_id,
                           const uint8_t sk_raw[32]) {
  assert(signet_store_put_agent(st, agent_id, sk_raw, 32, NULL, 1000) == 0);
}

static void test_backfill_and_collision_detection(void) {
  char *db_path = make_temp_db_path();

  /* Legacy agent written with the pre-pubkey API. */
  uint8_t legacy_sk[32]; char legacy_pk[65];
  gen_keypair(legacy_sk, legacy_pk);
  seed_legacy_row(db_path, "legacy-agent", legacy_sk);

  /* Open the key store over the same DB (as signetd does at startup). */
  SignetAuditLoggerConfig alc = { .path = NULL, .to_stdout = false, .flush_each_write = false };
  SignetAuditLogger *audit = signet_audit_logger_new(&alc);
  SignetKeyStoreConfig kcfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetKeyStore *ks = signet_key_store_new(audit, &kcfg);
  assert(ks != NULL);
  SignetStore *st = signet_key_store_get_store(ks);
  assert(st != NULL);

  /* A modern (adopted) agent whose pubkey is already populated. */
  uint8_t modern_sk[32]; char modern_pk[65];
  gen_keypair(modern_sk, modern_pk);
  char out_pk[65] = {0};
  assert(signet_key_store_adopt_agent(ks, "modern-agent", modern_sk, modern_pk, NULL,
                                      BUNKER_PK, RELAYS, 1, out_pk, NULL) == SIGNET_ADOPT_OK);

  /* 1: before backfill the legacy pubkey is invisible to collision checks. */
  bool in_use = true;
  assert(signet_store_pubkey_in_use(st, legacy_pk, NULL, &in_use) == 0);
  assert(in_use == false);

  /* 2: backfill updates exactly the legacy row. */
  size_t updated = 0, failed = 0;
  assert(signet_key_store_backfill_pubkeys(ks, &updated, &failed) == 0);
  assert(updated == 1);
  assert(failed == 0);

  SignetAgentMeta meta;
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_get_agent_meta(st, "legacy-agent", &meta) == 0);
  assert(meta.pubkey != NULL);
  assert(g_ascii_strcasecmp(meta.pubkey, legacy_pk) == 0);
  signet_agent_meta_clear(&meta);

  /* Idempotent: a second pass has nothing to do. */
  assert(signet_key_store_backfill_pubkeys(ks, &updated, &failed) == 0);
  assert(updated == 0);
  assert(failed == 0);

  /* Collision checks now see the legacy pubkey. */
  assert(signet_store_pubkey_in_use(st, legacy_pk, NULL, &in_use) == 0);
  assert(in_use == true);
  assert(signet_store_pubkey_in_use(st, legacy_pk, "legacy-agent", &in_use) == 0);
  assert(in_use == false); /* excluded from its own check */

  /* 3: adopting the SAME custody key under a new agent_id is now rejected. */
  char out_pk2[65] = {0};
  assert(signet_key_store_adopt_agent(ks, "impostor", legacy_sk, legacy_pk, NULL,
                                      BUNKER_PK, RELAYS, 1, out_pk2, NULL)
         == SIGNET_ADOPT_ERR_PUBKEY_EXISTS);

  /* 4: set_agent_pubkey never overwrites and rejects malformed input. */
  assert(signet_store_set_agent_pubkey(st, "legacy-agent", modern_pk) == 1);
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_get_agent_meta(st, "legacy-agent", &meta) == 0);
  assert(g_ascii_strcasecmp(meta.pubkey, legacy_pk) == 0); /* unchanged */
  signet_agent_meta_clear(&meta);
  assert(signet_store_set_agent_pubkey(st, "no-such-agent", legacy_pk) == 1);
  assert(signet_store_set_agent_pubkey(st, "legacy-agent", "not-a-pubkey") == -1);
  /* 64 chars but not hex must be rejected too. */
  assert(signet_store_set_agent_pubkey(st, "legacy-agent",
      "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz") == -1);

  /* Uppercase input is canonicalized to lowercase on write. */
  {
    uint8_t up_sk[32]; char up_pk[65];
    gen_keypair(up_sk, up_pk);
    seed_via_store(st, "upper-agent", up_sk);
    char upper[65];
    for (int i = 0; i < 65; i++) upper[i] = (char)g_ascii_toupper(up_pk[i]);
    assert(signet_store_set_agent_pubkey(st, "upper-agent", upper) == 0);
    SignetAgentMeta um;
    memset(&um, 0, sizeof(um));
    assert(signet_store_get_agent_meta(st, "upper-agent", &um) == 0);
    assert(um.pubkey && strcmp(um.pubkey, up_pk) == 0); /* stored lowercase */
    signet_agent_meta_clear(&um);
    bool up_in_use = false;
    assert(signet_store_pubkey_in_use(st, up_pk, NULL, &up_in_use) == 0);
    assert(up_in_use == true);
    sodium_memzero(up_sk, sizeof(up_sk));
  }

  sodium_memzero(legacy_sk, sizeof(legacy_sk));
  sodium_memzero(modern_sk, sizeof(modern_sk));
  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_backfill_and_collision_detection: PASS\n");
}

/* nostrc-ttm: DB-level uniqueness. put_agent_ex is an upsert keyed on
 * agent_id — replacing the same agent works, while binding another agent to
 * an existing pubkey or connect_secret fails with 1 and leaves the original
 * row untouched (OR REPLACE would have silently deleted it). */
static void test_db_level_uniqueness(void) {
  char *db_path = make_temp_db_path();
  SignetStoreConfig scfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetStore *st = signet_store_open(&scfg);
  assert(st != NULL);

  uint8_t sk1[32]; char pk1[65];
  uint8_t sk2[32]; char pk2[65];
  uint8_t sk3[32]; char pk3[65];
  gen_keypair(sk1, pk1);
  gen_keypair(sk2, pk2);
  gen_keypair(sk3, pk3);

  /* Insert agent-a, then replace it (same agent_id, new key): both succeed. */
  assert(signet_store_put_agent_ex(st, "agent-a", sk1, 32, "secret-a", pk1,
                                   "provisioned", 1000) == 0);
  assert(signet_store_put_agent_ex(st, "agent-a", sk2, 32, "secret-a2", pk2,
                                   "rotated", 2000) == 0);
  SignetAgentMeta meta;
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_get_agent_meta(st, "agent-a", &meta) == 0);
  assert(meta.pubkey && g_ascii_strcasecmp(meta.pubkey, pk2) == 0);
  signet_agent_meta_clear(&meta);

  /* Binding a DIFFERENT agent to agent-a's pubkey fails with 1 and must NOT
   * delete agent-a (the old INSERT OR REPLACE behavior). */
  assert(signet_store_put_agent_ex(st, "impostor", sk3, 32, NULL, pk2,
                                   "adopted", 3000) == 1);
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_get_agent_meta(st, "agent-a", &meta) == 0); /* intact */
  assert(meta.pubkey && g_ascii_strcasecmp(meta.pubkey, pk2) == 0);
  signet_agent_meta_clear(&meta);
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_get_agent_meta(st, "impostor", &meta) != 0); /* absent */

  /* Reusing another agent's connect_secret also fails with 1, intact rows. */
  assert(signet_store_put_agent_ex(st, "agent-c", sk3, 32, "dup-secret", pk3,
                                   "provisioned", 4000) == 0);
  uint8_t sk4[32]; char pk4[65];
  gen_keypair(sk4, pk4);
  assert(signet_store_put_agent_ex(st, "agent-d", sk4, 32, "dup-secret", pk4,
                                   "provisioned", 5000) == 1);
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_get_agent_meta(st, "agent-c", &meta) == 0); /* intact */
  signet_agent_meta_clear(&meta);
  memset(&meta, 0, sizeof(meta));
  assert(signet_store_get_agent_meta(st, "agent-d", &meta) != 0); /* absent */

  /* Case-insensitivity: put_agent_ex canonicalizes to lowercase, so binding
   * another agent to the UPPERCASE form of an existing pubkey still fails. */
  {
    char upper_pk2[65];
    for (int i = 0; i < 65; i++) upper_pk2[i] = (char)g_ascii_toupper(pk2[i]);
    assert(signet_store_put_agent_ex(st, "impostor-uc", sk4, 32, NULL, upper_pk2,
                                     "adopted", 5500) == 1);
    memset(&meta, 0, sizeof(meta));
    assert(signet_store_get_agent_meta(st, "impostor-uc", &meta) != 0); /* absent */
  }

  /* Malformed pubkeys are rejected outright. */
  assert(signet_store_put_agent_ex(st, "bad-pk", sk4, 32, NULL, "deadbeef",
                                   "adopted", 5600) == -1);
  assert(signet_store_put_agent_ex(st, "bad-pk", sk4, 32, NULL,
      "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
      "adopted", 5700) == -1);

  /* Legacy rows with NULL pubkey do not trip the partial unique index. */
  uint8_t sk5[32]; char pk5[65];
  uint8_t sk6[32]; char pk6[65];
  gen_keypair(sk5, pk5);
  gen_keypair(sk6, pk6);
  assert(signet_store_put_agent(st, "legacy-1", sk5, 32, NULL, 6000) == 0);
  assert(signet_store_put_agent(st, "legacy-2", sk6, 32, NULL, 7000) == 0);

  sodium_memzero(sk1, sizeof(sk1));
  sodium_memzero(sk2, sizeof(sk2));
  sodium_memzero(sk3, sizeof(sk3));
  sodium_memzero(sk4, sizeof(sk4));
  sodium_memzero(sk5, sizeof(sk5));
  sodium_memzero(sk6, sizeof(sk6));
  signet_store_close(st);
  unlink(db_path);
  g_free(db_path);
  printf("test_db_level_uniqueness: PASS\n");
}

/* Duplicate legacy custody keys: the same secret stored under two agent_ids
 * (possible pre-index). Backfill must populate the first, count the second as
 * a per-row failure (pubkey already bound), and NOT abort the pass. */
static void test_backfill_duplicate_legacy_keys(void) {
  char *db_path = make_temp_db_path();

  uint8_t sk[32]; char pk[65];
  gen_keypair(sk, pk);
  seed_legacy_row(db_path, "twin-a", sk);
  {
    SignetStoreConfig scfg = { .db_path = db_path, .master_key = MASTER_KEY };
    SignetStore *st = signet_store_open(&scfg);
    assert(st != NULL);
    assert(signet_store_put_agent(st, "twin-b", sk, 32, NULL, 1100) == 0);
    signet_store_close(st);
  }

  SignetAuditLoggerConfig alc = { .path = NULL, .to_stdout = false, .flush_each_write = false };
  SignetAuditLogger *audit = signet_audit_logger_new(&alc);
  SignetKeyStoreConfig kcfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetKeyStore *ks = signet_key_store_new(audit, &kcfg);
  assert(ks != NULL);

  size_t updated = 0, failed = 0;
  assert(signet_key_store_backfill_pubkeys(ks, &updated, &failed) == 0);
  assert(updated == 1);
  assert(failed == 1);

  /* Exactly one of the twins carries the pubkey. */
  SignetStore *st = signet_key_store_get_store(ks);
  SignetAgentMeta ma, mb;
  memset(&ma, 0, sizeof(ma));
  memset(&mb, 0, sizeof(mb));
  assert(signet_store_get_agent_meta(st, "twin-a", &ma) == 0);
  assert(signet_store_get_agent_meta(st, "twin-b", &mb) == 0);
  bool a_set = (ma.pubkey && ma.pubkey[0]);
  bool b_set = (mb.pubkey && mb.pubkey[0]);
  assert(a_set != b_set);
  signet_agent_meta_clear(&ma);
  signet_agent_meta_clear(&mb);

  sodium_memzero(sk, sizeof(sk));
  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_backfill_duplicate_legacy_keys: PASS\n");
}

static void test_backfill_empty_store(void) {
  char *db_path = make_temp_db_path();
  SignetAuditLoggerConfig alc = { .path = NULL, .to_stdout = false, .flush_each_write = false };
  SignetAuditLogger *audit = signet_audit_logger_new(&alc);
  SignetKeyStoreConfig kcfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetKeyStore *ks = signet_key_store_new(audit, &kcfg);
  assert(ks != NULL);

  size_t updated = 99, failed = 99;
  assert(signet_key_store_backfill_pubkeys(ks, &updated, &failed) == 0);
  assert(updated == 0);
  assert(failed == 0);

  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_backfill_empty_store: PASS\n");
}

int main(void) {
  if (sodium_init() < 0) {
    fprintf(stderr, "sodium_init failed\n");
    return 1;
  }

  test_backfill_and_collision_detection();
  test_db_level_uniqueness();
  test_backfill_duplicate_legacy_keys();
  test_backfill_empty_store();

  printf("All pubkey backfill tests passed.\n");
  return 0;
}
