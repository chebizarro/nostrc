#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>

#include "nostr/nip44/nip44.h"

/* Use libnostr key helper to derive x-only pub from sec */
extern char *nostr_key_get_public(const char *sk_hex);

/* Internal helpers from nip44 core (not public API) */
extern int nip44_hkdf_extract(const uint8_t *salt, size_t salt_len,
                              const uint8_t *ikm, size_t ikm_len,
                              uint8_t prk_out[32]);
extern int nip44_hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t info_len,
                             uint8_t okm_out[], size_t okm_len);
extern int  nip44_chacha20_xor(const uint8_t key[32], const uint8_t nonce12[12],
                               const uint8_t *in, uint8_t *out, size_t len);
extern int nip44_hmac_sha256(const uint8_t *key, size_t key_len,
                             const uint8_t *data1, size_t len1,
                             const uint8_t *data2, size_t len2,
                             uint8_t mac_out[32]);
extern int  nip44_pad(const uint8_t *in, size_t in_len, uint8_t **out_padded, size_t *out_padded_len);
extern size_t nip44_calc_padded_len(size_t unpadded_len);
extern int  nip44_base64_encode(const uint8_t *buf, size_t len, char **out_b64);

static void hex_to_bytes(const char *hex, uint8_t *out, size_t outlen) {
  for (size_t i=0;i<outlen;i++){ unsigned int b=0; sscanf(hex+2*i, "%2x", &b); out[i]=(uint8_t)b; }
}

#if !defined(VECTORS_PATH) || !defined(VECTORS_SHA256)
#error "Official NIP-44 JSON vectors require VECTORS_PATH and VECTORS_SHA256; no builtin fallback"
#endif
#include <jansson.h>

static int run_conversation_key_vectors(json_t *v2_valid) {
  json_t *cases = json_object_get(v2_valid, "get_conversation_key");
  if (!json_is_array(cases)) return -1;

  size_t idx;
  json_t *item;
  json_array_foreach(cases, idx, item) {
    const char *sec = json_string_value(json_object_get(item, "sec1"));
    const char *pub = json_string_value(json_object_get(item, "pub2"));
    const char *expected = json_string_value(json_object_get(item, "conversation_key"));
    if (!sec || !pub || !expected) return -1;

    uint8_t sk[32], pk[32], got[32], want[32];
    hex_to_bytes(sec, sk, sizeof(sk));
    hex_to_bytes(pub, pk, sizeof(pk));
    hex_to_bytes(expected, want, sizeof(want));
    if (nostr_nip44_convkey(sk, pk, got) != 0 ||
        memcmp(got, want, sizeof(got)) != 0) {
      return -1;
    }
    memset(sk, 0, sizeof(sk));
    memset(got, 0, sizeof(got));
  }
  return 0;
}

static int run_invalid_vectors(json_t *v2) {
  json_t *invalid = json_object_get(v2, "invalid");
  if (!json_is_object(invalid)) return -1;

  json_t *bad_keys = json_object_get(invalid, "get_conversation_key");
  if (!json_is_array(bad_keys)) return -1;
  size_t idx;
  json_t *item;
  json_array_foreach(bad_keys, idx, item) {
    const char *sec = json_string_value(json_object_get(item, "sec1"));
    const char *pub = json_string_value(json_object_get(item, "pub2"));
    uint8_t sk[32], pk[32], out[32];
    if (!sec || !pub) return -1;
    hex_to_bytes(sec, sk, sizeof(sk));
    hex_to_bytes(pub, pk, sizeof(pk));
    memset(out, 0xa5, sizeof(out));
    if (nostr_nip44_convkey(sk, pk, out) == 0) return -1;
    for (size_t i = 0; i < sizeof(out); ++i) {
      if (out[i] != 0) return -1;
    }
  }

  json_t *bad_decrypt = json_object_get(invalid, "decrypt");
  if (!json_is_array(bad_decrypt)) return -1;
  json_array_foreach(bad_decrypt, idx, item) {
    const char *conv_hex = json_string_value(json_object_get(item, "conversation_key"));
    const char *payload = json_string_value(json_object_get(item, "payload"));
    uint8_t conv[32];
    uint8_t *plain = (uint8_t *)0x1;
    size_t plain_len = 123;
    if (!conv_hex || !payload) return -1;
    hex_to_bytes(conv_hex, conv, sizeof(conv));
    if (nostr_nip44_decrypt_v2_with_convkey(
            conv, payload, &plain, &plain_len) == 0 ||
        plain != NULL || plain_len != 0) {
      free(plain);
      return -1;
    }
  }

  json_t *bad_lengths = json_object_get(invalid, "encrypt_msg_lengths");
  if (!json_is_array(bad_lengths)) return -1;
  uint8_t conv[32] = {0};
  const uint8_t one = 'x';
  json_array_foreach(bad_lengths, idx, item) {
    const json_int_t len = json_integer_value(item);
    char *cipher = (char *)0x1;
    if (len < 0 || nostr_nip44_encrypt_v2_with_convkey(
                       conv, &one, (size_t)len, &cipher) == 0 ||
        cipher != NULL) {
      free(cipher);
      return -1;
    }
  }
  return 0;
}

/*
 * NOTE (nostrc-3nj): jansson is retained here for loading external JSON test
 * vector files from disk. The NostrJsonInterface is specifically for Nostr
 * protocol JSON (events, envelopes, filters), not arbitrary JSON file loading.
 */
static int run_get_message_keys_vectors(json_t *v2_valid) {
  int failures = 0;
  json_t *gmk = json_object_get(v2_valid, "get_message_keys");
  if (!json_is_object(gmk)) return -1;

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
    if (nip44_hkdf_expand(conv, nonce, 32, okm, sizeof(okm)) != 0) {
      failures++;
      continue;
    }
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

static int deterministic_encrypt(const uint8_t conv[32],
                                 const uint8_t nonce[32],
                                 const uint8_t *plaintext,
                                 size_t plaintext_len,
                                 char **out_b64) {
  uint8_t okm[76] = {0};
  uint8_t mac[32] = {0};
  uint8_t *padded = NULL;
  uint8_t *cipher = NULL;
  uint8_t *payload = NULL;
  size_t padded_len = 0;
  size_t payload_len = 0;
  int rc = -1;
  *out_b64 = NULL;

  if (nip44_hkdf_expand(conv, nonce, 32, okm, sizeof(okm)) != 0 ||
      nip44_pad(plaintext, plaintext_len, &padded, &padded_len) != 0) {
    goto done;
  }
  cipher = malloc(padded_len);
  if (!cipher ||
      nip44_chacha20_xor(okm, okm + 32, padded, cipher, padded_len) != 0 ||
      nip44_hmac_sha256(okm + 44, 32, nonce, 32,
                        cipher, padded_len, mac) != 0) {
    goto done;
  }
  payload_len = 1 + 32 + padded_len + 32;
  payload = malloc(payload_len);
  if (!payload) goto done;
  size_t off = 0;
  payload[off++] = NOSTR_NIP44_V2;
  memcpy(payload + off, nonce, 32); off += 32;
  memcpy(payload + off, cipher, padded_len); off += padded_len;
  memcpy(payload + off, mac, 32);
  rc = nip44_base64_encode(payload, payload_len, out_b64);

done:
  OPENSSL_cleanse(okm, sizeof(okm));
  OPENSSL_cleanse(mac, sizeof(mac));
  if (padded) { OPENSSL_cleanse(padded, padded_len); free(padded); }
  if (cipher) { OPENSSL_cleanse(cipher, padded_len); free(cipher); }
  if (payload) { OPENSSL_cleanse(payload, payload_len); free(payload); }
  return rc;
}

static int digest_matches(const uint8_t *data, size_t len, const char *hex) {
  uint8_t digest[SHA256_DIGEST_LENGTH];
  uint8_t expected[SHA256_DIGEST_LENGTH];
  if (!data || !hex || strlen(hex) != SHA256_DIGEST_LENGTH * 2) return 0;
  SHA256(data, len, digest);
  hex_to_bytes(hex, expected, sizeof(expected));
  return memcmp(digest, expected, sizeof(digest)) == 0;
}

static int run_padding_vectors(json_t *valid) {
  json_t *cases = json_object_get(valid, "calc_padded_len");
  if (!json_is_array(cases)) return -1;
  size_t idx;
  json_t *item;
  json_array_foreach(cases, idx, item) {
    if (!json_is_array(item) || json_array_size(item) != 2) return -1;
    const size_t input = (size_t)json_integer_value(json_array_get(item, 0));
    const size_t expected = (size_t)json_integer_value(json_array_get(item, 1));
    if (nip44_calc_padded_len(input) != expected) return -1;
  }
  return 0;
}

static int run_long_vectors(json_t *valid) {
  json_t *cases = json_object_get(valid, "encrypt_decrypt_long_msg");
  if (!json_is_array(cases)) return -1;
  size_t idx;
  json_t *item;
  json_array_foreach(cases, idx, item) {
    const char *conv_hex = json_string_value(json_object_get(item, "conversation_key"));
    const char *nonce_hex = json_string_value(json_object_get(item, "nonce"));
    const char *pattern = json_string_value(json_object_get(item, "pattern"));
    const size_t repeat = (size_t)json_integer_value(json_object_get(item, "repeat"));
    const char *plain_hash = json_string_value(json_object_get(item, "plaintext_sha256"));
    const char *payload_hash = json_string_value(json_object_get(item, "payload_sha256"));
    if (!conv_hex || !nonce_hex || !pattern || !plain_hash || !payload_hash) return -1;

    const size_t pattern_len = strlen(pattern);
    if (pattern_len == 0 || repeat > 65535 / pattern_len) return -1;
    const size_t plaintext_len = pattern_len * repeat;
    uint8_t *plaintext = malloc(plaintext_len);
    if (!plaintext) return -1;
    for (size_t i = 0; i < repeat; ++i) {
      memcpy(plaintext + i * pattern_len, pattern, pattern_len);
    }
    if (!digest_matches(plaintext, plaintext_len, plain_hash)) {
      free(plaintext);
      return -1;
    }

    uint8_t conv[32], nonce[32];
    hex_to_bytes(conv_hex, conv, sizeof(conv));
    hex_to_bytes(nonce_hex, nonce, sizeof(nonce));
    char *payload = NULL;
    if (deterministic_encrypt(conv, nonce, plaintext, plaintext_len,
                              &payload) != 0 ||
        !digest_matches((const uint8_t *)payload, strlen(payload),
                        payload_hash)) {
      free(payload);
      OPENSSL_cleanse(plaintext, plaintext_len);
      free(plaintext);
      return -1;
    }
    free(payload);
    OPENSSL_cleanse(plaintext, plaintext_len);
    free(plaintext);
  }
  return 0;
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
  if (!have_nonce || nip44_hkdf_expand(conv, nonce, 32, okm, sizeof(okm)) != 0) {
    return -3;
  }
  const uint8_t *chacha_key = okm + 0;
  const uint8_t *chacha_nonce = okm + 32;
  const uint8_t *hmac_key = okm + 44;

  uint8_t *padded=NULL; size_t padded_len=0;
  rc = nip44_pad((const uint8_t*)plaintext, strlen(plaintext), &padded, &padded_len);
  if (rc!=0) return -4;

  uint8_t *cipher = (uint8_t*)malloc(padded_len);
  if (!cipher){ free(padded); return -4; }

  char *b64_payload = NULL;

  if (have_nonce) {
    /* manual deterministic build */
    if (nip44_chacha20_xor(chacha_key, chacha_nonce, padded, cipher, padded_len)!=0){ free(cipher); free(padded); return -5; }
    uint8_t mac[32];
    if (nip44_hmac_sha256(hmac_key, 32, nonce, 32, cipher, padded_len, mac) != 0) {
      free(cipher); free(padded); return -6;
    }
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
  /* Read once: the bytes hashed here are exactly the bytes parsed and executed. */
  FILE *file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "official NIP-44 JSON vectors unreadable: %s: %s\n",
            path, strerror(errno));
    return -1;
  }
  unsigned char bytes[37631]; /* pinned 37630-byte input, plus one to detect growth */
  const size_t len = fread(bytes, 1, sizeof(bytes), file);
  const int read_failed = ferror(file);
  const int close_failed = fclose(file);
  if (read_failed || close_failed) {
    fprintf(stderr, "official NIP-44 JSON vectors read failed: %s\n", path);
    return -1;
  }
  json_error_t err;
  json_t *root = json_loadb((const char *)bytes, len, JSON_REJECT_DUPLICATES, &err);
  if (!root) {
    fprintf(stderr, "official NIP-44 JSON vectors malformed: line %d: %s\n",
            err.line, err.text);
    return -1;
  }
  if (len != 37630 || !digest_matches(bytes, len, VECTORS_SHA256)) {
    fprintf(stderr, "official NIP-44 JSON vectors SHA256 mismatch: %s\n", path);
    json_decref(root);
    return -1;
  }
  int failures = 0; int total = 0;

  /* Navigate: root object -> v2 -> valid -> encrypt_decrypt (array) */
  json_t *v2 = json_object_get(root, "v2");
  json_t *valid = v2 ? json_object_get(v2, "valid") : NULL;
  json_t *encdec = valid ? json_object_get(valid, "encrypt_decrypt") : NULL;
  if (!encdec || !json_is_array(encdec)) {
    json_decref(root);
    return -2;
  }
  /* Validate every official key-derivation class before payloads. */
  if (run_conversation_key_vectors(valid) != 0) {
    json_decref(root);
    return -31;
  }
  if (run_get_message_keys_vectors(valid) != 0) {
    json_decref(root);
    return -32;
  }
  if (run_invalid_vectors(v2) != 0) {
    json_decref(root);
    return -33;
  }
  if (run_padding_vectors(valid) != 0) {
    json_decref(root);
    return -34;
  }
  if (run_long_vectors(valid) != 0) {
    json_decref(root);
    return -35;
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
  if (failures) {
    fprintf(stderr, "NIP-44 JSON vectors: %d/%d failed\n", failures, total);
    json_decref(root);
    return -4;
  }
  json_t *invalid = json_object_get(v2, "invalid");
  printf("official NIP-44 JSON vectors: sha256=%s "
         "conversation_keys=%zu message_keys=%zu padding=%zu encrypt_decrypt=%d "
         "long_messages=%zu invalid_lengths=%zu invalid_keys=%zu invalid_decrypt=%zu\n",
         VECTORS_SHA256,
         json_array_size(json_object_get(valid, "get_conversation_key")),
         json_array_size(json_object_get(json_object_get(valid, "get_message_keys"), "keys")),
         json_array_size(json_object_get(valid, "calc_padded_len")), total,
         json_array_size(json_object_get(valid, "encrypt_decrypt_long_msg")),
         json_array_size(json_object_get(invalid, "encrypt_msg_lengths")),
         json_array_size(json_object_get(invalid, "get_conversation_key")),
         json_array_size(json_object_get(invalid, "decrypt")));
  json_decref(root);
  return 0;
}

int main(int argc, char **argv) {
  /* An alternate file is only for refusal diagnostics; the same hash is required. */
  if (argc > 2) {
    fprintf(stderr, "usage: %s [official-vector-file]\n", argv[0]);
    return 1;
  }
  const int rc = run_json_vectors(argc == 2 ? argv[1] : VECTORS_PATH);
  if (rc != 0) {
    fprintf(stderr, "official NIP-44 vector conformance failed: %d\n", rc);
    return 1;
  }
  printf("test_nip44_vectors (official json): OK\n");
  return 0;
}
