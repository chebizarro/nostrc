/* SPDX-License-Identifier: MIT
 *
 * test_reissue_self_service.c - Proves the self-service authorization path for
 * agent/reissue-connect (and ONLY that method):
 *   1. a provisioner can reissue for any existing agent (unchanged);
 *   2. an agent can reissue for ITSELF: sender pubkey == agent identity pubkey;
 *   3. an agent CANNOT reissue for a different agent (unauthorized, no mutation);
 *   4. an unknown sender cannot reissue (unauthorized, no mutation) — and gets
 *      the same answer for an unknown agent_id (no existence probing);
 *   5. a replayed identical self-service event still executes exactly once;
 *   6. the freshly minted secret consumes exactly once and the old secret is
 *      dead;
 *   7. the self-service exception does NOT leak to other management methods:
 *      an agent sending agent/get-status is silently dropped.
 */

#include "signet/key_store.h"
#include "signet/mgmt_protocol.h"
#include "signet/replay_cache.h"
#include "signet/revocation.h"
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
  char tmpl[] = "/tmp/signet-test-selfsvc-XXXXXX.db";
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
  SignetReplayCache *replay_self;
  SignetDenyList *deny;
  char *db_path;
  char bunker_sk_hex[65];
  char bunker_pk_hex[65];
  char prov_sk_hex[65];
  char prov_pk_hex[65];
  /* agent "stew" identity */
  char stew_sk_hex[65];
  char stew_pk_hex[65];
  /* agent "other" identity */
  char other_sk_hex[65];
  char other_pk_hex[65];
} Fixture;

static void adopt_agent(Fixture *f, const char *agent_id,
                        const char sk_hex[65], const char pk_hex[65],
                        const char *fixed_secret) {
  uint8_t sk_raw[32];
  assert(hex_to_bytes(sk_hex, sk_raw, 32) == 0);
  char out_pk[65] = {0};
  assert(signet_key_store_adopt_agent(f->ks, agent_id, sk_raw, pk_hex,
                                      fixed_secret, f->bunker_pk_hex,
                                      RELAYS, 1, out_pk, NULL) == SIGNET_ADOPT_OK);
  sodium_memzero(sk_raw, sizeof(sk_raw));
}

static void fixture_setup(Fixture *f) {
  memset(f, 0, sizeof(*f));

  gen_keypair_hex(f->bunker_sk_hex, f->bunker_pk_hex);
  gen_keypair_hex(f->prov_sk_hex, f->prov_pk_hex);
  gen_keypair_hex(f->stew_sk_hex, f->stew_pk_hex);
  gen_keypair_hex(f->other_sk_hex, f->other_pk_hex);

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
  f->mgmt = signet_mgmt_handler_new(f->ks, NULL, NULL, NULL, &mcfg);
  assert(f->mgmt != NULL);

  SignetReplayCacheConfig rcfg = { .max_entries = 128, .ttl_seconds = 300, .skew_seconds = 300 };
  f->replay = signet_replay_cache_new(&rcfg);
  assert(f->replay != NULL);
  signet_mgmt_handler_set_replay_cache(f->mgmt, f->replay);

  /* Deliberately TINY self-service replay domain so the isolation test can
   * cheaply overflow it. */
  SignetReplayCacheConfig scfg = { .max_entries = 8, .ttl_seconds = 300, .skew_seconds = 300 };
  f->replay_self = signet_replay_cache_new(&scfg);
  assert(f->replay_self != NULL);
  signet_mgmt_handler_set_self_replay_cache(f->mgmt, f->replay_self);

  /* Live deny list, shared with the handler exactly as signetd wires it. */
  f->deny = signet_deny_list_new(signet_key_store_get_store(f->ks));
  assert(f->deny != NULL);
  signet_mgmt_handler_set_deny_list(f->mgmt, f->deny);

  adopt_agent(f, "stew", f->stew_sk_hex, f->stew_pk_hex, "stew-initial");
  adopt_agent(f, "other", f->other_sk_hex, f->other_pk_hex, "other-initial");
}

static void fixture_teardown(Fixture *f) {
  signet_mgmt_handler_free(f->mgmt);
  signet_deny_list_free(f->deny);
  signet_key_store_free(f->ks);
  signet_replay_cache_free(f->replay);
  signet_replay_cache_free(f->replay_self);
  unlink(f->db_path);
  g_free(f->db_path);
  sodium_memzero(f->bunker_sk_hex, sizeof(f->bunker_sk_hex));
  sodium_memzero(f->prov_sk_hex, sizeof(f->prov_sk_hex));
  sodium_memzero(f->stew_sk_hex, sizeof(f->stew_sk_hex));
  sodium_memzero(f->other_sk_hex, sizeof(f->other_sk_hex));
}

/* NIP-44 v2 encrypt `plaintext` from `sender_sk_hex` to the bunker — what the
 * management transport hands to handle_event after gift-wrap unwrapping. */
static char *encrypt_to_bunker(Fixture *f, const char *sender_sk_hex,
                               const char *plaintext) {
  uint8_t sk[32], pk[32];
  assert(hex_to_bytes(sender_sk_hex, sk, 32) == 0);
  assert(hex_to_bytes(f->bunker_pk_hex, pk, 32) == 0);
  char *cipher = NULL;
  int rc = nostr_nip44_encrypt_v2(sk, pk, (const uint8_t *)plaintext,
                                  strlen(plaintext), &cipher);
  sodium_memzero(sk, sizeof(sk));
  assert(rc == 0 && cipher != NULL);
  return cipher;
}

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

/* Dispatch a reissue-connect intent for `agent_id`, signed-sender
 * `sender_pk`, content encrypted from `sender_sk`. */
static int send_reissue(Fixture *f, const char *sender_sk, const char *sender_pk,
                        const char *agent_id, const char *event_id, int64_t now) {
  char *payload = g_strdup_printf("{\"agent_id\":\"%s\",\"request_id\":\"r-%s\"}",
                                  agent_id, event_id);
  char *cipher = encrypt_to_bunker(f, sender_sk, payload);
  g_free(payload);
  int rc = signet_mgmt_handler_handle_request(f->mgmt, sender_pk, cipher,
                                            SIGNET_MGMT_OP_REISSUE_CONNECT,
                                            event_id, now);
  free(cipher);
  return rc;
}

static void test_provisioner_can_reissue_any(void) {
  Fixture f;
  fixture_setup(&f);
  int64_t now = 1752380000;

  assert(send_reissue(&f, f.prov_sk_hex, f.prov_pk_hex, "stew", "evt-p1", now) == 0);
  assert(send_reissue(&f, f.prov_sk_hex, f.prov_pk_hex, "other", "evt-p2", now) == 0);
  char *s = read_connect_secret(&f, "stew");
  assert(s && strcmp(s, "stew-initial") != 0);
  sodium_memzero(s, strlen(s)); g_free(s);

  fixture_teardown(&f);
  printf("test_provisioner_can_reissue_any: PASS\n");
}

static void test_agent_can_reissue_itself(void) {
  Fixture f;
  fixture_setup(&f);
  int64_t now = 1752380000;

  assert(send_reissue(&f, f.stew_sk_hex, f.stew_pk_hex, "stew", "evt-s1", now) == 0);

  /* 6: the fresh secret consumes exactly once; the old one is dead. */
  char *fresh = read_connect_secret(&f, "stew");
  assert(fresh && strcmp(fresh, "stew-initial") != 0);
  char *agent = NULL;
  assert(signet_key_store_consume_connect_secret(f.ks, "stew-initial", now, &agent) == 1);
  assert(signet_key_store_consume_connect_secret(f.ks, fresh, now, &agent) == 0);
  assert(agent && strcmp(agent, "stew") == 0);
  g_free(agent);
  agent = NULL;
  assert(signet_key_store_consume_connect_secret(f.ks, fresh, now, &agent) == 1);
  sodium_memzero(fresh, strlen(fresh)); g_free(fresh);

  fixture_teardown(&f);
  printf("test_agent_can_reissue_itself: PASS\n");
}

static void test_agent_cannot_reissue_other(void) {
  Fixture f;
  fixture_setup(&f);
  int64_t now = 1752380000;

  /* stew (valid agent) targets "other": rejected, other's secret untouched. */
  assert(send_reissue(&f, f.stew_sk_hex, f.stew_pk_hex, "other", "evt-x1", now) == -1);
  char *s = read_connect_secret(&f, "other");
  assert(s && strcmp(s, "other-initial") == 0);
  sodium_memzero(s, strlen(s)); g_free(s);

  fixture_teardown(&f);
  printf("test_agent_cannot_reissue_other: PASS\n");
}

static void test_unknown_sender_cannot_reissue(void) {
  Fixture f;
  fixture_setup(&f);
  int64_t now = 1752380000;

  char rando_sk[65], rando_pk[65];
  gen_keypair_hex(rando_sk, rando_pk);

  /* Unknown sender targeting a real agent: rejected, no mutation. */
  assert(send_reissue(&f, rando_sk, rando_pk, "stew", "evt-u1", now) == -1);
  char *s = read_connect_secret(&f, "stew");
  assert(s && strcmp(s, "stew-initial") == 0);
  sodium_memzero(s, strlen(s)); g_free(s);

  /* Unknown sender targeting an unknown agent: same rejection (no probing). */
  assert(send_reissue(&f, rando_sk, rando_pk, "no-such-agent", "evt-u2", now) == -1);

  /* Unauthorized attempts must NOT mark the replay cache (auth runs before
   * the replay mark), so they cannot churn/evict legitimate entries. Proven
   * by reusing the rejected event id for an authorized request: it executes
   * rather than being reported as a duplicate. */
  assert(send_reissue(&f, f.prov_sk_hex, f.prov_pk_hex, "stew", "evt-u1", now) == 0);

  sodium_memzero(rando_sk, sizeof(rando_sk));
  fixture_teardown(&f);
  printf("test_unknown_sender_cannot_reissue: PASS\n");
}

static void test_self_service_replay_executes_once(void) {
  Fixture f;
  fixture_setup(&f);
  int64_t now = 1752380000;

  assert(send_reissue(&f, f.stew_sk_hex, f.stew_pk_hex, "stew", "evt-r1", now) == 0);
  char *first = read_connect_secret(&f, "stew");
  assert(first != NULL);

  /* Identical redelivery (same event id) is rejected; secret is stable. */
  assert(send_reissue(&f, f.stew_sk_hex, f.stew_pk_hex, "stew", "evt-r1", now) == -1);
  char *after = read_connect_secret(&f, "stew");
  assert(after && strcmp(first, after) == 0);

  sodium_memzero(first, strlen(first)); g_free(first);
  sodium_memzero(after, strlen(after)); g_free(after);
  fixture_teardown(&f);
  printf("test_self_service_replay_executes_once: PASS\n");
}

static void test_self_service_does_not_leak_to_other_methods(void) {
  Fixture f;
  fixture_setup(&f);
  int64_t now = 1752380000;

  /* An agent sending get-status (empty content, would succeed for a
   * provisioner) must be silently dropped: the self-service exception is
   * scoped to agent/reissue-connect only. */
  assert(signet_mgmt_handler_handle_request(f.mgmt, f.stew_pk_hex, "",
                                          SIGNET_MGMT_OP_GET_STATUS,
                                          "evt-m1", now) == -1);
  /* Same for a mutating method. */
  char *cipher = encrypt_to_bunker(&f, f.stew_sk_hex,
                                   "{\"agent_id\":\"stew\",\"request_id\":\"rk\"}");
  assert(signet_mgmt_handler_handle_request(f.mgmt, f.stew_pk_hex, cipher,
                                          SIGNET_MGMT_OP_ROTATE_KEY,
                                          "evt-m2", now) == -1);
  free(cipher);

  fixture_teardown(&f);
  printf("test_self_service_does_not_leak_to_other_methods: PASS\n");
}

/* Replay-domain isolation: an agent flooding unique self-reissue intents
 * must not evict provisioner event ids from the privileged replay cache
 * (which would allow re-executing a captured provisioner mutation). */
static void test_self_flood_cannot_evict_provisioner_entries(void) {
  Fixture f;
  fixture_setup(&f);
  int64_t now = 1752380000;

  /* A provisioner mutation is executed and its event id marked. */
  assert(send_reissue(&f, f.prov_sk_hex, f.prov_pk_hex, "stew", "evt-priv", now) == 0);

  /* Flood: 24 unique self-service events — 3x the self cache capacity (8),
   * enough to have fully cycled a shared cache of that size. */
  for (int i = 0; i < 24; i++) {
    char evt[32];
    snprintf(evt, sizeof(evt), "evt-flood-%d", i);
    assert(send_reissue(&f, f.stew_sk_hex, f.stew_pk_hex, "stew", evt, now) == 0);
  }

  /* The captured provisioner event still cannot be replayed. */
  assert(send_reissue(&f, f.prov_sk_hex, f.prov_pk_hex, "stew", "evt-priv", now) == -1);

  fixture_teardown(&f);
  printf("test_self_flood_cannot_evict_provisioner_entries: PASS\n");
}

/* Suspension: a deny-listed (but not revoked) agent must not obtain a fresh
 * connect_secret via self-service OR via a provisioner, until the deny entry
 * is lifted. Suspension of one agent must not affect others. */
static void test_suspended_agent_cannot_bypass(void) {
  Fixture f;
  fixture_setup(&f);
  int64_t now = 1752380000;

  /* Suspend stew: deny-list its pubkey WITHOUT revoking/wiping the key. */
  assert(signet_deny_list_add(f.deny, f.stew_pk_hex, "stew", "suspended", now) == 0);

  /* Self-service reissue is refused; the stored secret is untouched. */
  assert(send_reissue(&f, f.stew_sk_hex, f.stew_pk_hex, "stew", "evt-d1", now) == -1);
  char *s = read_connect_secret(&f, "stew");
  assert(s && strcmp(s, "stew-initial") == 0);
  sodium_memzero(s, strlen(s)); g_free(s);

  /* Even a provisioner cannot mint for a suspended agent. */
  assert(send_reissue(&f, f.prov_sk_hex, f.prov_pk_hex, "stew", "evt-d2", now) == -1);
  s = read_connect_secret(&f, "stew");
  assert(s && strcmp(s, "stew-initial") == 0);
  sodium_memzero(s, strlen(s)); g_free(s);

  /* Suspension is scoped: the other agent still self-services fine. */
  assert(send_reissue(&f, f.other_sk_hex, f.other_pk_hex, "other", "evt-d3", now) == 0);

  /* Lifting the suspension restores self-service. */
  assert(signet_deny_list_remove(f.deny, f.stew_pk_hex) == 0);
  assert(send_reissue(&f, f.stew_sk_hex, f.stew_pk_hex, "stew", "evt-d4", now) == 0);
  s = read_connect_secret(&f, "stew");
  assert(s && strcmp(s, "stew-initial") != 0);
  sodium_memzero(s, strlen(s)); g_free(s);

  fixture_teardown(&f);
  printf("test_suspended_agent_cannot_bypass: PASS\n");
}

/* Full revocation: the key is wiped, so the revoked agent can no longer
 * self-authorize at all, and a provisioner gets not_found. */
static void test_revoked_agent_cannot_reissue(void) {
  Fixture f;
  fixture_setup(&f);
  int64_t now = 1752380000;

  SignetStore *st = signet_key_store_get_store(f.ks);
  assert(st != NULL);
  assert(signet_revoke_agent(st, f.ks, f.deny, NULL, "stew", f.stew_pk_hex,
                             "test revoke", now) == 0);

  /* Self-service with the (formerly valid) agent key is refused. */
  assert(send_reissue(&f, f.stew_sk_hex, f.stew_pk_hex, "stew", "evt-v1", now) == -1);
  /* Provisioner reissue for the revoked agent fails too (row is gone). */
  assert(send_reissue(&f, f.prov_sk_hex, f.prov_pk_hex, "stew", "evt-v2", now) == -1);

  fixture_teardown(&f);
  printf("test_revoked_agent_cannot_reissue: PASS\n");
}

int main(void) {
  if (sodium_init() < 0) {
    fprintf(stderr, "sodium_init failed\n");
    return 1;
  }

  test_provisioner_can_reissue_any();
  test_agent_can_reissue_itself();
  test_agent_cannot_reissue_other();
  test_unknown_sender_cannot_reissue();
  test_self_service_replay_executes_once();
  test_self_service_does_not_leak_to_other_methods();
  test_self_flood_cannot_evict_provisioner_entries();
  test_suspended_agent_cannot_bypass();
  test_revoked_agent_cannot_reissue();

  printf("All reissue self-service tests passed.\n");
  return 0;
}
