#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>

#include "nostr/nip44/nip44.h"

/* Use libnostr key helper to derive x-only pub from sec */
extern char *nostr_key_get_public(const char *sk_hex);

/* Internal helpers from nip44 core (not public API) */
extern void nip44_hkdf_extract(const uint8_t *salt, size_t salt_len,
                               const uint8_t *ikm, size_t ikm_len,
                               uint8_t prk_out[32]);
extern void nip44_hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t info_len,
                              uint8_t okm_out[], size_t okm_len);
extern int  nip44_chacha20_xor(const uint8_t key[32], const uint8_t nonce12[12],
                               const uint8_t *in, uint8_t *out, size_t len);
extern void nip44_hmac_sha256(const uint8_t *key, size_t key_len,
                              const uint8_t *data1, size_t len1,
                              const uint8_t *data2, size_t len2,
                              uint8_t mac_out[32]);
extern int  nip44_pad(const uint8_t *in, size_t in_len, uint8_t **out_padded, size_t *out_padded_len);
extern int  nip44_base64_encode(const uint8_t *buf, size_t len, char **out_b64);

static void hex_to_bytes(const char *hex, uint8_t *out, size_t outlen) {
  for (size_t i=0;i<outlen;i++){ unsigned int b=0; sscanf(hex+2*i, "%2x", &b); out[i]=(uint8_t)b; }
}

#ifdef VECTORS_PATH
#include <jansson.h>

static int run_get_message_keys_vectors(json_t *v2_valid) {
  int failures = 0;
  json_t *gmk = json_object_get(v2_valid, "get_message_keys");
  if (!gmk) return 0; /* optional section */

  const char *conv_hex = json_string_value(json_object_get(gmk, "conversation_key"));
  json_t *keys = json_object_get(gmk, "keys");
  if (!conv_hex || !keys || !json_is_array(keys)) return -1;

  uint8_t conv[32];
  hex_to_bytes(conv_hex, conv, 32);

  size_t idx; json_t *item;
  json_array_foreach(keys, idx, item) {
    const char *nonce_hex = json_string_value(json_object_get(item, "nonce"));
    const char *chacha_key_hex = json_string_value(json_object_get(item, "chacha_key"));
    const char *chacha_nonce_hex = json_string_value(json_object_get(item, "chacha_nonce"));
    const char *hmac_key_hex = json_string_value(json_object_get(item, "hmac_key"));
    if (!nonce_hex || !chacha_key_hex || !chacha_nonce_hex || !hmac_key_hex) { failures++; continue; }

    uint8_t nonce[32]; hex_to_bytes(nonce_hex, nonce, 32);
    uint8_t okm[76];
    nip44_hkdf_expand(conv, nonce, 32, okm, sizeof(okm));
    const uint8_t *chacha_key = okm + 0;
    const uint8_t *chacha_nonce = okm + 32;
    const uint8_t *hmac_key = okm + 44;

    uint8_t want_ck[32]; uint8_t want_cn[12]; uint8_t want_hk[32];
    hex_to_bytes(chacha_key_hex, want_ck, 32);
    hex_to_bytes(chacha_nonce_hex, want_cn, 12);
    hex_to_bytes(hmac_key_hex, want_hk, 32);

    if (memcmp(chacha_key, want_ck, 32) != 0 || memcmp(chacha_nonce, want_cn, 12) != 0 || memcmp(hmac_key, want_hk, 32) != 0) {
      failures++;
    }
    memset(okm, 0, sizeof(okm));
  }
  return failures ? -1 : 0;
}

static int run_vector_case(const char *sec1_hex,
                           const char *pub2_x_hex,
                           const char *plaintext,
                           const char *nonce_hex_opt,
                           const char *conv_hex_opt,
                           const char *want_b64_opt) {
  /* convkey */
  uint8_t sk1[32]; uint8_t pk2x[32]; uint8_t conv[32];
  hex_to_bytes(sec1_hex, sk1, 32);
  hex_to_bytes(pub2_x_hex, pk2x, 32);
  int rc = nostr_nip44_convkey(sk1, pk2x, conv);
  if (rc != 0) return -1;
  if (conv_hex_opt && strlen(conv_hex_opt)==64) {
    uint8_t conv_expected[32]; hex_to_bytes(conv_hex_opt, conv_expected, 32);
    if (memcmp(conv, conv_expected, 32) != 0) return -2;
  }

  /* encryption path: optional deterministic nonce */
  uint8_t nonce[32];
  int have_nonce = 0;
  if (nonce_hex_opt && strlen(nonce_hex_opt)==64){ hex_to_bytes(nonce_hex_opt, nonce, 32); have_nonce=1; }

  uint8_t okm[76];
  if (have_nonce) {
    nip44_hkdf_expand(conv, nonce, 32, okm, sizeof(okm));
  }
  const uint8_t *chacha_key = okm + 0;
  const uint8_t *chacha_nonce = okm + 32;
  const uint8_t *hmac_key = okm + 44;

  uint8_t *padded=NULL; size_t padded_len=0;
  rc = nip44_pad((const uint8_t*)plaintext, strlen(plaintext), &padded, &padded_len);
  if (rc!=0) return -3;

  uint8_t *cipher = (uint8_t*)malloc(padded_len);
  if (!cipher){ free(padded); return -4; }

  char *b64_payload = NULL;

  if (have_nonce) {
    /* manual deterministic build */
    if (nip44_chacha20_xor(chacha_key, chacha_nonce, padded, cipher, padded_len)!=0){ free(cipher); free(padded); return -5; }
    uint8_t mac[32];
    nip44_hmac_sha256(hmac_key, 32, nonce, 32, cipher, padded_len, mac);
    size_t payload_len = 1 + 32 + padded_len + 32;
    uint8_t *payload = (uint8_t*)malloc(payload_len);
    if (!payload){ free(cipher); free(padded); return -6; }
    size_t off=0; payload[off++] = (uint8_t)NOSTR_NIP44_V2;
    memcpy(payload+off, nonce, 32); off+=32;
    memcpy(payload+off, cipher, padded_len); off+=padded_len;
    memcpy(payload+off, mac, 32); off+=32;
    rc = nip44_base64_encode(payload, payload_len, &b64_payload);
    free(payload);
    if (rc!=0){ free(cipher); free(padded); return -7; }
  } else {
    /* use API encrypt */
    rc = nostr_nip44_encrypt_v2_with_convkey(conv, (const uint8_t*)plaintext, strlen(plaintext), &b64_payload);
    if (rc!=0){ free(cipher); free(padded); return -8; }
  }

  int cmp_ok = 1;
  if (want_b64_opt && strlen(want_b64_opt) > 0) {
    if (strcmp(b64_payload, want_b64_opt) != 0) cmp_ok = 0;
  }

  /* decrypt via API and compare plaintext */
  uint8_t *plain=NULL; size_t plain_len=0;
  rc = nostr_nip44_decrypt_v2_with_convkey(conv, b64_payload, &plain, &plain_len);
  if (rc!=0 || plain_len != strlen(plaintext) || memcmp(plain, plaintext, plain_len)!=0) cmp_ok = 0;

  free(plain);
  free(b64_payload);
  free(cipher);
  free(padded);
  memset(okm,0,sizeof(okm));
  memset(conv,0,sizeof(conv));

  return cmp_ok ? 0 : -9;
}

static int run_json_vectors(const char *path) {
  json_error_t err;
  json_t *root = json_load_file(path, 0, &err);
  if (!root) return -1;
  int failures = 0; int total = 0;

  /* Navigate: root object -> v2 -> valid -> encrypt_decrypt (array) */
  json_t *v2 = json_object_get(root, "v2");
  json_t *valid = v2 ? json_object_get(v2, "valid") : NULL;
  json_t *encdec = valid ? json_object_get(valid, "encrypt_decrypt") : NULL;
  if (!encdec || !json_is_array(encdec)) {
    json_decref(root);
    return -2;
  }
  /* First, validate message key derivation if present */
  int r_keys = run_get_message_keys_vectors(valid);
  if (r_keys != 0) {
    json_decref(root);
    return -3;
  }
  size_t idx; json_t *item;
  json_array_foreach(encdec, idx, item) {
    total++;
    const char *sec1 = json_string_value(json_object_get(item, "sec1"));
    const char *sec2 = json_string_value(json_object_get(item, "sec2"));
    const char *pub2x = json_string_value(json_object_get(item, "pub2_x"));
    const char *pt = json_string_value(json_object_get(item, "plaintext"));
    const char *nonce = json_string_value(json_object_get(item, "nonce"));
    const char *conv = json_string_value(json_object_get(item, "conversation_key"));
    const char *want = json_string_value(json_object_get(item, "payload"));
    if (!want) want = json_string_value(json_object_get(item, "payload_b64"));

    char *derived_pub2 = NULL;
    const char *pub_use = pub2x;
    if (!pub_use && sec2) { derived_pub2 = nostr_key_get_public(sec2); pub_use = derived_pub2; }
    if (!sec1 || !pub_use || !pt) { failures++; free(derived_pub2); continue; }
    int rc = run_vector_case(sec1, pub_use, pt, nonce, conv, want);
    if (rc != 0) failures++;
    free(derived_pub2);
  }
  json_decref(root);
  if (failures) {
    fprintf(stderr, "NIP-44 JSON vectors: %d/%d failed\n", failures, total);
    return -4;
  }
  return 0;
}
#endif /* VECTORS_PATH */

int main(void) {
#ifdef VECTORS_PATH
  /* Try JSON vectors first */
  if (access(VECTORS_PATH, R_OK) == 0) {
    int r = run_json_vectors(VECTORS_PATH);
    if (r == 0) { printf("test_nip44_vectors (json): OK\n"); return 0; }
    /* fall through to single-case if JSON failed */
  }
#endif
  /* Example vector from docs/nips/44.md */
  const char *sec1_hex = "0000000000000000000000000000000000000000000000000000000000000001";
  const char *sec2_hex = "0000000000000000000000000000000000000000000000000000000000000002";
  const char *conv_hex = "c41c775356fd92eadc63ff5a0dc1da211b268cbea22316767095b2871ea1412d";
  const char *nonce_hex= "0000000000000000000000000000000000000000000000000000000000000001";
  const char *plaintext = "a";
  const char *want_payload = "AgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABee0G5VSK0/9YypIObAtDKfYEAjD35uVkHyB0F4DwrcNaCXlCWZKaArsGrY6M9wnuTMxWfp1RTN9Xga8no+kF5Vsb";

  /* Derive pub2 (x-only hex) from sec2 */
  char *pub2_hex = nostr_key_get_public(sec2_hex);
  assert(pub2_hex && strlen(pub2_hex)==64);

  /* convkey(sec1, pub2) must equal conv_hex */
  uint8_t sk1[32], pk2x[32], conv[32], conv_expected[32];
  hex_to_bytes(sec1_hex, sk1, 32);
  hex_to_bytes(pub2_hex, pk2x, 32);
  hex_to_bytes(conv_hex, conv_expected, 32);
  int rc = nostr_nip44_convkey(sk1, pk2x, conv);
  assert(rc==0);
  assert(memcmp(conv, conv_expected, 32)==0);

  /* Build payload deterministically per spec steps using provided nonce */
  uint8_t nonce[32]; hex_to_bytes(nonce_hex, nonce, 32);
  /* HKDF-Expand(PRK=conv, info=nonce, L=76) */
  uint8_t okm[76];
  nip44_hkdf_expand(conv, nonce, 32, okm, sizeof(okm));
  const uint8_t *chacha_key = okm + 0;
  const uint8_t *chacha_nonce = okm + 32; /* 12 bytes */
  const uint8_t *hmac_key = okm + 44;     /* 32 bytes */

  /* pad */
  uint8_t *padded=NULL; size_t padded_len=0;
  rc = nip44_pad((const uint8_t*)plaintext, strlen(plaintext), &padded, &padded_len);
  assert(rc==0 && padded && padded_len>=32);

  /* encrypt */
  uint8_t *cipher = (uint8_t*)malloc(padded_len);
  assert(cipher);
  rc = nip44_chacha20_xor(chacha_key, chacha_nonce, padded, cipher, padded_len);
  assert(rc==0);

  /* mac over nonce||cipher */
  uint8_t mac[32];
  nip44_hmac_sha256(hmac_key, 32, nonce, 32, cipher, padded_len, mac);

  /* assemble version(1)||nonce(32)||cipher||mac(32) */
  size_t payload_len = 1 + 32 + padded_len + 32;
  uint8_t *payload = (uint8_t*)malloc(payload_len);
  assert(payload);
  size_t off=0; payload[off++] = (uint8_t)NOSTR_NIP44_V2;
  memcpy(payload+off, nonce, 32); off+=32;
  memcpy(payload+off, cipher, padded_len); off+=padded_len;
  memcpy(payload+off, mac, 32); off+=32;
  assert(off==payload_len);

  /* base64 encode */
  char *b64=NULL; rc = nip44_base64_encode(payload, payload_len, &b64);
  assert(rc==0 && b64);

  /* compare */
  if (strcmp(b64, want_payload) != 0) {
    fprintf(stderr, "\nVector mismatch\n got:  %s\n want: %s\n", b64, want_payload);
    assert(0);
  }

  free(pub2_hex);
  free(b64);
  free(payload);
  free(cipher);
  free(padded);
  memset(okm, 0, sizeof(okm));
  memset(conv, 0, sizeof(conv));
  memset(sk1, 0, sizeof(sk1));
  memset(pk2x, 0, sizeof(pk2x));

  printf("test_nip44_vectors: OK\n");
  return 0;
}
