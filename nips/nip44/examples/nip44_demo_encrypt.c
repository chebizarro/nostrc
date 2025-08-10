#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "nostr/nip44/nip44.h"

/* Optional helper from libnostr to derive x-only pub from secret */
extern char *nostr_key_get_public(const char *sk_hex);

/* Internal helpers for deterministic mode (nonce provided) */
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

static void usage(const char *prog){
  fprintf(stderr,
    "Usage: %s --sec1 <64-hex> (--pub2x <64-hex> | --sec2 <64-hex>) --msg <text> [--nonce <64-hex>]\n"
    "Outputs base64 payload and decrypts it back. If --nonce provided, uses deterministic path.\n",
    prog);
}

int main(int argc, char **argv){
  const char *sec1=NULL, *pub2x=NULL, *sec2=NULL, *msg=NULL, *nonce_hex=NULL;
  for (int i=1;i<argc;i++){
    if (!strcmp(argv[i], "--sec1") && i+1<argc) sec1=argv[++i];
    else if (!strcmp(argv[i], "--pub2x") && i+1<argc) pub2x=argv[++i];
    else if (!strcmp(argv[i], "--sec2") && i+1<argc) sec2=argv[++i];
    else if (!strcmp(argv[i], "--msg") && i+1<argc) msg=argv[++i];
    else if (!strcmp(argv[i], "--nonce") && i+1<argc) nonce_hex=argv[++i];
    else { usage(argv[0]); return 2; }
  }
  if (!sec1 || !msg || (!pub2x && !sec2)) { usage(argv[0]); return 2; }

  char *derived_pub = NULL;
  const char *use_pub = pub2x;
  if (!use_pub) { derived_pub = nostr_key_get_public(sec2); use_pub = derived_pub; }
  if (!use_pub || strlen(use_pub)!=64) { fprintf(stderr, "invalid pub2x\n"); free(derived_pub); return 2; }
  if (strlen(sec1)!=64) { fprintf(stderr, "invalid sec1\n"); free(derived_pub); return 2; }

  uint8_t sk1[32], pk2x[32], conv[32];
  hex_to_bytes(sec1, sk1, 32);
  hex_to_bytes(use_pub, pk2x, 32);
  if (nostr_nip44_convkey(sk1, pk2x, conv)!=0){ fprintf(stderr, "convkey failed\n"); free(derived_pub); return 1; }

  char *b64=NULL;
  if (nonce_hex && strlen(nonce_hex)==64) {
    /* Deterministic build to match vectors/test */
    uint8_t nonce[32]; hex_to_bytes(nonce_hex, nonce, 32);
    uint8_t okm[76];
    nip44_hkdf_expand(conv, nonce, 32, okm, sizeof(okm));
    const uint8_t *chacha_key = okm + 0;
    const uint8_t *chacha_nonce = okm + 32;
    const uint8_t *hmac_key = okm + 44;

    uint8_t *padded=NULL; size_t padded_len=0;
    if (nip44_pad((const uint8_t*)msg, strlen(msg), &padded, &padded_len)!=0){ free(derived_pub); return 1; }
    uint8_t *cipher = (uint8_t*)malloc(padded_len);
    if (!cipher){ free(padded); free(derived_pub); return 1; }
    if (nip44_chacha20_xor(chacha_key, chacha_nonce, padded, cipher, padded_len)!=0){ free(cipher); free(padded); free(derived_pub); return 1; }
    uint8_t mac[32];
    nip44_hmac_sha256(hmac_key, 32, nonce, 32, cipher, padded_len, mac);
    size_t payload_len = 1 + 32 + padded_len + 32;
    uint8_t *payload = (uint8_t*)malloc(payload_len);
    if (!payload){ free(cipher); free(padded); free(derived_pub); return 1; }
    size_t off=0; payload[off++] = (uint8_t)NOSTR_NIP44_V2;
    memcpy(payload+off, nonce, 32); off+=32;
    memcpy(payload+off, cipher, padded_len); off+=padded_len;
    memcpy(payload+off, mac, 32); off+=32;
    if (nip44_base64_encode(payload, payload_len, &b64)!=0){ free(payload); free(cipher); free(padded); free(derived_pub); return 1; }
    free(payload); free(cipher); free(padded);
  } else {
    if (nostr_nip44_encrypt_v2_with_convkey(conv, (const uint8_t*)msg, strlen(msg), &b64)!=0){ free(derived_pub); return 1; }
  }

  printf("payload_b64: %s\n", b64);

  uint8_t *plain=NULL; size_t plain_len=0;
  if (nostr_nip44_decrypt_v2_with_convkey(conv, b64, &plain, &plain_len)!=0){ fprintf(stderr, "decrypt failed\n"); free(b64); free(derived_pub); return 1; }
  printf("decrypted: %.*s\n", (int)plain_len, (const char*)plain);

  free(plain);
  free(b64);
  free(derived_pub);
  memset(conv,0,sizeof(conv));
  memset(sk1,0,sizeof(sk1));
  memset(pk2x,0,sizeof(pk2x));
  return 0;
}
