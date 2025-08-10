#include <sodium.h>
#include <stdint.h>
#include <string.h>

// XChaCha20-Poly1305 IETF wrappers (AEAD)
int nip49_aead_encrypt_xchacha20poly1305(const uint8_t key32[32],
                                         const uint8_t nonce24[24],
                                         const uint8_t *ad, size_t ad_len,
                                         const uint8_t pt32[32],
                                         uint8_t out_ct48[48]) {
  if (!key32 || !nonce24 || !pt32 || !out_ct48) return -1;
  unsigned long long clen = 0ULL;
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(out_ct48, &clen,
                                                  pt32, 32,
                                                  ad, ad_len,
                                                  NULL,
                                                  nonce24,
                                                  key32) != 0) {
    return -1;
  }
  return (clen == 48ULL) ? 0 : -1;
}

int nip49_aead_decrypt_xchacha20poly1305(const uint8_t key32[32],
                                         const uint8_t nonce24[24],
                                         const uint8_t *ad, size_t ad_len,
                                         const uint8_t ct48[48],
                                         uint8_t out_pt32[32]) {
  if (!key32 || !nonce24 || !ct48 || !out_pt32) return -1;
  unsigned long long mlen = 0ULL;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(out_pt32, &mlen,
                                                  NULL,
                                                  ct48, 48,
                                                  ad, ad_len,
                                                  nonce24,
                                                  key32) != 0) {
    return -1;
  }
  return (mlen == 32ULL) ? 0 : -1;
}
