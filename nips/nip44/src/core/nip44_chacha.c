#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int nip44_chacha20_xor(const uint8_t key[32], const uint8_t nonce12[12],
                       const uint8_t *in, uint8_t *out, size_t len) {
  int ok = 0;
  int outl = 0, finl = 0;
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return -1;
  /* OpenSSL EVP_chacha20 expects a 16-byte IV: 4-byte counter (little-endian) + 12-byte nonce.
     NIP-44 uses the IETF convention with starting counter = 0 and a 12-byte nonce. */
  uint8_t iv[16] = {0};
  /* Start counter at 0 (little-endian) */
  iv[0] = 0; iv[1] = 0; iv[2] = 0; iv[3] = 0;
  memcpy(iv + 4, nonce12, 12);
  if (EVP_EncryptInit_ex(ctx, EVP_chacha20(), NULL, key, iv) != 1) goto done;
  if (EVP_EncryptUpdate(ctx, out, &outl, in, (int)len) != 1) goto done;
  if (EVP_EncryptFinal_ex(ctx, out + outl, &finl) != 1) goto done;
  ok = 1;
 done:
  EVP_CIPHER_CTX_free(ctx);
  return ok ? 0 : -1;
}
