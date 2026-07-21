/* SPDX-License-Identifier: MIT
 *
 * test_mgmt_replay.c - Proves replay protection on the 25910 management plane:
 *   1. without an attached replay cache the handler executes duplicate event
 *      ids twice (documents that protection is opt-in wiring by the daemon);
 *   2. with a replay cache attached, a duplicate event id is rejected and the
 *      command does not execute a second time;
 *   3. a NON-IDEMPOTENT command (agent/reissue-connect) delivered twice with
 *      the same event id mutates state exactly once — the connect_secret
 *      minted by the first delivery survives the replay;
 *   4. with a replay cache attached, an event with a missing event id fails
 *      closed and does not execute.
 */

#include "signet/key_store.h"
#include "signet/mgmt_protocol.h"
#include "signet/replay_cache.h"
#include "signet/store.h"
#include "signet/audit_logger.h"

#include <nostr-keys.h>
#include <nostr/nip44/nip44.h>

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

static char *make_temp_db_path(void) {
  char tmpl[] = "/tmp/signet-test-mgmt-replay-XXXXXX.db";
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

/* Generate a fresh keypair: 64-hex secret + 64-hex pubkey. */
static void gen_keypair_hex(char sk_hex[65], char pk_hex[65]) {
  char *sk = nostr_key_generate_private();
  assert(sk && strlen(sk) == 64);
  char *pk = nostr_key_get_public(sk);
  assert(pk && strlen(pk) == 64);
  memcpy(sk_hex, sk, 65);
  memcpy(pk_hex, pk, 65);
  sodium_memzero(sk, strlen(sk));
  free(sk);
  free(pk);
}

typedef struct {
  SignetKeyStore *ks;
  SignetMgmtHandler *mgmt;
  SignetReplayCache *replay;
  char *db_path;
  char bunker_sk_hex[65];
  char bunker_pk_hex[65];
  char prov_sk_hex[65];
  char prov_pk_hex[65];
} Fixture;

static void fixture_setup(Fixture *f, bool with_replay_cache) {
  memset(f, 0, sizeof(*f));

  gen_keypair_hex(f->bunker_sk_hex, f->bunker_pk_hex);
  gen_keypair_hex(f->prov_sk_hex, f->prov_pk_hex);

  f->db_path = make_temp_db_path();
  SignetAuditLoggerConfig alc = { .path = NULL, .to_stdout = false, .flush_each_write = false };
  SignetAuditLogger *audit = signet_audit_logger_new(&alc);
  SignetKeyStoreConfig kcfg = { .db_path = f->db_path, .master_key = MASTER_KEY };
  f->ks = signet_key_store_new(audit, &kcfg);
  assert(f->ks != NULL);

  const char *const provs[] = { f->prov_pk_hex };
  SignetMgmtHandlerConfig mcfg = {
    .provisioner_pubkeys = provs,
    .n_provisioner_pubkeys = 1,
    .bunker_secret_key_hex = f->bunker_sk_hex,
    .bunker_pubkey_hex = f->bunker_pk_hex,
    .relay_urls = RELAYS,
    .n_relay_urls = 1,
  };
  /* No relay pool: ack publishing becomes a no-op, which is fine — these
   * tests assert on handler return codes and persisted store state. */
  f->mgmt = signet_mgmt_handler_new(f->ks, NULL, NULL, NULL, &mcfg);
  assert(f->mgmt != NULL);

  if (with_replay_cache) {
    SignetReplayCacheConfig rcfg = { .max_entries = 128, .ttl_seconds = 300, .skew_seconds = 300 };
    f->replay = signet_replay_cache_new(&rcfg);
    assert(f->replay != NULL);
    signet_mgmt_handler_set_replay_cache(f->mgmt, f->replay);
  }
}

static void fixture_teardown(Fixture *f) {
  signet_mgmt_handler_free(f->mgmt);
  signet_key_store_free(f->ks);
  signet_replay_cache_free(f->replay);
  unlink(f->db_path);
  g_free(f->db_path);
  sodium_memzero(f->bunker_sk_hex, sizeof(f->bunker_sk_hex));
  sodium_memzero(f->prov_sk_hex, sizeof(f->prov_sk_hex));
}

/* NIP-44 v2 encrypt `plaintext` from the provisioner to the bunker, as the
 * management transport does after gift-wrap unwrapping. Caller frees. */
static char *encrypt_to_bunker(Fixture *f, const char *plaintext) {
  uint8_t sk[32], pk[32];
  assert(hex_to_bytes(f->prov_sk_hex, sk, 32) == 0);
  assert(hex_to_bytes(f->bunker_pk_hex, pk, 32) == 0);
  char *cipher = NULL;
  int rc = nostr_nip44_encrypt_v2(sk, pk, (const uint8_t *)plaintext,
                                  strlen(plaintext), &cipher);
  sodium_memzero(sk, sizeof(sk));
  assert(rc == 0 && cipher != NULL);
  return cipher;
}

/* Adopt a known agent so non-idempotent commands have a target. */
static void adopt_agent(Fixture *f, const char *agent_id) {
  char sk_hex[65], pk_hex[65];
  gen_keypair_hex(sk_hex, pk_hex);
  uint8_t sk_raw[32];
  assert(hex_to_bytes(sk_hex, sk_raw, 32) == 0);
  sodium_memzero(sk_hex, sizeof(sk_hex));
  char out_pk[65] = {0};
  assert(signet_key_store_adopt_agent(f->ks, agent_id, sk_raw, pk_hex,
                                      "initial-secret", f->bunker_pk_hex,
                                      RELAYS, 1, out_pk, NULL) == SIGNET_ADOPT_OK);
  sodium_memzero(sk_raw, sizeof(sk_raw));
}

/* Read the currently persisted connect_secret for an agent. Caller frees. */
static char *read_connect_secret(Fixture *f, const char *agent_id) {
  SignetStore *st = signet_key_store_get_store(f->ks);
  assert(st != NULL);
  SignetAgentRecord rec;
  memset(&rec, 0, sizeof(rec));
  assert(signet_store_get_agent(st, agent_id, &rec) == 0);
  char *secret = rec.connect_secret ? g_strdup(rec.connect_secret) : NULL;
  signet_agent_record_clear(&rec);
  return secret;
}

/* 1: without a replay cache, duplicates execute twice (opt-in wiring). */
static void test_no_cache_duplicates_execute(void) {
  Fixture f;
  fixture_setup(&f, false);

  int64_t now = 1752380000;
  assert(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, "",
                                          SIGNET_MGMT_OP_GET_STATUS, "evt-dup", now) == 0);
  assert(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, "",
                                          SIGNET_MGMT_OP_GET_STATUS, "evt-dup", now) == 0);

  fixture_teardown(&f);
  printf("test_no_cache_duplicates_execute: PASS\n");
}

/* 2: with a replay cache, a duplicate event id is rejected; fresh ids run. */
static void test_cache_rejects_duplicate(void) {
  Fixture f;
  fixture_setup(&f, true);

  int64_t now = 1752380000;
  assert(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, "",
                                          SIGNET_MGMT_OP_GET_STATUS, "evt-1", now) == 0);
  assert(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, "",
                                          SIGNET_MGMT_OP_GET_STATUS, "evt-1", now) == -1);
  assert(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, "",
                                          SIGNET_MGMT_OP_GET_STATUS, "evt-2", now) == 0);

  fixture_teardown(&f);
  printf("test_cache_rejects_duplicate: PASS\n");
}

/* 3: a replayed agent/reissue-connect mutates state exactly once. */
static void test_replayed_reissue_mints_once(void) {
  Fixture f;
  fixture_setup(&f, true);
  adopt_agent(&f, "stew");

  char *cipher = encrypt_to_bunker(&f, "{\"agent_id\":\"stew\",\"request_id\":\"r1\"}");
  int64_t now = 1752380000;

  /* First delivery executes and replaces the connect_secret. */
  assert(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, cipher,
                                          SIGNET_MGMT_OP_REISSUE_CONNECT, "evt-r", now) == 0);
  char *after_first = read_connect_secret(&f, "stew");
  assert(after_first != NULL);
  assert(strcmp(after_first, "initial-secret") != 0);

  /* Replay of the SAME event id is rejected and must not mint again. */
  assert(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, cipher,
                                          SIGNET_MGMT_OP_REISSUE_CONNECT, "evt-r", now) == -1);
  char *after_replay = read_connect_secret(&f, "stew");
  assert(after_replay != NULL);
  assert(strcmp(after_first, after_replay) == 0);

  /* A genuinely new request (new event id) mints a different secret. */
  assert(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, cipher,
                                          SIGNET_MGMT_OP_REISSUE_CONNECT, "evt-r2", now) == 0);
  char *after_second = read_connect_secret(&f, "stew");
  assert(after_second != NULL);
  assert(strcmp(after_second, after_replay) != 0);

  sodium_memzero(after_first, strlen(after_first)); g_free(after_first);
  sodium_memzero(after_replay, strlen(after_replay)); g_free(after_replay);
  sodium_memzero(after_second, strlen(after_second)); g_free(after_second);
  free(cipher);
  fixture_teardown(&f);
  printf("test_replayed_reissue_mints_once: PASS\n");
}

/* 4: with a replay cache, a missing event id fails closed (no execution). */
static void test_missing_event_id_fails_closed(void) {
  Fixture f;
  fixture_setup(&f, true);
  adopt_agent(&f, "stew");

  char *cipher = encrypt_to_bunker(&f, "{\"agent_id\":\"stew\",\"request_id\":\"r1\"}");
  int64_t now = 1752380000;

  assert(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, cipher,
                                          SIGNET_MGMT_OP_REISSUE_CONNECT, "", now) == -1);
  assert(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, cipher,
                                          SIGNET_MGMT_OP_REISSUE_CONNECT, NULL, now) == -1);

  /* The original secret is untouched. */
  char *secret = read_connect_secret(&f, "stew");
  assert(secret != NULL && strcmp(secret, "initial-secret") == 0);
  sodium_memzero(secret, strlen(secret));
  g_free(secret);

  free(cipher);
  fixture_teardown(&f);
  printf("test_missing_event_id_fails_closed: PASS\n");
}

int main(void) {
  if (sodium_init() < 0) {
    fprintf(stderr, "sodium_init failed\n");
    return 1;
  }

  test_no_cache_duplicates_execute();
  test_cache_rejects_duplicate();
  test_replayed_reissue_mints_once();
  test_missing_event_id_fails_closed();

  printf("All management replay tests passed.\n");
  return 0;
}
