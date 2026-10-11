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
 *
 * Cache-only mode (no persistent store, nostrc-xjznk) pairs the same way for
 * the life of the process:
 *   9. pairing, then reconnect with the client's own spent secret or none;
 *  10. a secret spent by (or pending for) another client never authorizes
 *      this one, bound or not; an unknown client is refused (both modes);
 *  11. revoke, rotate-key and revoke + re-adopt invalidate the binding;
 *  12. a restarted cache-only signer holds no bindings, so the client must
 *      pair again with a fresh secret;
 *  13. after rotate-key or restore, reissue-connect mints a fresh secret
 *      that re-pairs (the earlier one dies), and revoke/evict/rotate/restore
 *      drop the agent's bindings at once.
 */

#include "signet/key_store.h"
#include "key_store_private.h"
#include "nip46_server_test_hooks.h"
#include "signet/nip46_server.h"
#include "signet/policy_engine.h"
#include "signet/policy_store.h"
#include "signet/revocation.h"
#include "signet/store.h"
#include "signet/audit_logger.h"
#include "signet/health_server.h" /* g_signet_metrics */

#include <nostr-keys.h>
#include <nostr/nip44/nip44.h>

#include "test_check.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <glib.h>
#include <sodium.h>
#include <sqlite3.h>

#define MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static char *make_temp_path(const char *tmpl_in) {
  char tmpl[128];
  g_strlcpy(tmpl, tmpl_in, sizeof(tmpl));
  int fd = mkstemps(tmpl, (int)strlen(strrchr(tmpl_in, '.')));
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

/* ------------------------- store-level lifecycle ------------------------- */

static void test_store_binding_lifecycle(void) {
  char *db_path = make_temp_path("/tmp/signet-test-bindings-XXXXXX.db");
  SignetStoreConfig scfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetStore *st = signet_store_open(&scfg);
  CHECK(st != NULL);

  /* Bindings resolve only against a live agent whose CURRENT identity
   * matches the pinned pubkey — create the agent row first. */
  char a_sk[65], a_pk[65];
  gen_keypair_hex(a_sk, a_pk);
  uint8_t a_raw[32];
  CHECK(hex_to_bytes(a_sk, a_raw, 32) == 0);
  CHECK(signet_store_put_agent_ex(st, "stew", a_raw, 32, NULL, a_pk,
                                   "adopted", 900) == 0);

  char c1_sk[65], c1_pk[65], c2_sk[65], c2_pk[65];
  gen_keypair_hex(c1_sk, c1_pk);
  gen_keypair_hex(c2_sk, c2_pk);

  /* Bind + lookup; the pairing-secret hash is retrievable. */
  CHECK(signet_store_bind_client(st, "stew", a_pk, c1_pk, "s1", 1000) == 0);
  char *agent = NULL;
  char *hash = NULL;
  CHECK(signet_store_lookup_client_binding(st, c1_pk, 1001, &agent, &hash) == 0);
  CHECK(agent && strcmp(agent, "stew") == 0);
  {
    char *expect = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "s1", -1);
    CHECK(hash && strcmp(hash, expect) == 0);
    g_free(expect);
  }
  g_free(agent);
  g_free(hash);
  agent = NULL;
  hash = NULL;

  /* Uppercase input canonicalizes: bind uppercase, lookup lowercase. */
  char upper[65];
  for (int i = 0; i < 65; i++) upper[i] = (char)g_ascii_toupper(c2_pk[i]);
  CHECK(signet_store_bind_client(st, "stew", a_pk, upper, NULL, 1002) == 0);
  CHECK(signet_store_lookup_client_binding(st, c2_pk, 1003, &agent, NULL) == 0);
  CHECK(agent && strcmp(agent, "stew") == 0);
  g_free(agent);
  agent = NULL;

  /* Unknown / malformed pubkeys. */
  CHECK(signet_store_lookup_client_binding(st,
      "1111111111111111111111111111111111111111111111111111111111111111",
      1004, &agent, NULL) == 1);
  CHECK(signet_store_lookup_client_binding(st, "nope", 1005, &agent, NULL) == 1);
  CHECK(signet_store_bind_client(st, "stew", a_pk, "nope", NULL, 1006) == -1);

  /* Revoke: lookup misses; second revoke reports already-revoked. */
  CHECK(signet_store_revoke_client(st, c1_pk, 2000) == 0);
  CHECK(signet_store_lookup_client_binding(st, c1_pk, 2001, &agent, NULL) == 1);
  CHECK(signet_store_revoke_client(st, c1_pk, 2002) == 1);

  /* List shows both rows, revoked one flagged. */
  SignetClientBinding *list = NULL;
  size_t count = 0;
  CHECK(signet_store_list_clients(st, "stew", &list, &count) == 0);
  CHECK(count == 2);
  size_t revoked_seen = 0, active_seen = 0;
  for (size_t i = 0; i < count; i++) {
    if (list[i].revoked_at != 0) revoked_seen++;
    else active_seen++;
  }
  CHECK(revoked_seen == 1 && active_seen == 1);
  signet_client_binding_list_free(list, count);

  /* Re-bind clears revocation. */
  CHECK(signet_store_bind_client(st, "stew", a_pk, c1_pk, NULL, 3000) == 0);
  CHECK(signet_store_lookup_client_binding(st, c1_pk, 3001, &agent, NULL) == 0);
  CHECK(agent && strcmp(agent, "stew") == 0);
  g_free(agent);
  agent = NULL;

  /* Identity pin: replace the agent's key (rotate/reprovision) — the pinned
   * binding no longer resolves toward the NEW identity. */
  char b_sk[65], b_pk[65];
  gen_keypair_hex(b_sk, b_pk);
  uint8_t b_raw[32];
  CHECK(hex_to_bytes(b_sk, b_raw, 32) == 0);
  CHECK(signet_store_put_agent_ex(st, "stew", b_raw, 32, NULL, b_pk,
                                   "rotated", 3500) == 0);
  CHECK(signet_store_lookup_client_binding(st, c1_pk, 3501, &agent, NULL) == 1);
  /* Restore identity A for the remaining assertions. */
  CHECK(signet_store_put_agent_ex(st, "stew", a_raw, 32, NULL, a_pk,
                                   "adopted", 3600) == 0);
  CHECK(signet_store_lookup_client_binding(st, c1_pk, 3601, &agent, NULL) == 0);
  g_free(agent);
  agent = NULL;

  /* Revoke-all for the agent. */
  CHECK(signet_store_revoke_agent_clients(st, "stew", 4000) == 2);
  CHECK(signet_store_lookup_client_binding(st, c1_pk, 4001, &agent, NULL) == 1);
  CHECK(signet_store_lookup_client_binding(st, c2_pk, 4002, &agent, NULL) == 1);
  CHECK(signet_store_revoke_agent_clients(st, "stew", 4003) == 0);

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
  char *audit_path;
  char bunker_sk_hex[65];
  char bunker_pk_hex[65];
  char stew_sk_hex[65];
  char stew_pk_hex[65];
  char client_sk_hex[65];
  char client_pk_hex[65];
  bool cache_only;
} N46Fixture;

static void n46_new_server(N46Fixture *f) {
  SignetNip46ServerConfig ncfg = { .identity = "bunker", .fido = NULL };
  /* No relay pool (response publish is a no-op) and no replay cache (replay
   * semantics are covered by the NIP-46 server's own tests). */
  f->srv = signet_nip46_server_new(NULL, f->pe, f->ks, NULL, f->audit, &ncfg);
  CHECK(f->srv != NULL);
}

/* Adopt `agent_id` under `sk_hex` with a known one-time connect secret. */
static void n46_adopt(N46Fixture *f, const char *agent_id, const char *sk_hex,
                      const char *pk_hex, const char *secret) {
  uint8_t sk_raw[32];
  CHECK(hex_to_bytes(sk_hex, sk_raw, 32) == 0);
  char out_pk[65] = {0};
  CHECK(signet_key_store_adopt_agent(f->ks, agent_id, sk_raw, pk_hex, secret,
                                      f->bunker_pk_hex, NULL, 0, out_pk,
                                      NULL) == SIGNET_ADOPT_OK);
  sodium_memzero(sk_raw, sizeof(sk_raw));
}

/* Opens the key store: SQLCipher-backed, or cache-only (no db_path). */
static void n46_open_key_store(N46Fixture *f) {
  SignetKeyStoreConfig kcfg = { .db_path = f->db_path, .master_key = MASTER_KEY };
  f->ks = signet_key_store_new(f->audit, f->cache_only ? NULL : &kcfg);
  CHECK(f->ks != NULL);
  CHECK((signet_key_store_get_store(f->ks) == NULL) == f->cache_only);
}

static void n46_setup_mode(N46Fixture *f, bool cache_only) {
  memset(f, 0, sizeof(*f));
  f->cache_only = cache_only;

  gen_keypair_hex(f->bunker_sk_hex, f->bunker_pk_hex);
  gen_keypair_hex(f->stew_sk_hex, f->stew_pk_hex);
  gen_keypair_hex(f->client_sk_hex, f->client_pk_hex);

  if (!cache_only) f->db_path = make_temp_path("/tmp/signet-test-n46bind-XXXXXX.db");

  /* Permissive policy for the test agent. */
  f->policy_path = make_temp_path("/tmp/signet-test-n46pol-XXXXXX.toml");
  {
    const char *policy =
        "[identity.stew]\n"
        "allow_clients = \"*\"\n"
        "allow_methods = \"*\"\n"
        "allow_kinds = \"*\"\n"
        "default = \"allow\"\n"
        "[identity.ops]\n"
        "allow_clients = \"*\"\n"
        "allow_methods = \"*\"\n"
        "allow_kinds = \"*\"\n"
        "default = \"allow\"\n";
    CHECK(g_file_set_contents(f->policy_path, policy, -1, NULL));
  }

  f->audit_path = make_temp_path("/tmp/signet-test-n46audit-XXXXXX.log");
  SignetAuditLoggerConfig alc = { .path = f->audit_path, .to_stdout = false, .flush_each_write = true };
  f->audit = signet_audit_logger_new(&alc);
  CHECK(f->audit != NULL);

  n46_open_key_store(f);

  f->ps = signet_policy_store_file_new(f->policy_path);
  CHECK(f->ps != NULL);
  SignetPolicyEngineConfig pcfg = { .default_decision = SIGNET_POLICY_DECISION_DENY };
  f->pe = signet_policy_engine_new(f->ps, f->audit, &pcfg);
  CHECK(f->pe != NULL);

  /* Adopt the agent with a known one-time connect secret. */
  n46_adopt(f, "stew", f->stew_sk_hex, f->stew_pk_hex, "one-time-secret");

  n46_new_server(f);
}

static void n46_setup(N46Fixture *f) {
  n46_setup_mode(f, false);
}

static void n46_teardown(N46Fixture *f) {
  signet_nip46_server_free(f->srv);
  signet_policy_engine_free(f->pe);
  signet_policy_store_free(f->ps);
  signet_key_store_free(f->ks);
  signet_audit_logger_free(f->audit);
  if (f->db_path) unlink(f->db_path);
  g_free(f->db_path);
  unlink(f->policy_path);
  g_free(f->policy_path);
  unlink(f->audit_path);
  g_free(f->audit_path);
  sodium_memzero(f->bunker_sk_hex, sizeof(f->bunker_sk_hex));
  sodium_memzero(f->stew_sk_hex, sizeof(f->stew_sk_hex));
  sodium_memzero(f->client_sk_hex, sizeof(f->client_sk_hex));
}

/* Send one NIP-46 request from `client_sk/pk` and report whether it was
 * ALLOWED, judged by the auth_ok metrics counter delta (the server publishes
 * responses via relays, which are absent here). */
static bool n46_send_bytes(N46Fixture *f, const char *client_sk, const char *client_pk,
                           const uint8_t *request, size_t request_len,
                           const char *event_id, int64_t now) {
  uint8_t sk[32], pk[32];
  CHECK(hex_to_bytes(client_sk, sk, 32) == 0);
  CHECK(hex_to_bytes(f->bunker_pk_hex, pk, 32) == 0);
  char *cipher = NULL;
  CHECK(nostr_nip44_encrypt_v2(sk, pk, request,
                                request_len, &cipher) == 0 && cipher);
  sodium_memzero(sk, sizeof(sk));

  gint ok_before = g_atomic_int_get(&g_signet_metrics.auth_ok);
  (void)signet_nip46_server_handle_event(f->srv, f->bunker_pk_hex, f->bunker_sk_hex,
                                         client_pk, cipher, now, event_id, now);
  free(cipher);
  return g_atomic_int_get(&g_signet_metrics.auth_ok) > ok_before;
}

static bool n46_send(N46Fixture *f, const char *client_sk, const char *client_pk,
                     const char *request_json, const char *event_id, int64_t now) {
  return n46_send_bytes(f, client_sk, client_pk,
      (const uint8_t *)request_json, strlen(request_json), event_id, now);
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

/* The agent `client_pk` is bound to, in either mode, or NULL. */
static char *n46_read_binding(N46Fixture *f, const char *client_pk, int64_t now) {
  SignetStore *st = signet_key_store_get_store(f->ks);
  CHECK((st == NULL) == f->cache_only);
  char *agent = NULL;
  int rc = st ? signet_store_lookup_client_binding(st, client_pk, now, &agent, NULL)
              : signet_key_store_ephemeral_lookup_client(f->ks, client_pk, &agent, NULL, NULL);
  CHECK(rc == 0 || rc == 1);
  if (rc != 0) {
    g_free(agent);
    return NULL;
  }
  return agent;
}

static void n46_expect_binding(N46Fixture *f, const char *client_pk,
                               const char *agent_id, int64_t now) {
  char *bound = n46_read_binding(f, client_pk, now);
  if (agent_id) {
    CHECK(bound && strcmp(bound, agent_id) == 0);
  } else {
    CHECK(bound == NULL);
  }
  g_free(bound);
}

static void n46_expect_last_audit(N46Fixture *f, const char *method,
                                  const char *status, const char *code) {
  char *content = NULL;
  CHECK(g_file_get_contents(f->audit_path, &content, NULL, NULL));
  char **lines = g_strsplit(content, "\n", -1);
  const char *last = NULL;
  for (size_t i = 0; lines[i]; i++)
    if (lines[i][0]) last = lines[i];
  CHECK(last != NULL);
  char *m = g_strdup_printf("\"method\":\"%s\"", method);
  char *st = g_strdup_printf("\"status\":\"%s\"", status);
  char *cd = g_strdup_printf("\"code\":\"%s\"", code);
  CHECK(strstr(last, m) != NULL);
  CHECK(strstr(last, st) != NULL);
  CHECK(strstr(last, cd) != NULL);
  g_free(m);
  g_free(st);
  g_free(cd);
  g_strfreev(lines);
  g_free(content);
}

/* Sends method [peer, input] (standard NIP-46 shape) from a client. */
static void n46_crypto(N46Fixture *f, const char *client_sk, const char *client_pk,
                       const char *method, const char *input,
                       const char *event_id, int64_t now) {
  char *req = g_strdup_printf(
      "{\"id\":\"%s\",\"method\":\"%s\",\"params\":[\"%s\",\"%s\"]}",
      event_id, method, f->stew_pk_hex, input);
  (void)n46_send(f, client_sk, client_pk, req, event_id, now);
  g_free(req);
}

static void test_fenced_nip46_nip44_contract(void) {
  N46Fixture f;
  n46_setup(&f);
  int64_t now = (int64_t)time(NULL);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex,
                    "one-time-secret", "crypto-connect", now));
  char other_sk[65], other_pk[65];
  gen_keypair_hex(other_sk, other_pk);
  CHECK(signet_store_bind_client(signet_key_store_get_store(f.ks),
      "stew", f.stew_pk_hex, other_pk, NULL, now) == 0);
  /* Never fenced: every bound client keeps working. */
  n46_crypto(&f, other_sk, other_pk, "nip44_encrypt", "secret", "crypto-unfenced", now);
  n46_expect_last_audit(&f, "nip44_encrypt", "ok", "ok");

  int64_t epoch = 0;
  CHECK(signet_key_store_writer_acquire(f.ks, "stew", f.client_pk_hex, &epoch) == 0);
  CHECK(epoch == 1);
  n46_crypto(&f, f.client_sk_hex, f.client_pk_hex, "nip44_encrypt", "secret",
             "crypto-owner", now);
  n46_expect_last_audit(&f, "nip44_encrypt", "ok", "ok");
  n46_crypto(&f, other_sk, other_pk, "nip44_encrypt", "secret",
             "crypto-non-owner", now);
  n46_expect_last_audit(&f, "nip44_encrypt", "error", "crypto_failed");
  /* NIP-04 stays unavailable once an identity is fenced. */
  n46_crypto(&f, f.client_sk_hex, f.client_pk_hex, "nip04_encrypt", "secret",
             "crypto-nip04", now);
  n46_expect_last_audit(&f, "nip04_encrypt", "error", "crypto_failed");

  char *req = g_strdup_printf(
      "{\"id\":\"escaped-nul\",\"method\":\"nip44_encrypt\",\"params\":[\"%s\",\"secret\\u0000hidden\"]}",
      f.stew_pk_hex);
  (void)n46_send(&f, f.client_sk_hex, f.client_pk_hex, req,
                 "crypto-escaped-nul", now);
  n46_expect_last_audit(&f, "unknown", "error", "invalid_request");
  g_free(req);
  req = g_strdup_printf(
      "{\"id\":\"literal-nul\",\"method\":\"nip44_encrypt\",\"params\":[\"%s\",\"secret\"]}",
      f.stew_pk_hex);
  const char hidden[] = "\0hidden";
  GByteArray *literal_nul = g_byte_array_new();
  g_byte_array_append(literal_nul, (const uint8_t *)req, strlen(req));
  g_byte_array_append(literal_nul, (const uint8_t *)hidden, sizeof(hidden) - 1);
  (void)n46_send_bytes(&f, f.client_sk_hex, f.client_pk_hex,
                       literal_nul->data, literal_nul->len,
                       "crypto-literal-nul", now);
  n46_expect_last_audit(&f, "unknown", "error", "decrypt_failed");
  g_byte_array_unref(literal_nul);
  g_free(req);

  char *ciphertext = NULL;
  CHECK(signet_key_store_crypt_nip44(f.ks, "stew", f.client_pk_hex,
      "nip44_encrypt", f.stew_pk_hex, "unseal", &ciphertext) == 0);
  n46_crypto(&f, f.client_sk_hex, f.client_pk_hex, "nip44_decrypt", ciphertext,
             "crypto-decrypt", now);
  n46_expect_last_audit(&f, "nip44_decrypt", "ok", "ok");
  n46_crypto(&f, other_sk, other_pk, "nip44_decrypt", ciphertext,
             "crypto-decrypt-non-owner", now);
  n46_expect_last_audit(&f, "nip44_decrypt", "error", "crypto_failed");
  g_free(ciphertext);

  /* Binary-safe pair: [peer, base64(plaintext)] / [peer, ciphertext]. */
  char *binary = g_base64_encode((const guchar *)"\0\xff\x80", 3);
  n46_crypto(&f, f.client_sk_hex, f.client_pk_hex, "nip44_encrypt_b64", binary,
             "crypto-binary", now);
  n46_expect_last_audit(&f, "nip44_encrypt_b64", "ok", "ok");
  n46_crypto(&f, other_sk, other_pk, "nip44_encrypt_b64", binary,
             "crypto-binary-non-owner", now);
  n46_expect_last_audit(&f, "nip44_encrypt_b64", "error", "crypto_failed");
  CHECK(signet_key_store_crypt_nip44(f.ks, "stew", f.client_pk_hex,
      "nip44_encrypt_b64", f.stew_pk_hex, binary, &ciphertext) == 0);
  n46_crypto(&f, f.client_sk_hex, f.client_pk_hex, "nip44_decrypt", ciphertext,
             "crypto-binary-text", now);
  n46_expect_last_audit(&f, "nip44_decrypt", "error", "invalid_plaintext");
  n46_crypto(&f, f.client_sk_hex, f.client_pk_hex, "nip44_decrypt_b64", ciphertext,
             "crypto-binary-decrypt", now);
  n46_expect_last_audit(&f, "nip44_decrypt_b64", "ok", "ok");
  n46_crypto(&f, other_sk, other_pk, "nip44_decrypt_b64", ciphertext,
             "crypto-binary-decrypt-non-owner", now);
  n46_expect_last_audit(&f, "nip44_decrypt_b64", "error", "crypto_failed");
  g_free(ciphertext);
  g_free(binary);
  const uint8_t invalid_utf8[] = {0xff, 0xfe, 0x80};
  binary = g_base64_encode(invalid_utf8, sizeof(invalid_utf8));
  CHECK(signet_key_store_crypt_nip44(f.ks, "stew", f.client_pk_hex,
      "nip44_encrypt_b64", f.stew_pk_hex, binary, &ciphertext) == 0);
  n46_crypto(&f, f.client_sk_hex, f.client_pk_hex, "nip44_decrypt", ciphertext,
             "crypto-invalid-utf8", now);
  n46_expect_last_audit(&f, "nip44_decrypt", "error", "invalid_plaintext");
  n46_crypto(&f, f.client_sk_hex, f.client_pk_hex, "nip44_decrypt_b64", ciphertext,
             "crypto-invalid-utf8-b64", now);
  n46_expect_last_audit(&f, "nip44_decrypt_b64", "ok", "ok");
  g_free(ciphertext);
  g_free(binary);

  signet_nip46_server_free(f.srv);
  n46_new_server(&f);
  n46_crypto(&f, f.client_sk_hex, f.client_pk_hex, "nip44_encrypt", "after restart",
             "crypto-restart", now);
  n46_expect_last_audit(&f, "nip44_encrypt", "ok", "ok");

  /* Transfer: the former owner is rejected, the new owner works. */
  CHECK(signet_key_store_writer_acquire(f.ks, "stew", other_pk, &epoch) == 0);
  n46_crypto(&f, f.client_sk_hex, f.client_pk_hex, "nip44_encrypt", "stale",
             "crypto-former-owner", now);
  n46_expect_last_audit(&f, "nip44_encrypt", "error", "crypto_failed");
  n46_crypto(&f, other_sk, other_pk, "nip44_encrypt", "current",
             "crypto-new-owner", now);
  n46_expect_last_audit(&f, "nip44_encrypt", "ok", "ok");
  int64_t revoked = 0;
  CHECK(signet_key_store_writer_revoke(f.ks, "stew", &revoked) == 0);
  n46_crypto(&f, other_sk, other_pk, "nip44_encrypt", "secret",
             "crypto-revoked", now);
  n46_expect_last_audit(&f, "nip44_encrypt", "error", "crypto_failed");
  sodium_memzero(other_sk, sizeof(other_sk));
  n46_teardown(&f);
  printf("test_fenced_nip46_nip44_contract: PASS\n");
}

static void test_fenced_nip46_signing_contract(void) {
  N46Fixture f;
  n46_setup(&f);
  int64_t now = (int64_t)time(NULL);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex,
                    "one-time-secret", "lease-connect", now));
  char other_sk[65], other_pk[65];
  gen_keypair_hex(other_sk, other_pk);
  CHECK(signet_store_bind_client(signet_key_store_get_store(f.ks),
      "stew", f.stew_pk_hex, other_pk, NULL, now) == 0);
  char *event = g_strdup_printf(
      "{\\\"kind\\\":1,\\\"created_at\\\":%lld,\\\"tags\\\":[],\\\"content\\\":\\\"lease test\\\"}",
      (long long)now);
  /* Standard NIP-46 shape: params = [event_json]. */
  char *sign = g_strdup_printf(
      "{\"id\":\"sign\",\"method\":\"sign_event\",\"params\":[\"%s\"]}", event);
  gint signed_before = g_atomic_int_get(&g_signet_metrics.sign_total);
  /* Never fenced: any bound client signs. */
  (void)n46_send(&f, other_sk, other_pk, sign, "unfenced-other", now);
  (void)n46_send(&f, f.client_sk_hex, f.client_pk_hex, sign, "unfenced-client", now);
  CHECK(g_atomic_int_get(&g_signet_metrics.sign_total) == signed_before + 2);

  int64_t epoch = 0;
  CHECK(signet_key_store_writer_acquire(f.ks, "stew", f.client_pk_hex, &epoch) == 0);
  CHECK(epoch == 1);
  (void)n46_send(&f, other_sk, other_pk, sign, "fenced-non-owner", now);
  n46_expect_last_audit(&f, "sign_event", "error", "sign_failed");
  CHECK(g_atomic_int_get(&g_signet_metrics.sign_total) == signed_before + 2);
  (void)n46_send(&f, f.client_sk_hex, f.client_pk_hex, sign, "fenced-owner", now);
  n46_expect_last_audit(&f, "sign_event", "ok", "ok");
  CHECK(g_atomic_int_get(&g_signet_metrics.sign_total) == signed_before + 3);
  const char *bad_kind =
      "{\"id\":\"bad-kind\",\"method\":\"sign_event\",\"params\":[\"{\\\"kind\\\":1.5,\\\"created_at\\\":1,\\\"tags\\\":[],\\\"content\\\":\\\"x\\\"}\"]}";
  (void)n46_send(&f, f.client_sk_hex, f.client_pk_hex, bad_kind,
                 "lease-bad-kind", now);
  CHECK(g_atomic_int_get(&g_signet_metrics.sign_total) == signed_before + 3);
  /* There is no lease-renewal NIP-46 method. */
  (void)n46_send(&f, f.client_sk_hex, f.client_pk_hex,
      "{\"id\":\"renew\",\"method\":\"writer_renew\",\"params\":[\"1\"]}",
      "lease-renew", now);
  n46_expect_last_audit(&f, "writer_renew", "error", "unsupported_method");

  /* Transfer to the other client; the former owner is rejected. */
  CHECK(signet_key_store_writer_acquire(f.ks, "stew", other_pk, &epoch) == 0);
  CHECK(epoch == 2);
  (void)n46_send(&f, f.client_sk_hex, f.client_pk_hex, sign, "former-owner", now);
  n46_expect_last_audit(&f, "sign_event", "error", "sign_failed");
  (void)n46_send(&f, other_sk, other_pk, sign, "new-owner", now);
  n46_expect_last_audit(&f, "sign_event", "ok", "ok");
  CHECK(g_atomic_int_get(&g_signet_metrics.sign_total) == signed_before + 4);
  /* The former owner can never be reassigned, so its stale requests can
   * never pass the owner check again. */
  CHECK(signet_key_store_writer_acquire(f.ks, "stew", f.client_pk_hex, &epoch) != 0);
  int64_t next_epoch = 0;
  CHECK(signet_key_store_writer_revoke(f.ks, "stew", &next_epoch) == 0);
  (void)n46_send(&f, other_sk, other_pk, sign, "revoked", now);
  n46_expect_last_audit(&f, "sign_event", "error", "sign_failed");
  CHECK(g_atomic_int_get(&g_signet_metrics.sign_total) == signed_before + 4);
  sodium_memzero(other_sk, sizeof(other_sk));
  g_free(event);
  g_free(sign);
  n46_teardown(&f);
  printf("test_fenced_nip46_signing_contract: PASS\n");
}

/* 2 + 3 + 4 + 5: pairing persists; restarts and stale secrets reconnect. */
static void test_pair_once_reconnect_freely(void) {
  N46Fixture f;
  n46_setup(&f);
  int64_t now = 1752380000;

  /* First connect: valid secret → allowed, secret consumed, binding stored. */
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);
  char *bound = n46_read_binding(&f, f.client_pk_hex, now);
  CHECK(bound && strcmp(bound, "stew") == 0);
  g_free(bound);
  {
    char *agent = NULL;
    CHECK(signet_key_store_consume_connect_secret(f.ks, "one-time-secret", now, &agent) == 1);
  }

  /* Daemon restart: fresh server object, empty RAM sessions, same store.
   * A request (no connect first!) resolves via the persistent binding. */
  signet_nip46_server_free(f.srv);
  n46_new_server(&f);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == true);

  /* Restarted plugin behavior: connect with the STALE consumed secret —
   * accepted because it hashes to the client's OWN recorded pairing secret. */
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e3", now) == true);

  /* Secretless reconnect also works. */
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e4", now) == true);

  /* An ARBITRARY wrong secret from the bound client is rejected — stale
   * fallback only honors the client's own former secret, so probing and
   * misconfiguration surface as auth_failed instead of being masked. */
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "totally-wrong", "e5", now) == false);

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

  CHECK(n46_connect(&f, rando_sk, rando_pk, NULL, "e1", now) == false);
  CHECK(n46_connect(&f, rando_sk, rando_pk, "wrong-secret", "e2", now) == false);
  CHECK(n46_get_public_key(&f, rando_sk, rando_pk, "e3", now) == false);
  CHECK(n46_read_binding(&f, rando_pk, now) == NULL);

  sodium_memzero(rando_sk, sizeof(rando_sk));
  n46_teardown(&f);
  printf("test_unbound_client_rejected: PASS\n");
}

/* 7: revoked binding is dead immediately; a fresh secret re-pairs. */
static void test_revoked_binding_and_repair(void) {
  N46Fixture f;
  n46_setup(&f);
  int64_t now = 1752380000;

  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);

  /* Revoke the binding (as agent/revoke-client would). */
  SignetStore *st = signet_key_store_get_store(f.ks);
  CHECK(signet_store_revoke_client(st, f.client_pk_hex, now) == 0);

  /* Requests and secretless/stale reconnects are rejected at once — the
   * persistent table is authoritative, RAM session notwithstanding. */
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e3", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e4", now) == false);

  /* Re-pair with a freshly reissued one-time secret. */
  char re_pk[65] = {0};
  char *fresh = NULL;
  CHECK(signet_key_store_reissue_connect_secret(f.ks, "stew", NULL, NULL, NULL, 0,
                                                 re_pk, &fresh, NULL) == 0);
  CHECK(fresh != NULL);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, fresh, "e5", now) == true);
  char *bound = n46_read_binding(&f, f.client_pk_hex, now);
  CHECK(bound && strcmp(bound, "stew") == 0);
  g_free(bound);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e6", now) == true);

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

  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);

  SignetStore *st = signet_key_store_get_store(f.ks);
  CHECK(signet_revoke_agent(st, f.ks, NULL, NULL, "stew", f.stew_pk_hex,
                             "test revoke", now) == 0);

  CHECK(n46_read_binding(&f, f.client_pk_hex, now) == NULL);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e3", now) == false);

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
  CHECK(deny != NULL);
  signet_nip46_server_set_deny_list(f.srv, deny);

  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == true);

  /* Suspend. */
  CHECK(signet_deny_list_add(deny, f.stew_pk_hex, "stew", "suspended", now) == 0);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e3", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e4", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e5", now) == false);

  /* Lift. */
  CHECK(signet_deny_list_remove(deny, f.stew_pk_hex) == 0);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e6", now) == true);

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

  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == true);

  char new_pk[65] = {0};
  CHECK(signet_key_store_rotate_agent(f.ks, "stew", new_pk, sizeof(new_pk)) == 0);

  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e3", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e4", now) == false);

  n46_teardown(&f);
  printf("test_rotation_invalidates_binding: PASS\n");
}

/* Resurrection guard: revoke the agent, then reprovision the SAME agent_id
 * under a new key — the old client's binding must convey nothing. */
static void test_reprovision_does_not_resurrect_binding(void) {
  N46Fixture f;
  n46_setup(&f);
  int64_t now = 1752380000;

  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);

  SignetStore *st = signet_key_store_get_store(f.ks);
  CHECK(signet_revoke_agent(st, f.ks, NULL, NULL, "stew", f.stew_pk_hex,
                             "test revoke", now) == 0);

  /* Reprovision the same agent_id with a brand-new identity. */
  char sk2[65], pk2[65];
  gen_keypair_hex(sk2, pk2);
  uint8_t sk2_raw[32];
  CHECK(hex_to_bytes(sk2, sk2_raw, 32) == 0);
  char out_pk[65] = {0};
  CHECK(signet_key_store_adopt_agent(f.ks, "stew", sk2_raw, pk2,
                                      "new-secret", f.bunker_pk_hex,
                                      NULL, 0, out_pk, NULL) == SIGNET_ADOPT_OK);
  sodium_memzero(sk2_raw, sizeof(sk2_raw));
  sodium_memzero(sk2, sizeof(sk2));

  /* The old client's binding is both revoked AND pinned to the dead
   * identity — requests and secretless reconnects fail. */
  CHECK(n46_read_binding(&f, f.client_pk_hex, now) == NULL);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e3", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e4", now) == false);

  /* The client CAN re-pair with the new identity's fresh secret. */
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "new-secret", "e5", now) == true);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e6", now) == true);

  n46_teardown(&f);
  printf("test_reprovision_does_not_resurrect_binding: PASS\n");
}

/* ------------- reconnect, both modes (cache-only: nostrc-xjznk) ------------ */

static const char *n46_mode_name(bool cache_only) {
  return cache_only ? "cache-only" : "persistent";
}

/* 9: a paired client reconnects with no secret or its own spent secret —
 * across a fresh NIP-46 server object too — and is served throughout; an
 * arbitrary wrong secret is refused. */
static void test_reconnect_own_or_no_secret(bool cache_only) {
  N46Fixture f;
  n46_setup_mode(&f, cache_only);
  int64_t now = 1752380000;

  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e0", now) == false);
  n46_expect_binding(&f, f.client_pk_hex, NULL, now);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);
  n46_expect_binding(&f, f.client_pk_hex, "stew", now);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == true);

  /* The secret is single-use: no other client can pair with it now. */
  {
    char other_sk[65], other_pk[65];
    gen_keypair_hex(other_sk, other_pk);
    CHECK(n46_connect(&f, other_sk, other_pk, "one-time-secret", "e3", now) == false);
    n46_expect_binding(&f, other_pk, NULL, now);
    sodium_memzero(other_sk, sizeof(other_sk));
  }

  /* Reload/restart of the client (same key): re-sends its spent secret... */
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e4", now) == true);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e5", now) == true);
  /* ...or omits it. */
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e6", now) == true);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e7", now) == true);

  /* The binding belongs to the signer, not the NIP-46 server object. */
  signet_nip46_server_free(f.srv);
  n46_new_server(&f);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e8", now) == true);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e9", now) == true);

  /* A wrong secret is surfaced, not masked by the binding. */
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "totally-wrong", "e10", now) == false);
  n46_expect_last_audit(&f, "connect", "error", "auth_failed");
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e11", now) == true);

  n46_teardown(&f);
  printf("test_reconnect_own_or_no_secret (%s): PASS\n", n46_mode_name(cache_only));
}

/* 10a: another client's secret — spent or pending — never authorizes this
 * client: neither a fresh client nor one bound to a different agent can use
 * it to reconnect or to reach that secret's agent. */
static void test_reconnect_foreign_secret_refused(bool cache_only) {
  N46Fixture f;
  n46_setup_mode(&f, cache_only);
  int64_t now = 1752380000;

  char ops_sk[65], ops_pk[65], other_sk[65], other_pk[65];
  gen_keypair_hex(ops_sk, ops_pk);
  gen_keypair_hex(other_sk, other_pk);
  n46_adopt(&f, "ops", ops_sk, ops_pk, "ops-secret");

  /* client pairs with stew; other pairs with ops. */
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);
  CHECK(n46_connect(&f, other_sk, other_pk, "ops-secret", "e2", now) == true);

  /* Each re-sending the OTHER client's spent secret is refused... */
  CHECK(n46_connect(&f, other_sk, other_pk, "one-time-secret", "e3", now) == false);
  n46_expect_last_audit(&f, "connect", "error", "auth_failed");
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "ops-secret", "e4", now) == false);
  n46_expect_last_audit(&f, "connect", "error", "auth_failed");

  /* ...and moves neither binding: each still resolves to its own agent. */
  n46_expect_binding(&f, f.client_pk_hex, "stew", now);
  n46_expect_binding(&f, other_pk, "ops", now);
  CHECK(n46_connect(&f, other_sk, other_pk, NULL, "e5", now) == true);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e6", now) == true);
  CHECK(n46_get_public_key(&f, other_sk, other_pk, "e7", now) == true);

  /* A third, unpaired client gets nothing from either spent secret. */
  char third_sk[65], third_pk[65];
  gen_keypair_hex(third_sk, third_pk);
  CHECK(n46_connect(&f, third_sk, third_pk, "one-time-secret", "e8", now) == false);
  CHECK(n46_connect(&f, third_sk, third_pk, "ops-secret", "e9", now) == false);
  CHECK(n46_get_public_key(&f, third_sk, third_pk, "e10", now) == false);
  n46_expect_last_audit(&f, "get_public_key", "error", "not_connected");
  n46_expect_binding(&f, third_pk, NULL, now);

  sodium_memzero(ops_sk, sizeof(ops_sk));
  sodium_memzero(other_sk, sizeof(other_sk));
  sodium_memzero(third_sk, sizeof(third_sk));
  n46_teardown(&f);
  printf("test_reconnect_foreign_secret_refused (%s): PASS\n", n46_mode_name(cache_only));
}

/* 10b: an unknown client is refused with no secret, a wrong secret, or a
 * request, and does not consume the pending secret by trying. */
static void test_reconnect_unknown_client_refused(bool cache_only) {
  N46Fixture f;
  n46_setup_mode(&f, cache_only);
  int64_t now = 1752380000;

  char rando_sk[65], rando_pk[65];
  gen_keypair_hex(rando_sk, rando_pk);

  CHECK(n46_connect(&f, rando_sk, rando_pk, NULL, "e1", now) == false);
  n46_expect_last_audit(&f, "connect", "error", "auth_failed");
  CHECK(n46_connect(&f, rando_sk, rando_pk, "wrong-secret", "e2", now) == false);
  n46_expect_last_audit(&f, "connect", "error", "auth_failed");
  CHECK(n46_get_public_key(&f, rando_sk, rando_pk, "e3", now) == false);
  n46_expect_last_audit(&f, "get_public_key", "error", "not_connected");
  n46_expect_binding(&f, rando_pk, NULL, now);

  /* The real client can still pair with the untouched secret. */
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e4", now) == true);

  sodium_memzero(rando_sk, sizeof(rando_sk));
  n46_teardown(&f);
  printf("test_reconnect_unknown_client_refused (%s): PASS\n", n46_mode_name(cache_only));
}

/* 11: cache-only bindings are pinned to the identity they were made against:
 * rotate-key, and revoke followed by re-adopting the SAME agent_id, both
 * leave the old client with nothing until it pairs with a fresh secret. */
static void test_cache_only_identity_change_invalidates(void) {
  N46Fixture f;
  n46_setup_mode(&f, true);
  int64_t now = 1752380000;

  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);

  char new_pk[65] = {0};
  CHECK(signet_key_store_rotate_agent(f.ks, "stew", new_pk, sizeof(new_pk)) == 0);
  n46_expect_binding(&f, f.client_pk_hex, NULL, now);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e2", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e3", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e4", now) == false);

  /* Revoke, then re-adopt the ORIGINAL key under the same agent_id: the old
   * binding must not come back even though the identity pubkey matches. */
  CHECK(signet_key_store_revoke_agent(f.ks, "stew") == 0);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e5", now) == false);
  n46_adopt(&f, "stew", f.stew_sk_hex, f.stew_pk_hex, "fresh-secret");
  n46_expect_binding(&f, f.client_pk_hex, NULL, now);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e6", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e7", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e8", now) == false);

  /* The fresh secret re-pairs; then the usual reconnects work again. */
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "fresh-secret", "e9", now) == true);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "fresh-secret", "e10", now) == true);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e11", now) == true);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e12", now) == true);

  /* A second agent cannot be adopted with a secret already pending. */
  char ops_sk[65], ops_pk[65];
  gen_keypair_hex(ops_sk, ops_pk);
  uint8_t ops_raw[32];
  CHECK(hex_to_bytes(ops_sk, ops_raw, 32) == 0);
  char out_pk[65] = {0};
  n46_adopt(&f, "ops", ops_sk, ops_pk, "ops-secret");
  CHECK(signet_key_store_adopt_agent(f.ks, "ops2", ops_raw, NULL, "ops-secret",
                                      f.bunker_pk_hex, NULL, 0, out_pk,
                                      NULL) != SIGNET_ADOPT_OK);
  sodium_memzero(ops_raw, sizeof(ops_raw));
  sodium_memzero(ops_sk, sizeof(ops_sk));

  n46_teardown(&f);
  printf("test_cache_only_identity_change_invalidates: PASS\n");
}

/* 12: a cache-only signer keeps nothing across a restart. The client is
 * refused until the operator re-adopts the agent (same key) with a fresh
 * secret; the old, spent secret stays refused and the fresh one pairs. */
static void test_cache_only_restart_requires_pairing(void) {
  N46Fixture f;
  n46_setup_mode(&f, true);
  int64_t now = 1752380000;

  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);

  /* Restart: new key store and server, nothing persisted. */
  signet_nip46_server_free(f.srv);
  signet_key_store_free(f.ks);
  n46_open_key_store(&f);
  n46_new_server(&f);
  n46_expect_binding(&f, f.client_pk_hex, NULL, now);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e2", now) == false);
  n46_expect_last_audit(&f, "connect", "error", "auth_failed");
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e3", now) == false);

  /* Operator re-provisions the agent with a fresh secret; the client pairs. */
  n46_adopt(&f, "stew", f.stew_sk_hex, f.stew_pk_hex, "after-restart");
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e4", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e5", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "after-restart", "e6", now) == true);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e7", now) == true);

  n46_teardown(&f);
  printf("test_cache_only_restart_requires_pairing: PASS\n");
}

/* Reissues a connect secret for `agent_id` (provisioner path unless
 * `expected_pk`), returning it (g_free) or NULL with *rc set. */
static char *n46_reissue(N46Fixture *f, const char *agent_id, const char *expected_pk,
                         int *rc_out) {
  char user_pk[65] = {0};
  char *secret = NULL;
  char *uri = NULL;
  const char *relays[] = { "ws://127.0.0.1:1" };
  int rc = signet_key_store_reissue_connect_secret(f->ks, agent_id, expected_pk,
                                                   f->bunker_pk_hex, relays, 1,
                                                   user_pk, &secret, &uri);
  if (rc_out) *rc_out = rc;
  if (rc == 0) {
    char pk[65] = {0};
    CHECK(secret && strlen(secret) == 64);
    CHECK(signet_key_store_get_agent_pubkey(f->ks, agent_id, pk, sizeof(pk)));
    CHECK(strcmp(user_pk, pk) == 0);
    CHECK(uri && g_str_has_prefix(uri, "bunker://") && strstr(uri, secret) != NULL);
  } else {
    CHECK(secret == NULL && uri == NULL);
  }
  if (uri) {
    sodium_memzero(uri, strlen(uri));
    g_free(uri);
  }
  return secret;
}

static void n46_free_secret(char *secret) {
  if (!secret) return;
  sodium_memzero(secret, strlen(secret));
  g_free(secret);
}

/* 13a: cache-only rotate-key leaves the agent without a pending secret (like
 * the store row); reissue-connect mints one — provisioner or self-service
 * under the CURRENT identity — and the client re-pairs. */
static void test_cache_only_reissue_after_rotate(void) {
  N46Fixture f;
  n46_setup_mode(&f, true);
  int64_t now = 1752380000;
  int rc = 0;

  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);
  CHECK(signet_key_store_ephemeral_binding_count(f.ks) == 1);

  char new_pk[65] = {0};
  CHECK(signet_key_store_rotate_agent(f.ks, "stew", new_pk, sizeof(new_pk)) == 0);
  /* Dropped by the rotate itself, not on the client's next lookup. */
  CHECK(signet_key_store_ephemeral_binding_count(f.ks) == 0);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e2", now) == false);

  /* Errors: unknown agent; self-service under the superseded identity. */
  CHECK(n46_reissue(&f, "nobody", NULL, &rc) == NULL && rc == 1);
  CHECK(n46_reissue(&f, "stew", f.stew_pk_hex, &rc) == NULL && rc == 2);

  /* Provisioner reissue, then a second one: only the latest secret pairs. */
  char *first = n46_reissue(&f, "stew", NULL, &rc);
  CHECK(first && rc == 0);
  char *second = n46_reissue(&f, "stew", new_pk, &rc); /* self-service */
  CHECK(second && rc == 0 && strcmp(first, second) != 0);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, first, "e3", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, second, "e4", now) == true);
  n46_expect_binding(&f, f.client_pk_hex, "stew", now);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e5", now) == true);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, second, "e6", now) == true);
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "e7", now) == true);

  /* A reissue keeps existing bindings; its secret pairs one more client. */
  char *third = n46_reissue(&f, "stew", NULL, &rc);
  CHECK(third && rc == 0);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e8", now) == true);
  char other_sk[65], other_pk[65];
  gen_keypair_hex(other_sk, other_pk);
  CHECK(n46_connect(&f, other_sk, other_pk, third, "e9", now) == true);
  CHECK(n46_connect(&f, other_sk, other_pk, second, "e10", now) == false);
  CHECK(signet_key_store_ephemeral_binding_count(f.ks) == 2);

  sodium_memzero(other_sk, sizeof(other_sk));
  n46_free_secret(first);
  n46_free_secret(second);
  n46_free_secret(third);
  n46_teardown(&f);
  printf("test_cache_only_reissue_after_rotate: PASS\n");
}

/* 13b: cache-only restore drops the agent's bindings and keeps a pending
 * secret (as the store row does); reissue-connect re-pairs once none is
 * pending. */
static void test_cache_only_reissue_after_restore(void) {
  N46Fixture f;
  n46_setup_mode(&f, true);
  int64_t now = 1752380000;
  int rc = 0;

  /* Restore while the adopt secret is still pending: it survives. */
  uint8_t sk_raw[32];
  CHECK(hex_to_bytes(f.stew_sk_hex, sk_raw, 32) == 0);
  char out_pk[65] = {0};
  CHECK(signet_key_store_restore_agent(f.ks, "stew", sk_raw, f.stew_pk_hex, out_pk) ==
        SIGNET_ADOPT_OK);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);

  /* Restore again: binding dropped at once, no secret pending now. */
  CHECK(signet_key_store_restore_agent(f.ks, "stew", sk_raw, f.stew_pk_hex, out_pk) ==
        SIGNET_ADOPT_OK);
  sodium_memzero(sk_raw, sizeof(sk_raw));
  CHECK(signet_key_store_ephemeral_binding_count(f.ks) == 0);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e2", now) == false);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e3", now) == false);

  char *fresh = n46_reissue(&f, "stew", f.stew_pk_hex, &rc);
  CHECK(fresh && rc == 0);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, fresh, "e4", now) == true);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, NULL, "e5", now) == true);

  n46_free_secret(fresh);
  n46_teardown(&f);
  printf("test_cache_only_reissue_after_restore: PASS\n");
}

/* 13c: revoke and evict drop exactly that agent's bindings at once. */
static void test_cache_only_revoke_evict_drop_bindings(void) {
  N46Fixture f;
  n46_setup_mode(&f, true);
  int64_t now = 1752380000;
  int rc = 0;

  char ops_sk[65], ops_pk[65], a_sk[65], a_pk[65], b_sk[65], b_pk[65];
  gen_keypair_hex(ops_sk, ops_pk);
  gen_keypair_hex(a_sk, a_pk);
  gen_keypair_hex(b_sk, b_pk);
  n46_adopt(&f, "ops", ops_sk, ops_pk, "ops-secret");

  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, "one-time-secret", "e1", now) == true);
  char *more = n46_reissue(&f, "stew", NULL, &rc);
  CHECK(more && rc == 0);
  CHECK(n46_connect(&f, a_sk, a_pk, more, "e2", now) == true);
  CHECK(n46_connect(&f, b_sk, b_pk, "ops-secret", "e3", now) == true);
  CHECK(signet_key_store_ephemeral_binding_count(f.ks) == 3);

  CHECK(signet_key_store_revoke_agent(f.ks, "stew") == 0);
  CHECK(signet_key_store_ephemeral_binding_count(f.ks) == 1);
  n46_expect_binding(&f, b_pk, "ops", now);
  CHECK(n46_connect(&f, b_sk, b_pk, NULL, "e4", now) == true);

  CHECK(signet_key_store_evict_agent(f.ks, "ops") == 0);
  CHECK(signet_key_store_ephemeral_binding_count(f.ks) == 0);
  CHECK(n46_connect(&f, b_sk, b_pk, NULL, "e5", now) == false);

  n46_free_secret(more);
  sodium_memzero(ops_sk, sizeof(ops_sk));
  sodium_memzero(a_sk, sizeof(a_sk));
  sodium_memzero(b_sk, sizeof(b_sk));
  n46_teardown(&f);
  printf("test_cache_only_revoke_evict_drop_bindings: PASS\n");
}

#ifdef SIGNET_ENABLE_TEST_HOOKS
typedef struct {
  N46Fixture *fixture;
  char new_sk[65];
  char new_pk[65];
  unsigned int calls;
} BindingRace;

static void re_adopt_after_lookup(void *data) {
  BindingRace *race = data;
  CHECK(++race->calls == 1);
  CHECK(signet_key_store_revoke_agent(race->fixture->ks, "stew") == 0);
  n46_adopt(race->fixture, "stew", race->new_sk, race->new_pk, "new-secret");
}

static void test_re_adopt_between_lookup_and_custody(bool cache_only) {
  static const char *methods[] = {
      "get_public_key", "sign_event", "nip04_encrypt", "nip04_decrypt",
      "nip44_encrypt", "nip44_decrypt", "nip44_encrypt_b64", "nip44_decrypt_b64"
#ifdef SIGNET_ENABLE_PASSKEYS
      , "webauthn_get_info", "webauthn_make_credential", "webauthn_get_assertion",
      "webauthn_export", "webauthn_import"
#endif
  };
  for (size_t i = 0; i < G_N_ELEMENTS(methods); i++) {
    N46Fixture f;
    n46_setup_mode(&f, cache_only);
    int64_t now = 1752380000;
    CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex,
                      "one-time-secret", "race-connect", now));
    BindingRace race = {.fixture = &f};
    gen_keypair_hex(race.new_sk, race.new_pk);
    signet_nip46_server_set_after_binding_hook(f.srv, re_adopt_after_lookup, &race);
    if (strcmp(methods[i], "get_public_key") == 0) {
      (void)n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "race-key", now);
    } else if (strcmp(methods[i], "sign_event") == 0) {
      const char *req = "{\"id\":\"race-sign\",\"method\":\"sign_event\",\"params\":[\"{\\\"kind\\\":1,\\\"created_at\\\":1752380000,\\\"tags\\\":[],\\\"content\\\":\\\"x\\\"}\"]}";
      (void)n46_send(&f, f.client_sk_hex, f.client_pk_hex, req, "race-sign", now);
#ifdef SIGNET_ENABLE_PASSKEYS
    } else if (g_str_has_prefix(methods[i], "webauthn_")) {
      char *req = g_strdup_printf(
          "{\"id\":\"race-fido\",\"method\":\"%s\",\"params\":[\"{}\"]}", methods[i]);
      (void)n46_send(&f, f.client_sk_hex, f.client_pk_hex, req, "race-fido", now);
      g_free(req);
#endif
    } else {
      n46_crypto(&f, f.client_sk_hex, f.client_pk_hex, methods[i],
                 "input", "race-crypto", now);
    }
    CHECK(race.calls == 1);
    n46_expect_last_audit(&f, methods[i], "error", "not_connected");
    signet_nip46_server_set_after_binding_hook(f.srv, NULL, NULL);
    CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex,
                      "new-secret", "race-repair", now));
    sodium_memzero(race.new_sk, sizeof(race.new_sk));
    n46_teardown(&f);
  }
  printf("test_re_adopt_between_lookup_and_custody (%s): PASS\n",
         n46_mode_name(cache_only));
}
#endif

static void test_spent_secret_reuse_rejected(bool cache_only) {
  N46Fixture f;
  n46_setup_mode(&f, cache_only);
  int64_t now = 1752380000;
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex,
                    "one-time-secret", "spent-1", now));
  char other_sk[65], other_pk[65], out_pk[65] = {0};
  uint8_t other_raw[32];
  gen_keypair_hex(other_sk, other_pk);
  CHECK(hex_to_bytes(other_sk, other_raw, sizeof(other_raw)) == 0);
  CHECK(signet_key_store_adopt_agent(f.ks, "ops", other_raw, other_pk,
      "one-time-secret", f.bunker_pk_hex, NULL, 0, out_pk, NULL) != SIGNET_ADOPT_OK);
  CHECK(signet_key_store_adopt_agent(f.ks, "ops", other_raw, other_pk,
      "distinct-secret", f.bunker_pk_hex, NULL, 0, out_pk, NULL) == SIGNET_ADOPT_OK);
#ifdef SIGNET_ENABLE_TEST_HOOKS
  int rc = 0;
  signet_key_store_test_next_reissue_secret(f.ks, "one-time-secret");
  CHECK(n46_reissue(&f, "ops", NULL, &rc) == NULL && rc != 0);
  signet_key_store_test_next_reissue_secret(f.ks, "distinct-secret");
  CHECK(n46_reissue(&f, "stew", NULL, &rc) == NULL && rc != 0);
  signet_key_store_test_next_reissue_secret(f.ks, "distinct-secret");
  CHECK(n46_reissue(&f, "ops", NULL, &rc) == NULL && rc != 0);
  char *fresh = n46_reissue(&f, "stew", NULL, &rc);
  CHECK(fresh && rc == 0);
  CHECK(n46_connect(&f, other_sk, other_pk, fresh, "spent-2", now));
  n46_free_secret(fresh);
  fresh = n46_reissue(&f, "stew", NULL, &rc);
  CHECK(fresh && rc == 0);
  CHECK(n46_connect(&f, f.client_sk_hex, f.client_pk_hex, fresh, "spent-rebind", now));
  n46_free_secret(fresh);
#endif
  CHECK(n46_get_public_key(&f, f.client_sk_hex, f.client_pk_hex, "spent-3", now));
  CHECK(signet_key_store_revoke_agent(f.ks, "stew") == 0);
  char new_sk[65], new_pk[65];
  uint8_t new_raw[32];
  gen_keypair_hex(new_sk, new_pk);
  CHECK(hex_to_bytes(new_sk, new_raw, sizeof(new_raw)) == 0);
  CHECK(signet_key_store_adopt_agent(f.ks, "stew", new_raw, new_pk,
      "one-time-secret", f.bunker_pk_hex, NULL, 0, out_pk, NULL) != SIGNET_ADOPT_OK);
  /* Different key and different secret remain usable after the refusal. */
  CHECK(signet_key_store_adopt_agent(f.ks, "stew", new_raw, new_pk,
      "revived-secret", f.bunker_pk_hex, NULL, 0, out_pk, NULL) == SIGNET_ADOPT_OK);
  sodium_memzero(new_raw, sizeof(new_raw));
  sodium_memzero(new_sk, sizeof(new_sk));
  sodium_memzero(other_raw, sizeof(other_raw));
  sodium_memzero(other_sk, sizeof(other_sk));
  n46_teardown(&f);
  printf("test_spent_secret_reuse_rejected (%s): PASS\n", n46_mode_name(cache_only));
}

int main(void) {
  if (sodium_init() < 0) {
    fprintf(stderr, "sodium_init failed\n");
    return 1;
  }

  test_store_binding_lifecycle();
  for (int cache_only = 0; cache_only <= 1; cache_only++)
    test_spent_secret_reuse_rejected(cache_only != 0);
  test_pair_once_reconnect_freely();
  test_unbound_client_rejected();
  test_revoked_binding_and_repair();
  test_agent_revocation_revokes_bindings();
  test_suspended_agent_binding_refused();
  test_rotation_invalidates_binding();
  test_reprovision_does_not_resurrect_binding();
  test_fenced_nip46_signing_contract();
  test_fenced_nip46_nip44_contract();

  for (int cache_only = 0; cache_only <= 1; cache_only++) {
    test_reconnect_own_or_no_secret(cache_only);
    test_reconnect_foreign_secret_refused(cache_only);
    test_reconnect_unknown_client_refused(cache_only);
#ifdef SIGNET_ENABLE_TEST_HOOKS
    test_re_adopt_between_lookup_and_custody(cache_only);
#endif
  }
  test_cache_only_identity_change_invalidates();
  test_cache_only_restart_requires_pairing();
  test_cache_only_reissue_after_rotate();
  test_cache_only_reissue_after_restore();
  test_cache_only_revoke_evict_drop_bindings();

  printf("All client binding tests passed.\n");
  return 0;
}
