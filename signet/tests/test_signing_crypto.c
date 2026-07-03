/* SPDX-License-Identifier: MIT
 *
 * test_signing_crypto.c - Proves REAL cryptographic behavior end to end:
 *   1. An agent key provisioned + loaded from the store produces a valid
 *      BIP-340 Schnorr signature whose derived pubkey matches the stored one.
 *   2. NIP-44 v2 encrypt/decrypt round-trips between two agent identities, and
 *      tampered ciphertext / wrong-key decryption are rejected.
 *   3. Credential payloads are envelope-encrypted at rest: the plaintext never
 *      appears in the on-disk database (or WAL), yet round-trips correctly.
 *
 * These replace placeholder assertions that only checked pointers were non-NULL.
 */

#include "signet/key_store.h"
#include "signet/store.h"
#include "signet/store_secrets.h"
#include "signet/audit_logger.h"

#include <nostr-event.h>
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

static char *make_temp_db_path(void) {
  char tmpl[] = "/tmp/signet-test-crypto-XXXXXX.db";
  int fd = mkstemps(tmpl, 3);
  assert(fd >= 0);
  close(fd);
  unlink(tmpl);
  return g_strdup(tmpl);
}

static void bytes_to_hex(const uint8_t *in, size_t n, char *out) {
  static const char *h = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[i * 2] = h[in[i] >> 4]; out[i * 2 + 1] = h[in[i] & 0xf]; }
  out[n * 2] = '\0';
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned int b;
    if (sscanf(hex + i * 2, "%2x", &b) != 1) return -1;
    out[i] = (uint8_t)b;
  }
  return 0;
}

/* Portable binary substring search (avoids relying on memmem availability). */
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

/* 1. Signing correctness: a stored agent key yields a valid Schnorr signature
 *    and the event's derived pubkey equals the agent's stored pubkey. */
static void test_agent_key_signs_valid_event(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_ks(&db_path);

  char pubkey_hex[65] = {0};
  assert(signet_key_store_provision_agent(ks, "signer", NULL, NULL, 0,
                                          pubkey_hex, sizeof(pubkey_hex), NULL) == 0);
  assert(strlen(pubkey_hex) == 64);

  SignetLoadedKey lk;
  memset(&lk, 0, sizeof(lk));
  assert(signet_key_store_load_agent_key(ks, "signer", &lk));
  assert(lk.secret_key && lk.secret_key_len == 32);

  char sk_hex[65];
  bytes_to_hex(lk.secret_key, 32, sk_hex);

  NostrEvent *evt = nostr_event_new();
  assert(evt);
  nostr_event_set_kind(evt, 1);
  nostr_event_set_created_at(evt, 1700000000);
  nostr_event_set_content(evt, "signet signing correctness test");
  assert(nostr_event_sign(evt, sk_hex) == 0);

  /* Independent verification of id + Schnorr signature. */
  assert(nostr_event_check_signature(evt));
  const char *evt_id = nostr_event_get_id(evt);
  assert(evt_id && strlen(evt_id) == 64);

  /* The pubkey the signature commits to must be the agent's stored pubkey. */
  const char *evt_pub = nostr_event_get_pubkey(evt);
  assert(evt_pub && strcmp(evt_pub, pubkey_hex) == 0);

  nostr_event_free(evt);
  sodium_memzero(sk_hex, sizeof(sk_hex));
  signet_loaded_key_clear(&lk);
  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_agent_key_signs_valid_event: PASS\n");
}

/* 2. NIP-44 v2 round-trip + tamper + wrong-key rejection between two agents. */
static void test_nip44_roundtrip(void) {
  char *db_path = NULL;
  SignetKeyStore *ks = open_ks(&db_path);

  char a_pub[65] = {0}, b_pub[65] = {0}, c_pub[65] = {0};
  assert(signet_key_store_provision_agent(ks, "alice", NULL, NULL, 0, a_pub, sizeof(a_pub), NULL) == 0);
  assert(signet_key_store_provision_agent(ks, "bob",   NULL, NULL, 0, b_pub, sizeof(b_pub), NULL) == 0);
  assert(signet_key_store_provision_agent(ks, "carol", NULL, NULL, 0, c_pub, sizeof(c_pub), NULL) == 0);

  SignetLoadedKey ak, bk, ck;
  memset(&ak, 0, sizeof(ak)); memset(&bk, 0, sizeof(bk)); memset(&ck, 0, sizeof(ck));
  assert(signet_key_store_load_agent_key(ks, "alice", &ak));
  assert(signet_key_store_load_agent_key(ks, "bob", &bk));
  assert(signet_key_store_load_agent_key(ks, "carol", &ck));

  uint8_t a_pk[32], b_pk[32];
  assert(hex_to_bytes(a_pub, a_pk, 32) == 0);
  assert(hex_to_bytes(b_pub, b_pk, 32) == 0);

  const char *msg = "the ships hung in the sky much the way bricks don't";
  char *ct = NULL;
  assert(nostr_nip44_encrypt_v2(ak.secret_key, b_pk, (const uint8_t *)msg, strlen(msg), &ct) == 0);
  assert(ct != NULL);
  assert(strstr(ct, "bricks") == NULL); /* ciphertext is not the plaintext */

  /* Bob decrypts with his sk + Alice's pk. */
  uint8_t *pt = NULL; size_t pt_len = 0;
  assert(nostr_nip44_decrypt_v2(bk.secret_key, a_pk, ct, &pt, &pt_len) == 0);
  assert(pt && pt_len == strlen(msg) && memcmp(pt, msg, pt_len) == 0);
  free(pt);

  /* Tampered ciphertext must fail the MAC check. */
  size_t clen = strlen(ct);
  char *tampered = g_strdup(ct);
  tampered[clen / 2] = (tampered[clen / 2] == 'A') ? 'B' : 'A';
  uint8_t *pt2 = NULL; size_t pt2_len = 0;
  assert(nostr_nip44_decrypt_v2(bk.secret_key, a_pk, tampered, &pt2, &pt2_len) != 0);
  g_free(tampered);

  /* A third party (carol) cannot decrypt Alice->Bob ciphertext. */
  uint8_t *pt3 = NULL; size_t pt3_len = 0;
  assert(nostr_nip44_decrypt_v2(ck.secret_key, a_pk, ct, &pt3, &pt3_len) != 0);

  free(ct);
  signet_loaded_key_clear(&ak);
  signet_loaded_key_clear(&bk);
  signet_loaded_key_clear(&ck);
  signet_key_store_free(ks);
  unlink(db_path);
  g_free(db_path);
  printf("test_nip44_roundtrip: PASS\n");
}

/* 3. Envelope encryption at rest: plaintext credential never hits disk. */
static void test_envelope_encryption_at_rest(void) {
  char *db_path = make_temp_db_path();
  SignetStoreConfig cfg = { .db_path = db_path, .master_key = MASTER_KEY };
  SignetStore *store = signet_store_open(&cfg);
  assert(store);

  const char *marker = "SUPER_SECRET_PLAINTEXT_MARKER_9f3a7c";
  size_t mlen = strlen(marker);

  assert(signet_store_put_secret(store, "cred-1", "agent-x", "deadbeef",
                                 SIGNET_SECRET_API_TOKEN, "label",
                                 (const uint8_t *)marker, mlen, NULL,
                                 1700000000) == 0);

  /* Round-trip: the decrypted payload matches exactly. */
  SignetSecretRecord rec;
  memset(&rec, 0, sizeof(rec));
  assert(signet_store_get_secret(store, "cred-1", &rec) == 0);
  assert(rec.payload && rec.payload_len == mlen && memcmp(rec.payload, marker, mlen) == 0);
  signet_secret_record_clear(&rec);

  bool enc = signet_store_is_encrypted(store);
  signet_store_close(store); /* checkpoints WAL */

  /* The plaintext marker must not appear in the db or its WAL sidecar. This
   * proves the envelope-encryption layer works regardless of whether the build
   * links SQLCipher. */
  bool found = false;
  size_t n = 0;
  uint8_t *b = read_file_bytes(db_path, &n);
  if (b) { found = found || buf_contains(b, n, (const uint8_t *)marker, mlen); free(b); }
  char *wal = g_strdup_printf("%s-wal", db_path);
  b = read_file_bytes(wal, &n);
  if (b) { found = found || buf_contains(b, n, (const uint8_t *)marker, mlen); free(b); }
  assert(!found);

  unlink(db_path);
  unlink(wal);
  g_free(wal);
  char *shm = g_strdup_printf("%s-shm", db_path);
  unlink(shm);
  g_free(shm);
  g_free(db_path);
  printf("test_envelope_encryption_at_rest: PASS (sqlcipher_backend=%s)\n", enc ? "yes" : "no");
}

int main(void) {
  assert(sodium_init() >= 0);
  test_agent_key_signs_valid_event();
  test_nip44_roundtrip();
  test_envelope_encryption_at_rest();
  printf("All signing/crypto tests passed!\n");
  return 0;
}
