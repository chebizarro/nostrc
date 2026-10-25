#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nostr/nip44/nip44.h"
#include <nostr/nip44/kdf.h>

static void hex_to_bytes(const char *hex, unsigned char *out, size_t outlen) {
  for (size_t i = 0; i < outlen; i++) {
    unsigned int byte; sscanf(hex + 2*i, "%2x", &byte); out[i] = (unsigned char)byte;
  }
}

static void test_public_kdf(void) {
  /* RFC 5869, SHA-256 test case 1. */
  uint8_t ikm[22], salt[13], info[10], prk[32], okm[42], expected[42];
  memset(ikm, 0x0b, sizeof ikm);
  for (size_t i = 0; i < sizeof salt; ++i) salt[i] = (uint8_t)i;
  for (size_t i = 0; i < sizeof info; ++i) info[i] = (uint8_t)(0xf0 + i);
  assert(nostr_nip44_hkdf_extract(salt, sizeof salt, ikm, sizeof ikm, prk) == 0);
  hex_to_bytes("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", expected, 32);
  assert(memcmp(prk, expected, 32) == 0);
  assert(nostr_nip44_hkdf_expand(prk, info, sizeof info, okm, sizeof okm) == 0);
  hex_to_bytes("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", expected, 42);
  assert(memcmp(okm, expected, sizeof okm) == 0);

  /* RFC 4231 case 1, with the message split across both HMAC inputs. */
  assert(nostr_nip44_hmac_sha256(ikm, 20, (const uint8_t *)"Hi ", 3,
                                (const uint8_t *)"There", 5, prk) == 0);
  hex_to_bytes("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", expected, 32);
  assert(memcmp(prk, expected, 32) == 0);
  assert(nostr_nip44_hkdf_extract(NULL, 0, ikm, sizeof ikm, prk) == -1);
  assert(nostr_nip44_hkdf_expand(prk, NULL, 1, okm, sizeof okm) == -1);
  assert(nostr_nip44_hkdf_expand(prk, NULL, 0, okm, 0) == -1);
  assert(nostr_nip44_hkdf_expand(prk, NULL, 0, okm, 8161) == -1);
  assert(nostr_nip44_hmac_sha256(ikm, 20, NULL, 1, NULL, 0, prk) == -1);
}

int main(void) {
  test_public_kdf();
  unsigned char sk[32] = {0}; sk[31] = 1;
  const char *g_x_hex = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
  unsigned char pkx[32]; hex_to_bytes(g_x_hex, pkx, 32);
  unsigned char conv[32]; memset(conv, 0, sizeof conv);
  int rc = nostr_nip44_convkey(sk, pkx, conv);
  assert(rc == 0);
  /* Ensure convkey is not all-zero */
  int allzero = 1; for (int i=0;i<32;i++) if (conv[i]) { allzero = 0; break; }
  assert(!allzero);
  printf("test_nip44_convkey: OK\n");
  return 0;
}
