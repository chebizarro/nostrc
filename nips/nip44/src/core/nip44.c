#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include "nostr/nip44/nip44.h"

int nip44_hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t info_len,
                      uint8_t okm_out[], size_t okm_len);
int nip44_hmac_sha256(const uint8_t *key, size_t key_len,
                      const uint8_t *data1, size_t len1,
                      const uint8_t *data2, size_t len2,
                      uint8_t mac_out[32]);
int nip44_chacha20_xor(const uint8_t key[32], const uint8_t nonce12[12],
                       const uint8_t *in, uint8_t *out, size_t len);
int nip44_base64_encode(const uint8_t *buf, size_t len, char **out_b64);
int nip44_base64_decode(const char *b64, uint8_t **out_buf, size_t *out_len);
int nip44_pad(const uint8_t *in, size_t in_len,
              uint8_t **out_padded, size_t *out_padded_len);
int nip44_unpad(const uint8_t *padded, size_t padded_len,
                uint8_t **out, size_t *out_len);

int nostr_nip44_encrypt_v2_with_convkey(const uint8_t convkey[32],
                                        const uint8_t *plaintext_utf8,
                                        size_t plaintext_len,
                                        char **out_base64) {
  uint8_t nonce[32] = {0};
  uint8_t okm[76] = {0};
  uint8_t mac[32] = {0};
  uint8_t *padded = NULL;
  uint8_t *cipher = NULL;
  uint8_t *payload = NULL;
  size_t padded_len = 0;
  size_t payload_len = 0;
  int rc = -1;

  if (out_base64) *out_base64 = NULL;
  if (!convkey || !plaintext_utf8 || !out_base64) return -1;

  if (RAND_bytes(nonce, sizeof(nonce)) != 1) goto done;
  if (nip44_hkdf_expand(convkey, nonce, sizeof(nonce),
                        okm, sizeof(okm)) != 0) {
    goto done;
  }

  if (nip44_pad(plaintext_utf8, plaintext_len,
                &padded, &padded_len) != 0) {
    goto done;
  }

  cipher = (uint8_t *)malloc(padded_len);
  if (!cipher) goto done;
  if (nip44_chacha20_xor(okm, okm + 32, padded,
                         cipher, padded_len) != 0) {
    goto done;
  }

  if (nip44_hmac_sha256(okm + 44, 32, nonce, sizeof(nonce),
                        cipher, padded_len, mac) != 0) {
    goto done;
  }

  payload_len = 1 + sizeof(nonce) + padded_len + sizeof(mac);
  payload = (uint8_t *)malloc(payload_len);
  if (!payload) goto done;

  size_t off = 0;
  payload[off++] = (uint8_t)NOSTR_NIP44_V2;
  memcpy(payload + off, nonce, sizeof(nonce));
  off += sizeof(nonce);
  memcpy(payload + off, cipher, padded_len);
  off += padded_len;
  memcpy(payload + off, mac, sizeof(mac));

  if (nip44_base64_encode(payload, payload_len, out_base64) != 0) goto done;
  rc = 0;

done:
  OPENSSL_cleanse(nonce, sizeof(nonce));
  OPENSSL_cleanse(okm, sizeof(okm));
  OPENSSL_cleanse(mac, sizeof(mac));
  if (cipher) {
    OPENSSL_cleanse(cipher, padded_len);
    free(cipher);
  }
  if (padded) {
    OPENSSL_cleanse(padded, padded_len);
    free(padded);
  }
  if (payload) {
    OPENSSL_cleanse(payload, payload_len);
    free(payload);
  }
  if (rc != 0 && out_base64) {
    free(*out_base64);
    *out_base64 = NULL;
  }
  return rc;
}

int nostr_nip44_decrypt_v2_with_convkey(const uint8_t convkey[32],
                                        const char *base64_payload,
                                        uint8_t **out_plaintext,
                                        size_t *out_plaintext_len) {
  uint8_t okm[76] = {0};
  uint8_t mac_calc[32] = {0};
  uint8_t *payload = NULL;
  uint8_t *padded = NULL;
  size_t payload_len = 0;
  size_t cipher_len = 0;
  int rc = -1;

  if (out_plaintext) *out_plaintext = NULL;
  if (out_plaintext_len) *out_plaintext_len = 0;
  if (!convkey || !base64_payload || !out_plaintext ||
      !out_plaintext_len) {
    return -1;
  }

  if (nip44_base64_decode(base64_payload, &payload, &payload_len) != 0) {
    goto done;
  }
  /* Minimum padded plaintext is two length bytes plus 32 padded bytes. */
  if (payload_len < 1 + 32 + 34 + 32 || payload[0] != NOSTR_NIP44_V2) {
    goto done;
  }

  const uint8_t *nonce = payload + 1;
  const uint8_t *cipher = payload + 1 + 32;
  cipher_len = payload_len - 1 - 32 - 32;
  const uint8_t *mac = payload + payload_len - 32;

  if (nip44_hkdf_expand(convkey, nonce, 32, okm, sizeof(okm)) != 0) {
    goto done;
  }
  if (nip44_hmac_sha256(okm + 44, 32, nonce, 32,
                        cipher, cipher_len, mac_calc) != 0) {
    goto done;
  }
  if (CRYPTO_memcmp(mac, mac_calc, sizeof(mac_calc)) != 0) goto done;

  padded = (uint8_t *)malloc(cipher_len);
  if (!padded) goto done;
  if (nip44_chacha20_xor(okm, okm + 32, cipher,
                         padded, cipher_len) != 0) {
    goto done;
  }

  if (nip44_unpad(padded, cipher_len,
                  out_plaintext, out_plaintext_len) != 0) {
    goto done;
  }
  rc = 0;

done:
  OPENSSL_cleanse(okm, sizeof(okm));
  OPENSSL_cleanse(mac_calc, sizeof(mac_calc));
  if (padded) {
    OPENSSL_cleanse(padded, cipher_len);
    free(padded);
  }
  if (payload) {
    OPENSSL_cleanse(payload, payload_len);
    free(payload);
  }
  if (rc != 0) {
    free(*out_plaintext);
    *out_plaintext = NULL;
    *out_plaintext_len = 0;
  }
  return rc;
}

int nostr_nip44_encrypt_v2(const uint8_t sender_sk[32],
                           const uint8_t receiver_pk_xonly[32],
                           const uint8_t *plaintext_utf8,
                           size_t plaintext_len,
                           char **out_base64) {
  uint8_t convkey[32] = {0};
  int rc = -1;

  if (!out_base64) return -1;
  *out_base64 = NULL;
  if (nostr_nip44_convkey(sender_sk, receiver_pk_xonly, convkey) == 0) {
    rc = nostr_nip44_encrypt_v2_with_convkey(
        convkey, plaintext_utf8, plaintext_len, out_base64);
  }
  OPENSSL_cleanse(convkey, sizeof(convkey));
  return rc;
}

int nostr_nip44_decrypt_v2(const uint8_t receiver_sk[32],
                           const uint8_t sender_pk_xonly[32],
                           const char *base64_payload,
                           uint8_t **out_plaintext,
                           size_t *out_plaintext_len) {
  uint8_t convkey[32] = {0};
  int rc = -1;

  if (!out_plaintext || !out_plaintext_len) return -1;
  *out_plaintext = NULL;
  *out_plaintext_len = 0;
  if (nostr_nip44_convkey(receiver_sk, sender_pk_xonly, convkey) == 0) {
    rc = nostr_nip44_decrypt_v2_with_convkey(
        convkey, base64_payload, out_plaintext, out_plaintext_len);
  }
  OPENSSL_cleanse(convkey, sizeof(convkey));
  return rc;
}
