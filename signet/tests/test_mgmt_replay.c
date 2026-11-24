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
#include "signet/store_audit.h"
#include <nostr-event.h>
#include <nostr/nip17/nip17.h>
#include <sqlite3.h>
#include <time.h>
#include "signet/audit_logger.h"

#include <nostr-keys.h>
#include <nostr/nip44/nip44.h>

#include "test_check.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <json-glib/json-glib.h>
#ifdef SIGNET_ENABLE_TEST_HOOKS
#include "mgmt_protocol_test_hooks.h"
#include "secret_json.h"
#endif
#include <sodium.h>

#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static const char *const RELAYS[] = { "wss://relay.example" };

static char *make_temp_db_path(void) {
  char tmpl[] = "/tmp/signet-test-mgmt-replay-XXXXXX.db";
  int fd = mkstemps(tmpl, 3);
  CHECK(fd >= 0);
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
  CHECK(sk && strlen(sk) == 64);
  char *pk = nostr_key_get_public(sk);
  CHECK(pk && strlen(pk) == 64);
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
  SignetAuditLogger *audit;
  char *db_path;
  char bunker_sk_hex[65];
  char bunker_pk_hex[65];
  char prov_sk_hex[65];
  char prov_pk_hex[65];
} Fixture;

static void fixture_open(Fixture *f, SignetRelayPool *pool) {
  SignetKeyStoreConfig kcfg = { .db_path = f->db_path, .master_key = MASTER_KEY };
  f->ks = signet_key_store_new(f->audit, &kcfg);
  CHECK(f->ks != NULL);

  const char *const provs[] = { f->prov_pk_hex };
  SignetMgmtHandlerConfig mcfg = {
    .provisioner_pubkeys = provs,
    .n_provisioner_pubkeys = 1,
    .bunker_secret_key_hex = f->bunker_sk_hex,
    .bunker_pubkey_hex = f->bunker_pk_hex,
    .relay_urls = RELAYS,
    .n_relay_urls = 1,
  };
  f->mgmt = signet_mgmt_handler_new(f->ks, pool, NULL, NULL, &mcfg);
  CHECK(f->mgmt != NULL);
}

static void fixture_setup_with_pool(Fixture *f, bool with_replay_cache, SignetRelayPool *pool) {
  memset(f, 0, sizeof(*f));
  gen_keypair_hex(f->bunker_sk_hex, f->bunker_pk_hex);
  gen_keypair_hex(f->prov_sk_hex, f->prov_pk_hex);
  f->db_path = make_temp_db_path();
  SignetAuditLoggerConfig alc = { .path = NULL, .to_stdout = false, .flush_each_write = false };
  f->audit = signet_audit_logger_new(&alc);
  CHECK(f->audit);
  fixture_open(f, pool);

  if (with_replay_cache) {
    SignetReplayCacheConfig rcfg = { .max_entries = 128, .ttl_seconds = 300, .skew_seconds = 300 };
    f->replay = signet_replay_cache_new(&rcfg);
    CHECK(f->replay != NULL);
    signet_mgmt_handler_set_replay_cache(f->mgmt, f->replay);
  }
}

static void fixture_setup(Fixture *f, bool with_replay_cache) {
  fixture_setup_with_pool(f, with_replay_cache, NULL);
}

static void fixture_teardown(Fixture *f) {
  signet_mgmt_handler_free(f->mgmt);
  signet_key_store_free(f->ks);
  signet_replay_cache_free(f->replay);
  signet_audit_logger_free(f->audit);
  unlink(f->db_path);
  g_free(f->db_path);
  sodium_memzero(f->bunker_sk_hex, sizeof(f->bunker_sk_hex));
  sodium_memzero(f->prov_sk_hex, sizeof(f->prov_sk_hex));
}

/* NIP-44 v2 encrypt `plaintext` from the provisioner to the bunker, as the
 * management transport does after gift-wrap unwrapping. Caller frees. */
static char *encrypt_to_bunker(Fixture *f, const char *plaintext) {
  uint8_t sk[32], pk[32];
  CHECK(hex_to_bytes(f->prov_sk_hex, sk, 32) == 0);
  CHECK(hex_to_bytes(f->bunker_pk_hex, pk, 32) == 0);
  char *cipher = NULL;
  int rc = nostr_nip44_encrypt_v2(sk, pk, (const uint8_t *)plaintext,
                                  strlen(plaintext), &cipher);
  sodium_memzero(sk, sizeof(sk));
  CHECK(rc == 0 && cipher != NULL);
  return cipher;
}

/* Adopt a known agent so non-idempotent commands have a target. */
static void adopt_agent(Fixture *f, const char *agent_id) {
  char sk_hex[65], pk_hex[65];
  gen_keypair_hex(sk_hex, pk_hex);
  uint8_t sk_raw[32];
  CHECK(hex_to_bytes(sk_hex, sk_raw, 32) == 0);
  sodium_memzero(sk_hex, sizeof(sk_hex));
  char out_pk[65] = {0};
  CHECK(signet_key_store_adopt_agent(f->ks, agent_id, sk_raw, pk_hex,
                                      "initial-secret", f->bunker_pk_hex,
                                      RELAYS, 1, out_pk, NULL) == SIGNET_ADOPT_OK);
  sodium_memzero(sk_raw, sizeof(sk_raw));
}

/* Read the currently persisted connect_secret for an agent. Caller frees. */
static char *read_connect_secret(Fixture *f, const char *agent_id) {
  SignetStore *st = signet_key_store_get_store(f->ks);
  CHECK(st != NULL);
  SignetAgentRecord rec;
  memset(&rec, 0, sizeof(rec));
  CHECK(signet_store_get_agent(st, agent_id, &rec) == 0);
  char *secret = rec.connect_secret ? g_strdup(rec.connect_secret) : NULL;
  signet_agent_record_clear(&rec);
  return secret;
}

/* Fresh outer wraps of one authenticated rumor must not mint twice. */
static void test_rewrapped_rumor_rejected(void) {
  Fixture f;
  fixture_setup(&f, true);
  int64_t now = time(NULL);
  const char *rpc = "{\"jsonrpc\":\"2.0\",\"id\":\"wrapped\","
    "\"method\":\"agent/provision\",\"params\":{\"agent_id\":\"rewrapped\"}}";
  NostrEvent *rumor = nostr_nip17_create_rumor(f.prov_pk_hex, f.bunker_pk_hex, rpc, now);
  CHECK(rumor);
  NostrEvent *seal = nostr_nip17_create_seal(rumor, f.prov_sk_hex, f.bunker_pk_hex);
  CHECK(seal);
  char *first_secret = NULL;
  for (int i = 0; i < 2; ++i) {
    NostrEvent *gift = nostr_nip17_create_gift_wrap(seal, f.bunker_pk_hex);
    CHECK(gift);
    char *json = nostr_event_serialize_compact(gift);
    char *id = nostr_event_get_id(gift);
    CHECK(signet_mgmt_handler_handle_gift_wrap(f.mgmt, json, id, now) == (i ? -1 : 0));
    char *secret = read_connect_secret(&f, "rewrapped");
    CHECK(secret);
    if (i) {
      CHECK(!strcmp(secret, first_secret));
      sodium_memzero(secret, strlen(secret)); g_free(secret);
    } else first_secret = secret;
    free(json); free(id); nostr_event_free(gift);
  }
  sodium_memzero(first_secret, strlen(first_secret)); g_free(first_secret);
  nostr_event_free(seal); nostr_event_free(rumor);
  fixture_teardown(&f);
  puts("test_rewrapped_rumor_rejected: PASS");
}

/* 1: without a replay cache, duplicates execute twice (opt-in wiring). */
static void test_no_cache_duplicates_execute(void) {
  Fixture f;
  fixture_setup(&f, false);

  int64_t now = 1752380000;
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, "",
                                          SIGNET_MGMT_OP_GET_STATUS, "evt-dup", now) == 0);
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, "",
                                          SIGNET_MGMT_OP_GET_STATUS, "evt-dup", now) == 0);

  fixture_teardown(&f);
  printf("test_no_cache_duplicates_execute: PASS\n");
}

/* 2: with a replay cache, a duplicate event id is rejected; fresh ids run. */
static void test_cache_rejects_duplicate(void) {
  Fixture f;
  fixture_setup(&f, true);

  int64_t now = 1752380000;
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, "",
                                          SIGNET_MGMT_OP_GET_STATUS, "evt-1", now) == 0);
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, "",
                                          SIGNET_MGMT_OP_GET_STATUS, "evt-1", now) == -1);
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, "",
                                          SIGNET_MGMT_OP_GET_STATUS, "evt-2", now) == 0);

  fixture_teardown(&f);
  printf("test_cache_rejects_duplicate: PASS\n");
}

static void test_provision_intent_preserves_identity(void) {
  Fixture f;
  fixture_setup(&f, true);
  const char *intent = "{\"jsonrpc\":\"2.0\",\"id\":\"provision\","
      "\"method\":\"agent/provision\",\"params\":{\"agent_id\":\"stable\"}}";
  int64_t now = 1752380000;
  CHECK(signet_mgmt_handler_handle_intent(f.mgmt, f.prov_pk_hex, intent,
      "provision-1", now, now) == 0);
  char first[65], second[65];
  CHECK(signet_key_store_get_agent_pubkey(f.ks, "stable", first, sizeof(first)));
  char *secret1 = read_connect_secret(&f, "stable");
  CHECK(secret1 != NULL);
  CHECK(signet_mgmt_handler_handle_intent(f.mgmt, f.prov_pk_hex, intent,
      "provision-2", now, now) == 0);
  CHECK(signet_key_store_get_agent_pubkey(f.ks, "stable", second, sizeof(second)));
  CHECK(strcmp(first, second) == 0);
  char *secret2 = read_connect_secret(&f, "stable");
  CHECK(secret2 != NULL && strcmp(secret1, secret2) != 0);
  CHECK(signet_mgmt_handler_handle_intent(f.mgmt, f.prov_pk_hex, intent,
      "provision-2", now, now) == -1);
  char *replayed = read_connect_secret(&f, "stable");
  CHECK(replayed != NULL && strcmp(secret2, replayed) == 0);
  const char *rotate = "{\"jsonrpc\":\"2.0\",\"id\":\"rotate\","
      "\"method\":\"agent/rotate-key\",\"params\":{\"agent_id\":\"stable\"}}";
  CHECK(signet_mgmt_handler_handle_intent(f.mgmt, f.prov_pk_hex, rotate,
      "rotate-1", now, now) == 0);
  CHECK(signet_key_store_get_agent_pubkey(f.ks, "stable", second, sizeof(second)));
  CHECK(strcmp(first, second) != 0);
  sodium_memzero(secret1, strlen(secret1)); g_free(secret1);
  sodium_memzero(secret2, strlen(secret2)); g_free(secret2);
  sodium_memzero(replayed, strlen(replayed)); g_free(replayed);
  fixture_teardown(&f);
  printf("test_provision_intent_preserves_identity: PASS\n");
}

/* 3: a replayed agent/reissue-connect mutates state exactly once. */
static void test_replayed_reissue_mints_once(void) {
  Fixture f;
  fixture_setup(&f, true);
  adopt_agent(&f, "stew");

  char *cipher = encrypt_to_bunker(&f, "{\"agent_id\":\"stew\",\"request_id\":\"r1\"}");
  int64_t now = 1752380000;

  /* First delivery executes and replaces the connect_secret. */
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, cipher,
                                          SIGNET_MGMT_OP_REISSUE_CONNECT, "evt-r", now) == 0);
  char *after_first = read_connect_secret(&f, "stew");
  CHECK(after_first != NULL);
  CHECK(strcmp(after_first, "initial-secret") != 0);

  /* Replay of the SAME event id is rejected and must not mint again. */
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, cipher,
                                          SIGNET_MGMT_OP_REISSUE_CONNECT, "evt-r", now) == -1);
  char *after_replay = read_connect_secret(&f, "stew");
  CHECK(after_replay != NULL);
  CHECK(strcmp(after_first, after_replay) == 0);

  /* A genuinely new request (new event id) mints a different secret. */
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, cipher,
                                          SIGNET_MGMT_OP_REISSUE_CONNECT, "evt-r2", now) == 0);
  char *after_second = read_connect_secret(&f, "stew");
  CHECK(after_second != NULL);
  CHECK(strcmp(after_second, after_replay) != 0);

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

  CHECK(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, cipher,
                                          SIGNET_MGMT_OP_REISSUE_CONNECT, "", now) == -1);
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, cipher,
                                          SIGNET_MGMT_OP_REISSUE_CONNECT, NULL, now) == -1);

  /* The original secret is untouched. */
  char *secret = read_connect_secret(&f, "stew");
  CHECK(secret != NULL && strcmp(secret, "initial-secret") == 0);
  sodium_memzero(secret, strlen(secret));
  g_free(secret);

  free(cipher);
  fixture_teardown(&f);
  printf("test_missing_event_id_fails_closed: PASS\n");
}

static void test_writer_management_requires_provisioner(void) {
  Fixture f;
  fixture_setup(&f, false);
  adopt_agent(&f, "service");
  const char *owner =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  int64_t rejected_epoch = 0;
  CHECK(signet_key_store_writer_acquire(f.ks, "service", f.prov_pk_hex,
                                        &rejected_epoch) != 0);
  CHECK(signet_store_writer_is_fenced(signet_key_store_get_store(f.ks),
                                      "service") == 0);
  char *plain = g_strdup_printf(
      "{\"agent_id\":\"service\",\"writer_pubkey\":\"%s\"}", owner);
  char *cipher = encrypt_to_bunker(&f, plain);
  g_free(plain);
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, owner, cipher,
      SIGNET_MGMT_OP_WRITER_ACQUIRE, "unauthorized", 1752380000) == -1);
  CHECK(signet_store_writer_is_fenced(signet_key_store_get_store(f.ks),
                                      "service") == 0);
  char *intent = g_strdup_printf(
      "{\"jsonrpc\":\"2.0\",\"id\":\"lease-1\",\"method\":\"agent/writer-acquire\","
      "\"params\":{\"agent_id\":\"service\",\"writer_pubkey\":\"%s\"}}",
      owner);
  CHECK(signet_mgmt_handler_handle_intent(f.mgmt, f.prov_pk_hex, intent,
                                          "acquire", 1752380000, 1752380000) == 0);
  g_free(intent);
  CHECK(signet_store_writer_is_fenced(signet_key_store_get_store(f.ks),
                                      "service") == 1);
  /* Lease renewal no longer exists on the management plane. */
  intent = g_strdup_printf(
      "{\"jsonrpc\":\"2.0\",\"id\":\"renew-1\",\"method\":\"agent/writer-renew\","
      "\"params\":{\"agent_id\":\"service\",\"writer_pubkey\":\"%s\",\"epoch\":1}}",
      owner);
  CHECK(signet_mgmt_handler_handle_intent(f.mgmt, f.prov_pk_hex, intent,
                                          "renew", 1752380000, 1752380000) != 0);
  g_free(intent);
  const char *next_owner =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  int64_t next_epoch = 0;
  CHECK(signet_key_store_writer_acquire(f.ks, "service", next_owner,
                                        &next_epoch) == 0);
  CHECK(next_epoch == 2);
  /* Former owner cannot use the management plane to reacquire itself. */
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, owner, cipher,
      SIGNET_MGMT_OP_WRITER_ACQUIRE, "stale-owner", 1752380001) == -1);
  CHECK(!signet_store_is_provisioner(signet_key_store_get_store(f.ks), owner));
  /* Nor can a provisioner reinstate it: writer client keys are single-use. */
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, cipher,
      SIGNET_MGMT_OP_WRITER_ACQUIRE, "reinstate", 1752380001) == -1);
  free(cipher);
  cipher = encrypt_to_bunker(&f, "{\"agent_id\":\"service\"}");
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, cipher,
      SIGNET_MGMT_OP_WRITER_REVOKE, "revoke", 1752380000) == 0);
  free(cipher);
  plain = g_strdup_printf(
      "{\"agent_id\":\"service\",\"writer_pubkey\":\"%s\"}", next_owner);
  cipher = encrypt_to_bunker(&f, plain);
  g_free(plain);
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, f.prov_pk_hex, cipher,
      SIGNET_MGMT_OP_WRITER_ACQUIRE, "revoked-owner", 1752380002) == -1);
  CHECK(signet_mgmt_handler_handle_request(f.mgmt, next_owner, cipher,
      SIGNET_MGMT_OP_WRITER_ACQUIRE, "revoked-self", 1752380002) == -1);
  free(cipher);
  fixture_teardown(&f);
  printf("test_writer_management_requires_provisioner: PASS\n");
}

static void test_direct_ingress_requires_gift_wrap(void) {
  Fixture f;
  fixture_setup(&f, true);
  int64_t now = time(NULL);
  char agent_sk[65], agent_pk[65];
  gen_keypair_hex(agent_sk, agent_pk);
  char *rpc = g_strdup_printf(
      "{\"jsonrpc\":\"2.0\",\"id\":\"adopt\","
      "\"method\":\"agent/adopt-existing\",\"params\":{"
      "\"agent_id\":\"private-agent\",\"agent_nsec\":\"%s\"}}", agent_sk);
  SignetStore *store = signet_key_store_get_store(f.ks);
  CHECK(signet_mgmt_handler_handle_direct_intent(f.mgmt, f.prov_pk_hex,
      rpc, "direct-adopt", now, now) == -1);
  SignetAgentRecord rec = {0};
  CHECK(signet_store_get_agent(store, "private-agent", &rec) != 0);
  signet_agent_record_clear(&rec);

  const char *methods[] = {"agent/restore-existing", "credential/create",
      "credential/import", "credential/rotate", "agent/revoke",
      "credential/deliver"};
  for (size_t i = 0; i < G_N_ELEMENTS(methods); i++) {
    char *direct = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\","
        "\"params\":{\"agent_nsec\":\"%s\",\"payload_b64\":\"PRIVATE-PAYLOAD\"}}",
        methods[i], agent_sk);
    CHECK(signet_mgmt_handler_handle_direct_intent(f.mgmt, f.prov_pk_hex,
        direct, "direct-other", now, now) == -1);
    sodium_memzero(direct, strlen(direct));
    g_free(direct);
  }
  sqlite3_stmt *stmt = NULL;
  CHECK(sqlite3_prepare_v2(signet_store_get_db(store),
      "SELECT detail FROM audit_log WHERE operation='mgmt_direct_refused'",
      -1, &stmt, NULL) == SQLITE_OK);
  int rows = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *detail = (const char *)sqlite3_column_text(stmt, 0);
    CHECK(detail && strstr(detail, "gift_wrap_required"));
    CHECK(!strstr(detail, agent_sk) && !strstr(detail, "PRIVATE-PAYLOAD"));
    rows++;
  }
  sqlite3_finalize(stmt);
  CHECK(rows == 7);
  CHECK(signet_audit_verify_chain(store, 0, 0, NULL) == 0);

  /* Exercise the real authenticated unwrap, not just the inner dispatcher. */
  NostrEvent *gift = nostr_nip17_wrap_dm(f.prov_sk_hex, f.bunker_pk_hex, rpc);
  CHECK(gift);
  char *json = nostr_event_serialize_compact(gift);
  CHECK(json);
  CHECK(signet_mgmt_handler_handle_gift_wrap(f.mgmt, json, "wrapped-adopt",
      time(NULL)) == 0);
  CHECK(signet_store_get_agent(store, "private-agent", &rec) == 0);
  uint8_t expected_sk[32];
  CHECK(hex_to_bytes(agent_sk, expected_sk, sizeof(expected_sk)) == 0);
  CHECK(rec.secret_key_len == sizeof(expected_sk));
  CHECK(sodium_memcmp(rec.secret_key, expected_sk, sizeof(expected_sk)) == 0);
  sodium_memzero(expected_sk, sizeof(expected_sk));
  signet_agent_record_clear(&rec);
  free(json);
  nostr_event_free(gift);
  sodium_memzero(rpc, strlen(rpc));
  g_free(rpc);
  sodium_memzero(agent_sk, sizeof(agent_sk));

  CHECK(signet_mgmt_handler_handle_direct_intent(f.mgmt, f.prov_pk_hex,
      "{\"jsonrpc\":\"2.0\",\"id\":\"list\",\"method\":\"agent/list\",\"params\":{}}",
      "direct-list", now, now) == 0);
  fixture_teardown(&f);
  printf("test_direct_ingress_requires_gift_wrap: PASS\n");
}

/* Real relay-pool ingress fixture. The relay fans every live event out to
 * each of its open REQs, oldest first, as a real relay does, and signetd's
 * startup leaves two REQs with the same filter on a fast relay
 * (nostrc-5xqun). Exercise an older draining REQ, a socket reconnect, a peer
 * relay that never reaches EOSE and a relay-set reload; history/EOSE/live
 * boundaries never depend on sleeps. */
typedef struct {
  Fixture f;
  SignetRelayPool *pool;
  GMainLoop *loop;
  const char *url;    /* the answering relay: subscribe on its first CONNECTED */
  gint accepted;
  gint dropped;
  gint retired_dropped;
  char *retired_id;
  int eoses;
  bool need_live;
  bool subscribed;
  bool isolate;
  bool reconnect;
  bool reload;
  gint64 started;
} IngressWire;

static char *adopt_gift(Fixture *f, const char *agent, int64_t rumor_at) {
  char sk[65], pk[65];
  gen_keypair_hex(sk, pk);
  char *rpc = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":\"%s\","
      "\"method\":\"agent/adopt-existing\",\"params\":{"
      "\"agent_id\":\"%s\",\"agent_nsec\":\"%s\"}}", agent, agent, sk);
  NostrEvent *rumor = nostr_nip17_create_rumor(f->prov_pk_hex,
      f->bunker_pk_hex, rpc, rumor_at);
  CHECK(rumor);
  NostrEvent *seal = nostr_nip17_create_seal(rumor, f->prov_sk_hex, f->bunker_pk_hex);
  CHECK(seal);
  NostrEvent *gift = nostr_nip17_create_gift_wrap(seal, f->bunker_pk_hex);
  CHECK(gift);
  char *json = nostr_event_serialize_compact(gift);
  CHECK(json);
  nostr_event_free(gift);
  nostr_event_free(seal);
  nostr_event_free(rumor);
  sodium_memzero(rpc, strlen(rpc));
  g_free(rpc);
  sodium_memzero(sk, sizeof(sk));
  return json;
}

static gboolean ingress_reload(void *data) {
  IngressWire *w = data;
  /* signetd changes its relay set from the main loop (SIGHUP). The fresh
   * pool must replay the subscription and admit the next live request. */
  w->need_live = true;
  CHECK(signet_relay_pool_set_relays(w->pool, &w->url, 1) == 0);
  return G_SOURCE_REMOVE;
}

static void ingress_event(const SignetRelayEventView *ev, void *data) {
  IngressWire *w = data;
  int rc = signet_mgmt_handler_handle_relay_event(w->f.mgmt, ev, time(NULL));
  if (!g_strcmp0(ev->event_id_hex, w->retired_id)) {
    /* CLOSE may already have removed this id from libnostr. If it still
     * drains, it must be audited as refused, never inherit the new EOSE. */
    CHECK(rc != 0 && ev->live_since == 0);
    CHECK(g_atomic_int_add(&w->retired_dropped, 1) == 0);
    return;
  }
  if (rc != 0) {
    g_atomic_int_inc(&w->dropped);
    return;
  }
  CHECK(ev->live_since > 0);
  CHECK(g_get_monotonic_time() - w->started < 10 * G_USEC_PER_SEC);
  int accepted = g_atomic_int_add(&w->accepted, 1) + 1;
  int phases = (w->reconnect || w->reload) ? 2 : 1;
  if (accepted % 2 == 1) {
    /* The warm-up came back on the required REQ alone, so the pool has
     * processed every older REQ's EOSE ahead of it. The relay fans these
     * out to all of its open REQs, oldest first: a still-fresh rumor from
     * BEFORE the boundary must not execute even when delivered live, and
     * the live mutation must execute whichever REQ carries it. Outer wrap
     * timestamps are randomized. */
    int phase = (accepted + 1) / 2;
    char agent[32];
    g_snprintf(agent, sizeof(agent), "stale%d", phase);
    char *json = adopt_gift(&w->f, agent, time(NULL) - 30);
    CHECK(signet_relay_pool_publish_event_json(w->pool, json) == 0);
    free(json);
    g_snprintf(agent, sizeof(agent), "live%d", phase);
    json = adopt_gift(&w->f, agent, time(NULL));
    CHECK(signet_relay_pool_publish_event_json(w->pool, json) == 0);
    free(json);
    return;
  }
  if (accepted == phases * 2) {
    g_main_loop_quit(w->loop);
    return;
  }
  if (w->reload) {
    g_idle_add(ingress_reload, w);
    return;
  }
  /* A fixture control publication asks the relay to disconnect only AFTER
   * the first live mutation has committed. It is not management traffic. */
  NostrEvent *control = nostr_event_new();
  nostr_event_set_kind(control, 12345);
  nostr_event_set_created_at(control, time(NULL));
  nostr_event_set_content(control, "reconnect");
  CHECK(nostr_event_sign(control, w->f.prov_sk_hex) == 0);
  char *json = nostr_event_serialize_compact(control);
  CHECK(signet_relay_pool_publish_event_json(w->pool, json) == 0);
  free(json);
  nostr_event_free(control);
}

static void ingress_subscription(const char *url, const char *state,
                                  const char *reason, void *data) {
  (void)reason;
  IngressWire *w = data;
  if (strcmp(state, "CONNECTED") == 0) {
    if (w->subscribed || strcmp(url, w->url)) return;
    /* Like signetd, subscribe after start. Isolation explicitly replaces a
     * draining REQ; it must be closed, never made live by the new one's EOSE. */
    w->subscribed = true;
    int kinds[] = {CAS_INTENT, NIP59_GIFT_WRAP};
    CHECK(signet_relay_pool_subscribe_scoped(w->pool, kinds, 2,
        w->f.bunker_pk_hex, time(NULL) - 49 * 3600) == 0);
    if (w->isolate)
      CHECK(signet_relay_pool_subscribe_scoped(w->pool, kinds, 2,
          w->f.bunker_pk_hex, time(NULL) - 49 * 3600) == 0);
    return;
  }
  if (strcmp(state, "DISCONNECTED") == 0) w->need_live = true;
  if (strcmp(state, "EOSE") || !w->need_live) return;
  w->need_live = false;
  CHECK(++w->eoses <= 2);
  CHECK(signet_relay_pool_is_subscribed(w->pool));
  /* Warm-up: a live mutation the relay answers on the required REQ only. */
  char agent[32];
  g_snprintf(agent, sizeof(agent), "warm%d", w->eoses);
  char *json = adopt_gift(&w->f, agent, time(NULL));
  CHECK(signet_relay_pool_publish_event_json(w->pool, json) == 0);
  free(json);
}

static gboolean ingress_deadline(void *data) {
  g_main_loop_quit(((IngressWire *)data)->loop);
  return G_SOURCE_REMOVE;
}

static int test_wire_ingress(const char *url, const char *fixture_path,
                             bool isolate, bool reconnect, bool reload,
                             const char *peer_url) {
  IngressWire w = {.url = url, .need_live = true, .isolate = isolate,
                   .reconnect = reconnect, .reload = reload};
  fixture_setup(&w.f, true);
  /* Future-dated history is inside the skew window and after the coming
   * EOSE timestamp; the pre-EOSE classification must still reject it. */
  char *history1 = adopt_gift(&w.f, "history1", time(NULL) + 60);
  char *history2 = adopt_gift(&w.f, "history2", time(NULL) + 60);
  char *late = adopt_gift(&w.f, "history-late", time(NULL) + 60);
  NostrEvent *late_event = nostr_event_new(); CHECK(late_event);
  CHECK(nostr_event_deserialize_compact(late_event, late, NULL) == 1);
  w.retired_id = nostr_event_get_id(late_event); CHECK(w.retired_id);
  nostr_event_free(late_event);
  char *fixtures = g_strdup_printf("[%s,%s,%s]", history1, history2, late);
  CHECK(g_file_set_contents(fixture_path, fixtures, -1, NULL));
  g_free(fixtures);
  free(history1);
  free(history2);
  free(late);
  w.loop = g_main_loop_new(NULL, FALSE);
  const char *relays[2] = {url, peer_url};
  SignetRelayPoolConfig cfg = {.relays = relays, .n_relays = peer_url ? 2 : 1,
      .on_event = ingress_event, .user_data = &w};
  w.pool = signet_relay_pool_new(&cfg);
  CHECK(w.pool);
  signet_relay_pool_set_subscription_callback(w.pool, ingress_subscription, &w);
  w.started = g_get_monotonic_time();
  /* Like signetd: start first, subscribe once the relay reports CONNECTED. */
  CHECK(signet_relay_pool_start(w.pool) == 0);
  guint deadline = g_timeout_add_seconds(12, ingress_deadline, &w);
  g_main_loop_run(w.loop);
  int phases = (reconnect || reload) ? 2 : 1;
  int expected_accepted = phases * 2;              /* warm-up + live per phase */
  int expected_dropped = phases * 2; /* history + stale; closed REQ late delivery is not forwarded */
  if (g_atomic_int_get(&w.accepted) == expected_accepted) g_source_remove(deadline);
  signet_relay_pool_free(w.pool);
  CHECK(w.eoses == phases && g_atomic_int_get(&w.accepted) == expected_accepted);
  CHECK(g_atomic_int_get(&w.dropped) == expected_dropped);
  SignetStore *store = signet_key_store_get_store(w.f.ks);
  const char *refused[] = {"history1", "history2", "history-late", "stale1", "stale2"};
  for (size_t i = 0; i < G_N_ELEMENTS(refused); i++) {
    SignetAgentRecord rec = {0};
    CHECK(signet_store_get_agent(store, refused[i], &rec) != 0);
    signet_agent_record_clear(&rec);
  }
  const char *executed[] = {"warm1", "live1", "warm2", "live2"};
  for (size_t i = 0; i < G_N_ELEMENTS(executed); i++) {
    SignetAgentRecord rec = {0};
    int rc = signet_store_get_agent(store, executed[i], &rec);
    CHECK((i < 2 || phases == 2) ? rc == 0 : rc != 0);
    signet_agent_record_clear(&rec);
  }
  sqlite3_stmt *stmt = NULL;
  CHECK(sqlite3_prepare_v2(signet_store_get_db(store),
      "SELECT count(*) FROM audit_log WHERE operation='mgmt_history_dropped'",
      -1, &stmt, NULL) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == expected_dropped + g_atomic_int_get(&w.retired_dropped));
  sqlite3_finalize(stmt);
  free(w.retired_id);
  fixture_teardown(&w.f);
  g_main_loop_unref(w.loop);
  puts("wire: one active REQ, per-REQ history/EOSE admission, retired delivery refusal, reconnect/reload PASS");
  return 0;
}

static void test_agent_id_validation(void) {
  Fixture f;
  fixture_setup(&f, false);
  const char *escaped_ids[] = {
    "with\\\"quote", "with\\\\slash", "with\\u0001control", "with\\u0000nul",
    "with space", "caf\\u00e9", "",
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  };
  for (size_t i = 0; i < G_N_ELEMENTS(escaped_ids); i++) {
    g_autofree char *intent = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"agent/provision\","
        "\"params\":{\"agent_id\":\"%s\"}}", escaped_ids[i]);
    CHECK(signet_mgmt_handler_handle_intent(f.mgmt, f.prov_pk_hex, intent,
        "invalid-agent-id", 1752380000, 1752380000) == -1);
  }
  char **ids = NULL;
  size_t count = 0;
  CHECK(signet_key_store_list_agents(f.ks, &ids, &count) == 0 && count == 0);
  g_strfreev(ids);

  const char *bad_id = "old\"\\\001agent";
  uint8_t sk[32] = { [31] = 1 };
  char pk[65];
  CHECK(signet_key_store_provision_agent(f.ks, bad_id, f.bunker_pk_hex,
      RELAYS, 1, pk, sizeof(pk), NULL) != 0);
  CHECK(signet_key_store_adopt_agent(f.ks, bad_id, sk, NULL, NULL,
      f.bunker_pk_hex, RELAYS, 1, pk, NULL) != SIGNET_ADOPT_OK);
  /* Restore updates an existing row, so seed one as a pre-upgrade record. */
  SignetStore *st = signet_key_store_get_store(f.ks);
  char *original_pk = nostr_key_get_public(
      "0000000000000000000000000000000000000000000000000000000000000001");
  CHECK(original_pk);
  CHECK(signet_store_put_agent_ex(st, bad_id, sk, sizeof(sk), NULL,
                                  original_pk, "adopted", 1752380000) == 0);
  CHECK(signet_key_store_restore_agent(f.ks, bad_id, sk, original_pk, pk)
        != SIGNET_ADOPT_OK);
  const SignetMgmtOp mint_ops[] = {
    SIGNET_MGMT_OP_PROVISION_AGENT, SIGNET_MGMT_OP_ADOPT_EXISTING,
    SIGNET_MGMT_OP_RESTORE_EXISTING,
  };
  for (size_t i = 0; i < G_N_ELEMENTS(mint_ops); i++) {
    SignetMgmtRequest req;
    g_autofree char *err = NULL;
    CHECK(signet_mgmt_request_parse(mint_ops[i],
        "{\"agent_id\":\"old\\\"agent\",\"agent_nsec\":\"test\"}", &req, &err) == -1);
    CHECK(err && strstr(err, "agent_id"));
  }
  const char *boundary_id =
      "A-z_09.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  CHECK(strlen(boundary_id) == 64);
  CHECK(signet_key_store_provision_agent(f.ks, boundary_id, f.bunker_pk_hex,
      RELAYS, 1, pk, sizeof(pk), NULL) == 0);
  free(original_pk);
  fixture_teardown(&f);
}

#ifdef SIGNET_ENABLE_TEST_HOOKS
static void capture_mgmt_reply(void *data, const char *recipient, const char *reply) {
  (void)recipient;
  char **out = data;
  CHECK(!*out);
  *out = g_strdup(reply);
}

static JsonObject *call_mgmt(Fixture *f, SignetMgmtOp op, const char *params,
                             SignetSecretJsonParser *parser) {
  char *reply = NULL;
  signet_mgmt_handler_set_reply_hook(f->mgmt, capture_mgmt_reply, &reply);
  char *cipher = params ? encrypt_to_bunker(f, params) : NULL;
  int rc = signet_mgmt_handler_handle_request(f->mgmt, f->prov_pk_hex, cipher,
                                               op, "json", 1752380000);
  signet_mgmt_handler_set_reply_hook(f->mgmt, NULL, NULL);
  free(cipher);
  CHECK(reply);
  CHECK(json_parser_load_from_data(parser, reply, -1, NULL));
  signet_secret_gfree(reply);
  JsonNode *root = json_parser_get_root(parser);
  CHECK(JSON_NODE_HOLDS_OBJECT(root));
  JsonObject *ack = json_node_get_object(root);
  if (rc != 0)
    fprintf(stderr, "%s failed (%s)\n", signet_mgmt_op_to_string(op),
            json_object_get_string_member(ack, "code"));
  CHECK(rc == 0 && json_object_get_boolean_member(ack, "ok"));
  return ack;
}

static void test_mgmt_result_json(void) {
  Fixture f;
  fixture_setup(&f, false);
  SignetStore *st = signet_key_store_get_store(f.ks);
  const char *legacy_id = "old\"\\\001agent";
  const char *legacy_params = "{\"agent_id\":\"old\\\"\\\\\\u0001agent\"}";
  char secret[65], expected_pk[65], pk[65];
  uint8_t raw[32];
  gen_keypair_hex(secret, expected_pk);
  CHECK(hex_to_bytes(secret, raw, sizeof(raw)) == 0);
  CHECK(signet_store_put_agent_ex(st, legacy_id, raw, sizeof(raw), NULL,
                                  expected_pk, "adopted", 1752380000) == 0);
  sodium_memzero(raw, sizeof(raw));
  signet_mgmt_handler_free(f.mgmt);
  signet_key_store_free(f.ks);
  fixture_open(&f, NULL);
  CHECK(signet_key_store_get_agent_pubkey(f.ks, legacy_id, pk, sizeof(pk)));
  CHECK(strcmp(pk, expected_pk) == 0);

  g_autoptr(SignetSecretJsonParser) p = json_parser_new();
  JsonObject *ack = call_mgmt(&f, SIGNET_MGMT_OP_LIST_AGENTS, NULL, p);
  CHECK(json_object_has_member(ack, "result"));
  JsonObject *res = json_object_get_object_member(ack, "result");
  CHECK(res && json_object_get_int_member(res, "count") == 1);
  JsonArray *agents = json_object_get_array_member(res, "agents");
  CHECK(agents && json_array_get_length(agents) == 1);
  CHECK(g_strcmp0(json_array_get_string_element(agents, 0), legacy_id) == 0);

  ack = call_mgmt(&f, SIGNET_MGMT_OP_ROTATE_KEY, legacy_params, p);
  res = json_object_get_object_member(ack, "result");
  CHECK(res && g_strcmp0(json_object_get_string_member(res, "agent_id"), legacy_id) == 0);
  CHECK(signet_key_store_get_agent_pubkey(f.ks, legacy_id, pk, sizeof(pk)));
  CHECK(g_strcmp0(json_object_get_string_member(res, "new_pubkey"), pk) == 0);
  CHECK(strcmp(expected_pk, pk) != 0);
  ack = call_mgmt(&f, SIGNET_MGMT_OP_REVOKE_AGENT, legacy_params, p);
  CHECK(g_strcmp0(json_object_get_string_member(ack, "code"), "revoked") == 0);
  CHECK(strstr(json_object_get_string_member(ack, "message"), legacy_id));
  CHECK(!signet_key_store_get_agent_pubkey(f.ks, legacy_id, pk, sizeof(pk)));
  SignetAgentRecord revoked = {0};
  CHECK(signet_store_get_agent(signet_key_store_get_store(f.ks), legacy_id, &revoked) == 1);
  signet_agent_record_clear(&revoked);

  ack = call_mgmt(&f, SIGNET_MGMT_OP_PROVISION_AGENT, "{\"agent_id\":\"new\"}", p);
  res = json_object_get_object_member(ack, "result");
  CHECK(res && g_strcmp0(json_object_get_string_member(res, "agent_id"), "new") == 0);
  CHECK(signet_key_store_get_agent_pubkey(f.ks, "new", pk, sizeof(pk)));
  CHECK(g_strcmp0(json_object_get_string_member(res, "pubkey"), pk) == 0);
  CHECK(g_str_has_prefix(json_object_get_string_member(res, "bunker_uri"), "bunker://"));
  CHECK(!json_object_has_member(res, "delivered"));

  char *adopt_params = g_strdup_printf(
      "{\"agent_id\":\"adopted\",\"agent_nsec\":\"%s\",\"connect_secret\":\"test\\u00e9\"}", secret);
  ack = call_mgmt(&f, SIGNET_MGMT_OP_ADOPT_EXISTING, adopt_params, p);
  res = json_object_get_object_member(ack, "result");
  CHECK(res && json_object_get_boolean_member(res, "adopted"));
  CHECK(g_strcmp0(json_object_get_string_member(res, "agent_id"), "adopted") == 0);
  CHECK(g_strcmp0(json_object_get_string_member(res, "pubkey"), expected_pk) == 0);
  CHECK(g_str_has_suffix(json_object_get_string_member(res, "bunker_uri"), "&secret=testé"));
  CHECK(!json_object_has_member(res, "delivered"));
  signet_secret_gfree(adopt_params);

  char *restore_params = g_strdup_printf(
      "{\"agent_id\":\"adopted\",\"agent_nsec\":\"%s\"}", secret);
  sodium_memzero(secret, sizeof(secret));
  ack = call_mgmt(&f, SIGNET_MGMT_OP_RESTORE_EXISTING, restore_params, p);
  signet_secret_gfree(restore_params);
  res = json_object_get_object_member(ack, "result");
  CHECK(res && json_object_get_boolean_member(res, "restored"));
  CHECK(!json_object_has_member(res, "bunker_uri"));
  CHECK(g_strcmp0(json_object_get_string_member(res, "agent_id"), "adopted") == 0);
  CHECK(g_strcmp0(json_object_get_string_member(res, "pubkey"), expected_pk) == 0);

  ack = call_mgmt(&f, SIGNET_MGMT_OP_GET_STATUS, NULL, p);
  res = json_object_get_object_member(ack, "result");
  CHECK(res && json_object_get_boolean_member(res, "db_open"));
  CHECK(!json_object_get_boolean_member(res, "relays_connected"));
  CHECK(json_object_get_int_member(res, "agents") > 0);

  const char *writer = "{\"agent_id\":\"new\",\"writer_pubkey\":"
      "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}";
  ack = call_mgmt(&f, SIGNET_MGMT_OP_WRITER_ACQUIRE, writer, p);
  res = json_object_get_object_member(ack, "result");
  CHECK(res && json_object_get_int_member(res, "epoch") == 1);
  ack = call_mgmt(&f, SIGNET_MGMT_OP_WRITER_REVOKE, "{\"agent_id\":\"new\"}", p);
  res = json_object_get_object_member(ack, "result");
  CHECK(res && json_object_get_int_member(res, "epoch") == 2);
  fixture_teardown(&f);
}

static void delivery_relay_connected(const char *url, const char *state,
                                      const char *reason, void *data) {
  (void)url;
  (void)reason;
  if (strcmp(state, "CONNECTED") == 0) g_main_loop_quit(data);
}

static gboolean delivery_relay_deadline(void *data) {
  g_main_loop_quit(data);
  return G_SOURCE_CONTINUE; /* removed by the caller */
}

static void test_mgmt_delivery_result_json(const char *url) {
  SignetRelayPoolConfig cfg = { .relays = &url, .n_relays = 1 };
  SignetRelayPool *pool = signet_relay_pool_new(&cfg);
  CHECK(pool);
  g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
  signet_relay_pool_set_subscription_callback(pool, delivery_relay_connected, loop);
  CHECK(signet_relay_pool_start(pool) == 0);
  guint deadline = g_timeout_add_seconds(10, delivery_relay_deadline, loop);
  g_main_loop_run(loop);
  g_source_remove(deadline);
  CHECK(signet_relay_pool_is_connected(pool));

  Fixture f;
  fixture_setup_with_pool(&f, false, pool);
  g_autoptr(SignetSecretJsonParser) p = json_parser_new();
  const SignetMgmtOp ops[] = {SIGNET_MGMT_OP_PROVISION_AGENT, SIGNET_MGMT_OP_ADOPT_EXISTING};
  for (size_t i = 0; i < G_N_ELEMENTS(ops); i++) {
    const char *agent = i == 0 ? "provisioned" : "adopted";
    char secret[65], pubkey[65];
    gen_keypair_hex(secret, pubkey);
    char *params = g_strdup_printf(
        "{\"agent_id\":\"%s\",\"agent_nsec\":\"%s\",\"deliver\":true,"
        "\"bootstrap_pubkey\":\"%s\",\"delivery_ttl\":123}", agent, secret, f.prov_pk_hex);
    sodium_memzero(secret, sizeof(secret));
    JsonObject *ack = call_mgmt(&f, ops[i], params, p);
    signet_secret_gfree(params);
    CHECK(json_object_has_member(ack, "result"));
    JsonObject *res = json_object_get_object_member(ack, "result");
    CHECK(res && json_object_get_boolean_member(res, "delivered"));
    CHECK(json_object_get_int_member(res, "delivery_expires_at") == 1752380123);
    CHECK(g_strcmp0(json_object_get_string_member(res, "agent_id"), agent) == 0);
    CHECK(g_str_has_prefix(json_object_get_string_member(res, "bunker_uri"), "bunker://"));
    if (i == 1) {
      CHECK(json_object_get_boolean_member(res, "adopted"));
      CHECK(g_strcmp0(json_object_get_string_member(res, "pubkey"), pubkey) == 0);
    }
  }
  fixture_teardown(&f);
  signet_relay_pool_free(pool);
}
#endif

int main(int argc, char **argv) {
  if (sodium_init() < 0) {
    fprintf(stderr, "sodium_init failed\n");
    return 1;
  }

  if (argc == 2 && strcmp(argv[1], "--agent-id") == 0) {
    test_agent_id_validation();
    return 0;
  }
#ifdef SIGNET_ENABLE_TEST_HOOKS
  if (argc == 2 && strcmp(argv[1], "--result-json") == 0) {
    test_mgmt_result_json();
    return 0;
  }
  if (argc == 3 && strcmp(argv[1], "--delivery-json") == 0) {
    test_mgmt_delivery_result_json(argv[2]);
    return 0;
  }
#endif

  if (argc >= 4 && strncmp(argv[1], "--wire-", 7) == 0) {
    const char *mode = argv[1] + 7;
    bool isolate = !strcmp(mode, "isolation") || !strcmp(mode, "combined");
    bool reconnect = !strcmp(mode, "ingress") || !strcmp(mode, "combined");
    bool reload = !strcmp(mode, "reload");
    bool multi = !strcmp(mode, "multi");
    const char *peer = argc > 4 ? argv[4] : NULL;
    if (!isolate && !reconnect && !reload && !multi) {
      fprintf(stderr, "unknown wire mode %s\n", argv[1]);
      return 2;
    }
    if ((multi || reload) && !peer) {
      fprintf(stderr, "%s needs a peer relay URL\n", argv[1]);
      return 2;
    }
    return test_wire_ingress(argv[2], argv[3], isolate, reconnect, reload, peer);
  }

  test_no_cache_duplicates_execute();
  test_cache_rejects_duplicate();
  test_rewrapped_rumor_rejected();
  test_provision_intent_preserves_identity();
  test_replayed_reissue_mints_once();
  test_missing_event_id_fails_closed();
  test_writer_management_requires_provisioner();
  test_direct_ingress_requires_gift_wrap();

  printf("All management replay tests passed.\n");
  return 0;
}
