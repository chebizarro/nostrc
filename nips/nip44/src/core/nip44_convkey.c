#include <stdint.h>
#include <string.h>
#include <openssl/crypto.h>
#include <secp256k1.h>
#include <secp256k1_ecdh.h>

/* Provide static implementation of ecdh_hash_xcopy for secp256k1_ecdh */
static int ecdh_hash_xcopy(unsigned char *out, const unsigned char *x32, const unsigned char *y32, void *data);

/* Use shared HKDF-Extract (EVP_MAC) from nip44_hkdf_hmac.c */
void nip44_hkdf_extract(const uint8_t *salt, size_t salt_len,
                        const uint8_t *ikm, size_t ikm_len,
                        uint8_t prk_out[32]);

int nostr_nip44_convkey(const uint8_t sender_sk[32],
                        const uint8_t receiver_pk_xonly[32],
                        uint8_t out_convkey[32]) {
  int rc = -1;
  secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
  if (!ctx) return -1;
  /* Verify secret */
  if (!secp256k1_ec_seckey_verify(ctx, sender_sk)) { secp256k1_context_destroy(ctx); return -1; }
  /* Build compressed pub from x-only assuming even Y (per x-only convention) */
  unsigned char comp[33]; comp[0] = 0x02; memcpy(comp + 1, receiver_pk_xonly, 32);
  secp256k1_pubkey pub;
  if (!secp256k1_ec_pubkey_parse(ctx, &pub, comp, sizeof(comp))) {
    /* Try odd parity as fallback */
    comp[0] = 0x03;
    if (!secp256k1_ec_pubkey_parse(ctx, &pub, comp, sizeof(comp))) { secp256k1_context_destroy(ctx); return -1; }
  }
  /* ECDH to get raw X coordinate */
  unsigned char x[32];
  if (!secp256k1_ecdh(ctx, x, &pub, sender_sk, ecdh_hash_xcopy, NULL)) { secp256k1_context_destroy(ctx); return -1; }
  secp256k1_context_destroy(ctx);
  /* HKDF-Extract with salt "nip44-v2" */
  static const uint8_t salt[] = { 'n','i','p','4','4','-','v','2' };
  nip44_hkdf_extract(salt, sizeof(salt), x, 32, out_convkey);
  OPENSSL_cleanse(x, sizeof(x));
  rc = 0;
  return rc;
}

/* Provide static implementation of ecdh_hash_xcopy for secp256k1_ecdh */
static int ecdh_hash_xcopy(unsigned char *out, const unsigned char *x32, const unsigned char *y32, void *data) {
  (void)y32; (void)data; memcpy(out, x32, 32); return 1;
}
