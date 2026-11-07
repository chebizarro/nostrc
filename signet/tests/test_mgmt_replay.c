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
  /* No relay pool: ack publishing becomes a no-op, which is fine — these
   * tests assert on handler return codes and persisted store state. */
  f->mgmt = signet_mgmt_handler_new(f->ks, NULL, NULL, NULL, &mcfg);
  CHECK(f->mgmt != NULL);

  if (with_replay_cache) {
    SignetReplayCacheConfig rcfg = { .max_entries = 128, .ttl_seconds = 300, .skew_seconds = 300 };
    f->replay = signet_replay_cache_new(&rcfg);
    CHECK(f->replay != NULL);
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
    /* signetd subscribes after start, once the relay is already up: the REQ
     * fires immediately and the CONNECTED re-subscribe installs a second
     * REQ with the same filter right behind it. The relay answers fresh
     * requests on the older one first. */
    w->subscribed = true;
    int kinds[] = {CAS_INTENT, NIP59_GIFT_WRAP};
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
  int expected_dropped = phases * 2 + (isolate ? 1 : 0); /* history + stale, late */
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
  CHECK(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == expected_dropped);
  sqlite3_finalize(stmt);
  fixture_teardown(&w.f);
  g_main_loop_unref(w.loop);
  puts(isolate && reconnect ? "wire: draining-REQ isolation, live mutation on the older REQ, reconnect and second live mutation PASS"
       : isolate ? "wire: draining startup REQ refused after the required EOSE PASS"
       : reload ? "wire: live mutations on the older REQ before and after relay-set reload PASS"
       : peer_url ? "wire: live mutation on the older REQ beside a peer relay that never reaches EOSE PASS"
               : "wire: startup REQ pair, pre-EOSE history and old rumors audited; live mutations on the older REQ within 10s and reconnect PASS");
  return 0;
}

int main(int argc, char **argv) {
  if (sodium_init() < 0) {
    fprintf(stderr, "sodium_init failed\n");
    return 1;
  }

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
  test_replayed_reissue_mints_once();
  test_missing_event_id_fails_closed();
  test_writer_management_requires_provisioner();
  test_direct_ingress_requires_gift_wrap();

  printf("All management replay tests passed.\n");
  return 0;
}
