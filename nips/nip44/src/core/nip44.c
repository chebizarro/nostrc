#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include "nostr/nip44/nip44.h"

/* Internal helpers */
void nip44_hkdf_extract(const uint8_t *salt, size_t salt_len,
                        const uint8_t *ikm, size_t ikm_len,
                        uint8_t prk_out[32]);
void nip44_hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t info_len,
                       uint8_t okm_out[], size_t okm_len);
void nip44_hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *data1, size_t len1,
                       const uint8_t *data2, size_t len2,
                       uint8_t mac_out[32]);
int  nip44_chacha20_xor(const uint8_t key[32], const uint8_t nonce12[12],
                        const uint8_t *in, uint8_t *out, size_t len);
int  nip44_base64_encode(const uint8_t *buf, size_t len, char **out_b64);
int  nip44_base64_decode(const char *b64, uint8_t **out_buf, size_t *out_len);
int  nip44_pad(const uint8_t *in, size_t in_len, uint8_t **out_padded, size_t *out_padded_len);
int  nip44_unpad(const uint8_t *padded, size_t padded_len, uint8_t **out, size_t *out_len);

int nostr_nip44_convkey(const uint8_t sender_sk[32],
                        const uint8_t receiver_pk_xonly[32],
                        uint8_t out_convkey[32]); /* implemented in nip44_convkey.c */

static const uint8_t NIP44_SALT[] = { 'n','i','p','4','4','-','v','2' };

int nostr_nip44_encrypt_v2_with_convkey(const uint8_t convkey[32],
                                        const uint8_t *plaintext_utf8, size_t plaintext_len,
                                        char **out_base64) {
  if (!convkey || !plaintext_utf8 || !out_base64) return -1;

  /* Nonce: 32 random bytes */
  uint8_t nonce[32];
  if (RAND_bytes(nonce, sizeof(nonce)) != 1) return -1;

  /* Key schedule: HKDF-Expand(PRK=convkey, info=nonce, L=76) */
  uint8_t okm[76];
  nip44_hkdf_expand(convkey, nonce, sizeof(nonce), okm, sizeof(okm));
  const uint8_t *chacha_key = okm + 0;
  const uint8_t *chacha_nonce = okm + 32; /* 12 bytes */
  const uint8_t *hmac_key = okm + 44;     /* 32 bytes */

  /* Padding */
  uint8_t *padded = NULL; size_t padded_len = 0;
  if (nip44_pad(plaintext_utf8, plaintext_len, &padded, &padded_len) != 0) return -1;

  /* Encrypt */
  uint8_t *cipher = malloc(padded_len);
  if (!cipher) { free(padded); return -1; }
  if (nip44_chacha20_xor(chacha_key, chacha_nonce, padded, cipher, padded_len) != 0) {
    OPENSSL_cleanse(cipher, padded_len); free(cipher); free(padded); return -1;
  }

  /* MAC over AAD = nonce || ciphertext */
  uint8_t mac[32];
  nip44_hmac_sha256(hmac_key, 32, nonce, sizeof(nonce), cipher, padded_len, mac);

  /* Assemble: version(1) || nonce(32) || ciphertext || mac(32), then base64 */
  size_t payload_len = 1 + sizeof(nonce) + padded_len + sizeof(mac);
  uint8_t *payload = malloc(payload_len);
  if (!payload) { OPENSSL_cleanse(cipher, padded_len); free(cipher); free(padded); return -1; }
  size_t off = 0;
  payload[off++] = (uint8_t)NOSTR_NIP44_V2;
  memcpy(payload + off, nonce, sizeof(nonce)); off += sizeof(nonce);
  memcpy(payload + off, cipher, padded_len); off += padded_len;
  memcpy(payload + off, mac, sizeof(mac)); off += sizeof(mac);

  int rc = nip44_base64_encode(payload, payload_len, out_base64);

  OPENSSL_cleanse(cipher, padded_len);
  free(cipher);
  OPENSSL_cleanse(padded, padded_len);
  free(padded);
  OPENSSL_cleanse(okm, sizeof(okm));
  OPENSSL_cleanse(payload, payload_len);
  free(payload);

  return rc;
}

int nostr_nip44_decrypt_v2_with_convkey(const uint8_t convkey[32],
                                        const char *base64_payload,
                                        uint8_t **out_plaintext, size_t *out_plaintext_len) {
  if (!convkey || !base64_payload || !out_plaintext || !out_plaintext_len) return -1;
  uint8_t *payload = NULL; size_t payload_len = 0;
  if (nip44_base64_decode(base64_payload, &payload, &payload_len) != 0) return -1;
  if (payload_len < 1 + 32 + 32 + 32) { free(payload); return -1; }

  size_t off = 0;
  uint8_t version = payload[off++];
  if (version != (uint8_t)NOSTR_NIP44_V2) { free(payload); return -1; }

  const uint8_t *nonce = payload + off; off += 32;
  const uint8_t *cipher = payload + off; size_t cipher_len = payload_len - off - 32;
  const uint8_t *mac = payload + payload_len - 32;

  /* Key schedule */
  uint8_t okm[76];
  nip44_hkdf_expand(convkey, nonce, 32, okm, sizeof(okm));
  const uint8_t *chacha_key = okm + 0;
  const uint8_t *chacha_nonce = okm + 32;
  const uint8_t *hmac_key = okm + 44;

  /* Verify MAC constant-time */
  uint8_t mac_calc[32];
  nip44_hmac_sha256(hmac_key, 32, nonce, 32, cipher, cipher_len, mac_calc);
  if (CRYPTO_memcmp(mac, mac_calc, 32) != 0) { OPENSSL_cleanse(okm, sizeof(okm)); free(payload); return -1; }

  /* Decrypt */
  uint8_t *padded = malloc(cipher_len);
  if (!padded) { OPENSSL_cleanse(okm, sizeof(okm)); free(payload); return -1; }
  if (nip44_chacha20_xor(chacha_key, chacha_nonce, cipher, padded, cipher_len) != 0) {
    OPENSSL_cleanse(padded, cipher_len); free(padded); OPENSSL_cleanse(okm, sizeof(okm)); free(payload); return -1;
  }

  int rc = nip44_unpad(padded, cipher_len, out_plaintext, out_plaintext_len);
  OPENSSL_cleanse(padded, cipher_len);
  free(padded);
  OPENSSL_cleanse(okm, sizeof(okm));
  free(payload);
  return rc;
}

int nostr_nip44_encrypt_v2(const uint8_t sender_sk[32],
                           const uint8_t receiver_pk_xonly[32],
                           const uint8_t *plaintext_utf8, size_t plaintext_len,
                           char **out_base64) {
  uint8_t convkey[32];
  if (nostr_nip44_convkey(sender_sk, receiver_pk_xonly, convkey) != 0) return -1;
  int rc = nostr_nip44_encrypt_v2_with_convkey(convkey, plaintext_utf8, plaintext_len, out_base64);
  OPENSSL_cleanse(convkey, sizeof(convkey));
  return rc;
}

int nostr_nip44_decrypt_v2(const uint8_t receiver_sk[32],
                           const uint8_t sender_pk_xonly[32],
                           const char *base64_payload,
                           uint8_t **out_plaintext, size_t *out_plaintext_len) {
  uint8_t convkey[32];
  if (nostr_nip44_convkey(receiver_sk, sender_pk_xonly, convkey) != 0) return -1;
  int rc = nostr_nip44_decrypt_v2_with_convkey(convkey, base64_payload, out_plaintext, out_plaintext_len);
  OPENSSL_cleanse(convkey, sizeof(convkey));
  return rc;
}
