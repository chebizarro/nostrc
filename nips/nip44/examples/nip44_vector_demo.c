#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "nostr/nip44/nip44.h"

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
extern char *nostr_key_get_public(const char *sk_hex);

static void hex_to_bytes(const char *hex, uint8_t *out, size_t outlen) {
  for (size_t i=0;i<outlen;i++){ unsigned int b=0; sscanf(hex+2*i, "%2x", &b); out[i]=(uint8_t)b; }
}

int main(void){
  /* Same example as test fallback */
  const char *sec1_hex = "0000000000000000000000000000000000000000000000000000000000000001";
  const char *sec2_hex = "0000000000000000000000000000000000000000000000000000000000000002";
  const char *conv_hex = "c41c775356fd92eadc63ff5a0dc1da211b268cbea22316767095b2871ea1412d";
  const char *nonce_hex= "0000000000000000000000000000000000000000000000000000000000000001";
  const char *plaintext = "a";
  const char *want_b64 = "AgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABee0G5VSK0/9YypIObAtDKfYEAjD35uVkHyB0F4DwrcNaCXlCWZKaArsGrY6M9wnuTMxWfp1RTN9Xga8no+kF5Vsb";

  char *pub2_hex = nostr_key_get_public(sec2_hex);
  assert(pub2_hex && strlen(pub2_hex)==64);

  uint8_t sk1[32], pk2x[32], conv[32], conv_expected[32];
  hex_to_bytes(sec1_hex, sk1, 32);
  hex_to_bytes(pub2_hex, pk2x, 32);
  hex_to_bytes(conv_hex, conv_expected, 32);
  int rc = nostr_nip44_convkey(sk1, pk2x, conv);
  assert(rc==0);
  assert(memcmp(conv, conv_expected, 32)==0);

  uint8_t nonce[32]; hex_to_bytes(nonce_hex, nonce, 32);
  uint8_t okm[76];
  nip44_hkdf_expand(conv, nonce, 32, okm, sizeof(okm));
  const uint8_t *ck = okm + 0;
  const uint8_t *cn = okm + 32;
  const uint8_t *hk = okm + 44;

  uint8_t *padded=NULL; size_t padded_len=0;
  rc = nip44_pad((const uint8_t*)plaintext, strlen(plaintext), &padded, &padded_len);
  assert(rc==0);
  uint8_t *cipher = (uint8_t*)malloc(padded_len);
  assert(cipher);
  rc = nip44_chacha20_xor(ck, cn, padded, cipher, padded_len);
  assert(rc==0);

  uint8_t mac[32];
  nip44_hmac_sha256(hk, 32, nonce, 32, cipher, padded_len, mac);

  size_t payload_len = 1 + 32 + padded_len + 32;
  uint8_t *payload = (uint8_t*)malloc(payload_len);
  assert(payload);
  size_t off=0; payload[off++] = (uint8_t)NOSTR_NIP44_V2;
  memcpy(payload+off, nonce, 32); off+=32;
  memcpy(payload+off, cipher, padded_len); off+=padded_len;
  memcpy(payload+off, mac, 32); off+=32;

  char *b64=NULL; rc = nip44_base64_encode(payload, payload_len, &b64);
  assert(rc==0 && b64);

  printf("vector_b64: %s\n", b64);
  if (strcmp(b64, want_b64) != 0) { fprintf(stderr, "mismatch\n"); return 1; }
  printf("OK\n");

  free(b64); free(payload); free(cipher); free(padded); free(pub2_hex);
  memset(okm,0,sizeof(okm)); memset(conv,0,sizeof(conv));
  return 0;
}
