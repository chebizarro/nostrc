#include <stdint.h>
#include <string.h>
#include <openssl/crypto.h>
#include <secp256k1.h>
#include <secp256k1_ecdh.h>

static int ecdh_hash_xcopy(unsigned char *out, const unsigned char *x32,
                           const unsigned char *y32, void *data);

int nip44_hkdf_extract(const uint8_t *salt, size_t salt_len,
                       const uint8_t *ikm, size_t ikm_len,
                       uint8_t prk_out[32]);

int nostr_nip44_convkey(const uint8_t sender_sk[32],
                        const uint8_t receiver_pk_xonly[32],
                        uint8_t out_convkey[32]) {
  static const uint8_t salt[] = { 'n','i','p','4','4','-','v','2' };
  secp256k1_context *ctx = NULL;
  unsigned char comp[33] = {0};
  unsigned char shared_x[32] = {0};
  unsigned char derived[32] = {0};
  int rc = -1;

  if (!sender_sk || !receiver_pk_xonly || !out_convkey) return -1;

  ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
  if (!ctx || !secp256k1_ec_seckey_verify(ctx, sender_sk)) goto done;

  comp[0] = 0x02;
  memcpy(comp + 1, receiver_pk_xonly, 32);
  secp256k1_pubkey pub;
  if (!secp256k1_ec_pubkey_parse(ctx, &pub, comp, sizeof(comp))) {
    comp[0] = 0x03;
    if (!secp256k1_ec_pubkey_parse(ctx, &pub, comp, sizeof(comp))) goto done;
  }

  if (!secp256k1_ecdh(ctx, shared_x, &pub, sender_sk,
                      ecdh_hash_xcopy, NULL)) {
    goto done;
  }
  if (nip44_hkdf_extract(salt, sizeof(salt), shared_x, sizeof(shared_x),
                         derived) != 0) {
    goto done;
  }
  memcpy(out_convkey, derived, sizeof(derived));
  rc = 0;

done:
  secp256k1_context_destroy(ctx);
  OPENSSL_cleanse(shared_x, sizeof(shared_x));
  OPENSSL_cleanse(derived, sizeof(derived));
  OPENSSL_cleanse(comp, sizeof(comp));
  if (rc != 0) OPENSSL_cleanse(out_convkey, 32);
  return rc;
}

static int ecdh_hash_xcopy(unsigned char *out, const unsigned char *x32,
                           const unsigned char *y32, void *data) {
  (void)y32;
  (void)data;
  memcpy(out, x32, 32);
  return 1;
}
