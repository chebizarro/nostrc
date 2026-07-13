/* SPDX-License-Identifier: MIT
 *
 * test_client_bindings.c - Proves persistent NIP-46 client bindings
 * (nostrc-xhb: pair once, reconnect without a fresh connect_secret):
 *
 * Store layer:
 *   1. bind/lookup/revoke/re-bind lifecycle, case canonicalization,
 *      malformed pubkeys, list with revoked rows, revoke-all-for-agent;
 *
 * NIP-46 flow (signet_nip46_server_handle_event):
 *   2. first connect with a valid one-time secret pairs the client:
 *      secret consumed AND binding persisted;
 *   3. daemon restart (new server instance, same store): a bound client's
 *      request resolves via the persistent binding with NO connect;
 *   4. reconnect with the STALE consumed secret succeeds via the binding
 *      (exactly what a restarted plugin sends) — no plugin changes needed;
 *   5. reconnect with NO secret succeeds via the binding;
 *   6. an unbound client with no/stale secret is rejected;
 *   7. a revoked binding rejects both requests and secretless reconnects,
 *      and a fresh secret (reissue) re-pairs, clearing the revocation;
 *   8. full agent revocation revokes its client bindings.
 */

#include "signet/key_store.h"
#include "signet/nip46_server.h"
#include "signet/policy_engine.h"
#include "signet/policy_store.h"
#include "signet/revocation.h"
#include "signet/store.h"
#include "signet/audit_logger.h"
#include "signet/health_server.h" /* g_signet_metrics */

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

static char *make_temp_path(const char *tmpl_in) {
  char tmpl[128];
  g_strlcpy(tmpl, tmpl_in, sizeof(tmpl));
  int fd = mkstemps(tmpl, (int)strlen(strrchr(tmpl_in, '.')));
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

/* ------------------------- store-level lifecycle ------------------------- */

static void test_store_binding_lifecycle(void) {
  char *db_path = make_temp_path("/tmp/signet-test-bindings-XXXXXX.db");
  SignetStoreConfig scfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetStore *st = signet_store_open(&scfg);
  assert(st != NULL);

  /* Bindings resolve only against a live agent whose CURRENT identity
   * matches the pinned pubkey — create the agent row first. */
  char a_sk[65], a_pk[65];
  gen_keypair_hex(a_sk, a_pk);
  uint8_t a_raw[32];
  assert(hex_to_bytes(a_sk, a_raw, 32) == 0);
  assert(signet_store_put_agent_ex(st, "stew", a_raw, 32, NULL, a_pk,
                                   "adopted", 900) == 0);

  char c1_sk[65], c1_pk[65], c2_sk[65], c2_pk[65];
  gen_keypair_hex(c1_sk, c1_pk);
  gen_keypair_hex(c2_sk, c2_pk);

  /* Bind + lookup; the pairing-secret hash is retrievable. */
  assert(signet_store_bind_client(st, "stew", a_pk, c1_pk, "s1", 1000) == 0);
  char *agent = NULL;
  char *hash = NULL;
  assert(signet_store_lookup_client_binding(st, c1_pk, 1001, &agent, &hash) == 0);
  assert(agent && strcmp(agent, "stew") == 0);
  {
    char *expect = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "s1", -1);
    assert(hash && strcmp(hash, expect) == 0);
    g_free(expect);
  }
  g_free(agent);
  g_free(hash);
  agent = NULL;
  hash = NULL;

  /* Uppercase input canonicalizes: bind uppercase, lookup lowercase. */
  char upper[65];
  for (int i = 0; i < 65; i++) upper[i] = (char)g_ascii_toupper(c2_pk[i]);
  assert(signet_store_bind_client(st, "stew", a_pk, upper, NULL, 1002) == 0);
  assert(signet_store_lookup_client_binding(st, c2_pk, 1003, &agent, NULL) == 0);
  assert(agent && strcmp(agent, "stew") == 0);
  g_free(agent);
  agent = NULL;

  /* Unknown / malformed pubkeys. */
  assert(signet_store_lookup_client_binding(st,
      "1111111111111111111111111111111111111111111111111111111111111111",
      1004, &agent, NULL) == 1);
  assert(signet_store_lookup_client_binding(st, "nope", 1005, &agent, NULL) == 1);
  assert(signet_store_bind_client(st, "stew", a_pk, "nope", NULL, 1006) == -1);

  /* Revoke: lookup misses; second revoke reports already-revoked. */
  assert(signet_store_revoke_client(st, c1_pk, 2000) == 0);
  assert(signet_store_lookup_client_binding(st, c1_pk, 2001, &agent, NULL) == 1);
  assert(signet_store_revoke_client(st, c1_pk, 2002) == 1);

  /* List shows both rows, revoked one flagged. */
  SignetClientBinding *list = NULL;
  size_t count = 0;
  assert(signet_store_list_clients(st, "stew", &list, &count) == 0);
  assert(count == 2);
  size_t revoked_seen = 0, active_seen = 0;
  for (size_t i = 0; i < count; i++) {
    if (list[i].revoked_at != 0) revoked_seen++;
    else active_seen++;
  }
  assert(revoked_seen == 1 && active_seen == 1);
  signet_client_binding_list_free(list, count);

  /* Re-bind clears revocation. */
  assert(signet_store_bind_client(st, "stew", a_pk, c1_pk, NULL, 3000) == 0);
  assert(signet_store_lookup_client_binding(st, c1_pk, 3001, &agent, NULL) == 0);
  assert(agent && strcmp(agent, "stew") == 0);
  g_free(agent);
  agent = NULL;

  /* Identity pin: replace the agent's key (rotate/reprovision) — the pinned
   * binding no longer resolves toward the NEW identity. */
  char b_sk[65], b_pk[65];
  gen_keypair_hex(b_sk, b_pk);
  uint8_t b_raw[32];
  assert(hex_to_bytes(b_sk, b_raw, 32) == 0);
  assert(signet_store_put_agent_ex(st, "stew", b_raw, 32, NULL, b_pk,
                                   "rotated", 3500) == 0);
  assert(signet_store_lookup_client_binding(st, c1_pk, 3501, &agent, NULL) == 1);
  /* Restore identity A for the remaining assertions. */
  assert(signet_store_put_agent_ex(st, "stew", a_raw, 32, NULL, a_pk,
                                   "adopted", 3600) == 0);
  assert(signet_store_lookup_client_binding(st, c1_pk, 3601, &agent, NULL) == 0);
  g_free(agent);
  agent = NULL;

  /* Revoke-all for the agent. */
  assert(signet_store_revoke_agent_clients(st, "stew", 4000) == 2);
  assert(signet_store_lookup_client_binding(st, c1_pk, 4001, &agent, NULL) == 1);
  assert(signet_store_lookup_client_binding(st, c2_pk, 4002, &agent, NULL) == 1);
  assert(signet_store_revoke_agent_clients(st, "stew", 4003) == 0);

  sodium_memzero(a_raw, sizeof(a_raw));
  sodium_memzero(b_raw, sizeof(b_raw));
  signet_store_close(st);
  unlink(db_path);
  g_free(db_path);
  printf("test_store_binding_lifecycle: PASS\n");
}

/* --------------------------- NIP-46 flow tests ---------------------------- */

typedef struct {
  SignetKeyStore *ks;
  SignetPolicyStore *ps;
  SignetPolicyEngine *pe;
  SignetAuditLogger *audit;
  SignetNip46Server *srv;
  char *db_path;
  char *policy_path;
  char bunker_sk_hex[65];
  char bunker_pk_hex[65];
  char stew_sk_hex[65];
  char stew_pk_hex[65];
  char client_sk_hex[65];
  char client_pk_hex[65];
} N46Fixture;

static void n46_new_server(N46Fixture *f) {
  SignetNip46ServerConfig ncfg = { .identity = "bunker", .fido = NULL };
  /* No relay pool (response publish is a no-op) and no replay cache (replay
   * semantics are covered by the NIP-46 server's own tests). */
  f->srv = signet_nip46_server_new(NULL, f->pe, f->ks, NULL, f->audit, &ncfg);
  assert(f->srv != NULL);
}

static void n46_setup(N46Fixture *f) {
  memset(f, 0, sizeof(*f));

  gen_keypair_hex(f->bunker_sk_hex, f->bunker_pk_hex);
  gen_keypair_hex(f->stew_sk_hex, f->stew_pk_hex);
  gen_keypair_hex(f->client_sk_hex, f->client_pk_hex);

  f->db_path = make_temp_path("/tmp/signet-test-n46bind-XXXXXX.db");

  /* Permissive policy for the test agent. */
  f->policy_path = make_temp_path("/tmp/signet-test-n46pol-XXXXXX.toml");
  {
    const char *policy =
        "[identity.stew]\n"
        "allow_clients = \"*\"\n"
        "allow_methods = \"*\"\n"
        "allow_kinds = \"*\"\n"
        "default = \"allow\"\n";
    assert(g_file_set_contents(f->policy_path, policy, -1, NULL));
  }

  SignetAuditLoggerConfig alc = { .path = NULL, .to_stdout = false, .flush_each_write = false };
  f->audit = signet_audit_logger_new(&alc);

  SignetKeyStoreConfig kcfg = { .db_path = f->db_path, .master_key = MASTER_KEY };
  f->ks = signet_key_store_new(f->audit, &kcfg);
  assert(f->ks != NULL);

  f->ps = signet_policy_store_file_new(f->policy_path);
  assert(f->ps != NULL);
  SignetPolicyEngineConfig pcfg = { .default_decision = SIGNET_POLICY_DECISION_DENY };
  f->pe = signet_policy_engine_new(f->ps, f->audit, &pcfg);
  assert(f->pe != NULL);

  /* Adopt the agent with a known one-time connect secret. */
  uint8_t sk_raw[32];
  assert(hex_to_bytes(f->stew_sk_hex, sk_raw, 32) == 0);
  char out_pk[65] = {0};
  assert(signet_key_store_adopt_agent(f->ks, "stew", sk_raw, f->stew_pk_hex,
                                      "one-time-secret", f->bunker_pk_hex,
                                      NULL, 0, out_pk, NULL) == SIGNET_ADOPT_OK);
  sodium_memzero(sk_raw, sizeof(sk_raw));

  n46_new_server(f);
}

static void n46_teardown(N46Fixture *f) {
  signet_nip46_server_free(f->srv);
  signet_policy_engine_free(f->pe);
  signet_policy_store_free(f->ps);
  signet_key_store_free(f->ks);
  unlink(f->db_path);
  g_free(f->db_path);
  unlink(f->policy_path);
  g_free(f->policy_path);
  sodium_memzero(f->bunker_sk_hex, sizeof(f->bunker_sk_hex));
  sodium_memzero(f->stew_sk_hex, sizeof(f->stew_sk_hex));
  sodium_memzero(f->client_sk_hex, sizeof(f->client_sk_hex));
}

/* Send one NIP-46 request from `client_sk/pk` and report whether it was
 * ALLOWED, judged by the auth_ok metrics counter delta (the server publishes
 * responses via relays, which are absent here). */
static bool n46_send(N46Fixture *f, const char *client_sk, const char *client_pk,
                     const char *request_json, const char *event_id, int64_t now) {
  uint8_t sk[32], pk[32];
  assert(hex_to_bytes(client_sk, sk, 32) == 0);
  assert(hex_to_bytes(f->bunker_pk_hex, pk, 32) == 0);
  char *cipher = NULL;
  assert(nostr_nip44_encrypt_v2(sk, pk, (const uint8_t *)request_json,
                                strlen(request_json), &cipher) == 0 && cipher);
  sodium_memzero(sk, sizeof(sk));

  gint ok_before = g_atomic_int_get(&g_signet_metrics.auth_ok);
  (void)signet_nip46_server_handle_event(f->srv, f->bunker_pk_hex, f->bunker_sk_hex,
                                         client_pk, cipher, now, event_id, now);
  free(cipher);
  return g_atomic_int_get(&g_signet_metrics.auth_ok) > ok_before;
}

static bool n46_connect(N46Fixture *f, const char *client_sk, const char *client_pk,
                        const char *secret_or_null, const char *event_id, int64_t now) {
  char *req;
  if (secret_or_null && secret_or_null[0]) {
    req = g_strdup_printf("{\"id\":\"c\",\"method\":\"connect\",\"params\":[\"%s\",\"%s\"]}",
                          f->bunker_pk_hex, secret_or_null);
  } else {
    req = g_strdup_printf("{\"id\":\"c\",\"method\":\"connect\",\"params\":[\"%s\"]}",
                          f->bunker_pk_hex);
  }
  bool allowed = n46_send(f, client_sk, client_pk, req, event_id, now);
  g_free(req);
  return allowed;
}

static bool n46_get_public_key(N46Fixture *f, const char *client_sk,
                               const char *client_pk, const char *event_id,
                               int64_t now) {
  const char *req = "{\"id\":\"g\",\"method\":\"get_public_key\",\"params\":[]}";
  return n46_send(f, client_sk, client_pk, req, event_id, now);
}

static char *n46_read_binding(N46Fixture *f, const char *client_pk, int64_t now) {
  SignetStore *st = signet_key_store_get_store(f->ks);
  assert(st != NULL);
  char *agent = NULL;
  int rc = signet_store_lookup_client_binding(st, client_pk, now, &agent, NULL);
  if (rc != 0) return NULL;
  return agent;
}

/* 2 + 3 + 4 + 5: pairing persists; restarts and stale secrets reconnect. */
static void test_pair_once_reconnect_freely(void) {
  N46Fixture f;
  n46_setup(&f);
  int64_t now = 1752380000;

  /* First connect: valid secret → allowed, secret consumed, binding stored. */
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);
  char *bound = n46_read_binding(&f, f.client_pk_hex, now);
  assert(bound && strcmp(bound, "stew") == 0);
  g_free(bound);
  {
    char *agent = NULL;
    assert(signet_key_store_consume_connect_secret(f.ks, "one-time-secret", now, &agent) == 1);
  }

  /* Daemon restart: fresh server object, empty RAM sessions, same store.
   * A request (no connect first!) resolves via the persistent binding. */
  signet_nip46_server_free(f.srv);
  n46_new_server(&f);
  assert(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == true);

  /* Restarted plugin behavior: connect with the STALE consumed secret —
   * accepted because it hashes to the client's OWN recorded pairing secret. */
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e3", now) == true);

  /* Secretless reconnect also works. */
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e4", now) == true);

  /* An ARBITRARY wrong secret from the bound client is rejected — stale
   * fallback only honors the client's own former secret, so probing and
   * misconfiguration surface as auth_failed instead of being masked. */
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "totally-wrong", "e5", now) == false);

  n46_teardown(&f);
  printf("test_pair_once_reconnect_freely: PASS\n");
}

/* 6: unbound clients cannot connect without a valid secret or do requests. */
static void test_unbound_client_rejected(void) {
  N46Fixture f;
  n46_setup(&f);
  int64_t now = 1752380000;

  char rando_sk[65], rando_pk[65];
  gen_keypair_hex(rando_sk, rando_pk);

  assert(n46_connect(&f, rando_sk, rando_pk, NULL, "e1", now) == false);
  assert(n46_connect(&f, rando_sk, rando_pk, "wrong-secret", "e2", now) == false);
  assert(n46_get_public_key(&f, rando_sk, rando_pk, "e3", now) == false);
  assert(n46_read_binding(&f, rando_pk, now) == NULL);

  sodium_memzero(rando_sk, sizeof(rando_sk));
  n46_teardown(&f);
  printf("test_unbound_client_rejected: PASS\n");
}

/* 7: revoked binding is dead immediately; a fresh secret re-pairs. */
static void test_revoked_binding_and_repair(void) {
  N46Fixture f;
  n46_setup(&f);
  int64_t now = 1752380000;

  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);

  /* Revoke the binding (as agent/revoke-client would). */
  SignetStore *st = signet_key_store_get_store(f.ks);
  assert(signet_store_revoke_client(st, f.client_pk_hex, now) == 0);

  /* Requests and secretless/stale reconnects are rejected at once — the
   * persistent table is authoritative, RAM session notwithstanding. */
  assert(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == false);
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e3", now) == false);
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e4", now) == false);

  /* Re-pair with a freshly reissued one-time secret. */
  char re_pk[65] = {0};
  char *fresh = NULL;
  assert(signet_key_store_reissue_connect_secret(f.ks, "stew", NULL, NULL, NULL, 0,
                                                 re_pk, &fresh, NULL) == 0);
  assert(fresh != NULL);
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, fresh, "e5", now) == true);
  char *bound = n46_read_binding(&f, f.client_pk_hex, now);
  assert(bound && strcmp(bound, "stew") == 0);
  g_free(bound);
  assert(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e6", now) == true);

  sodium_memzero(fresh, strlen(fresh));
  g_free(fresh);
  n46_teardown(&f);
  printf("test_revoked_binding_and_repair: PASS\n");
}

/* 8: full agent revocation revokes its client bindings. */
static void test_agent_revocation_revokes_bindings(void) {
  N46Fixture f;
  n46_setup(&f);
  int64_t now = 1752380000;

  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);

  SignetStore *st = signet_key_store_get_store(f.ks);
  assert(signet_revoke_agent(st, f.ks, NULL, NULL, "stew", f.stew_pk_hex,
                             "test revoke", now) == 0);

  assert(n46_read_binding(&f, f.client_pk_hex, now) == NULL);
  assert(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == false);
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e3", now) == false);

  n46_teardown(&f);
  printf("test_agent_revocation_revokes_bindings: PASS\n");
}

/* Suspension precedence: a deny-listed (not revoked) agent's bound client is
 * refused on requests AND reconnects; lifting the entry restores service. */
static void test_suspended_agent_binding_refused(void) {
  N46Fixture f;
  n46_setup(&f);
  int64_t now = 1752380000;

  SignetStore *st = signet_key_store_get_store(f.ks);
  SignetDenyList *deny = signet_deny_list_new(st);
  assert(deny != NULL);
  signet_nip46_server_set_deny_list(f.srv, deny);

  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);
  assert(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == true);

  /* Suspend. */
  assert(signet_deny_list_add(deny, f.stew_pk_hex, "stew", "suspended", now) == 0);
  assert(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e3", now) == false);
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e4", now) == false);
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e5", now) == false);

  /* Lift. */
  assert(signet_deny_list_remove(deny, f.stew_pk_hex) == 0);
  assert(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e6", now) == true);

  signet_nip46_server_set_deny_list(f.srv, NULL);
  signet_deny_list_free(deny);
  n46_teardown(&f);
  printf("test_suspended_agent_binding_refused: PASS\n");
}

/* Identity pin: rotating the agent's key invalidates existing bindings — a
 * client bound to the OLD identity gets nothing toward the new one. */
static void test_rotation_invalidates_binding(void) {
  N46Fixture f;
  n46_setup(&f);
  int64_t now = 1752380000;

  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);
  assert(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == true);

  char new_pk[65] = {0};
  assert(signet_key_store_rotate_agent(f.ks, "stew", new_pk, sizeof(new_pk)) == 0);

  assert(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e3", now) == false);
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e4", now) == false);

  n46_teardown(&f);
  printf("test_rotation_invalidates_binding: PASS\n");
}

/* Resurrection guard: revoke the agent, then reprovision the SAME agent_id
 * under a new key — the old client's binding must convey nothing. */
static void test_reprovision_does_not_resurrect_binding(void) {
  N46Fixture f;
  n46_setup(&f);
  int64_t now = 1752380000;

  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);

  SignetStore *st = signet_key_store_get_store(f.ks);
  assert(signet_revoke_agent(st, f.ks, NULL, NULL, "stew", f.stew_pk_hex,
                             "test revoke", now) == 0);

  /* Reprovision the same agent_id with a brand-new identity. */
  char sk2[65], pk2[65];
  gen_keypair_hex(sk2, pk2);
  uint8_t sk2_raw[32];
  assert(hex_to_bytes(sk2, sk2_raw, 32) == 0);
  char out_pk[65] = {0};
  assert(signet_key_store_adopt_agent(f.ks, "stew", sk2_raw, pk2,
                                      "new-secret", f.bunker_pk_hex,
                                      NULL, 0, out_pk, NULL) == SIGNET_ADOPT_OK);
  sodium_memzero(sk2_raw, sizeof(sk2_raw));
  sodium_memzero(sk2, sizeof(sk2));

  /* The old client's binding is both revoked AND pinned to the dead
   * identity — requests and secretless reconnects fail. */
  assert(n46_read_binding(&f, f.client_pk_hex, now) == NULL);
  assert(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == false);
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e3", now) == false);
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e4", now) == false);

  /* The client CAN re-pair with the new identity's fresh secret. */
  assert(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "new-secret", "e5", now) == true);
  assert(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e6", now) == true);

  n46_teardown(&f);
  printf("test_reprovision_does_not_resurrect_binding: PASS\n");
}

int main(void) {
  if (sodium_init() < 0) {
    fprintf(stderr, "sodium_init failed\n");
    return 1;
  }

  test_store_binding_lifecycle();
  test_pair_once_reconnect_freely();
  test_unbound_client_rejected();
  test_revoked_binding_and_repair();
  test_agent_revocation_revokes_bindings();
  test_suspended_agent_binding_refused();
  test_rotation_invalidates_binding();
  test_reprovision_does_not_resurrect_binding();

  printf("All client binding tests passed.\n");
  return 0;
}
