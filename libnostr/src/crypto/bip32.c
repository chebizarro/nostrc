#include <nostr/crypto/bip32.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/bn.h>
#include <secp256k1.h>
#include <pthread.h>
#include <openssl/rand.h>
#include <string.h>
#include <stdlib.h>

/* Thread-safe global secp256k1 context */
static pthread_once_t g_secp_once = PTHREAD_ONCE_INIT;
static secp256k1_context *g_secp = NULL;
static void secp_init_once(void) {
  g_secp = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
  unsigned char seed[32];
  if (g_secp && RAND_bytes(seed, sizeof(seed)) == 1) {
    (void)secp256k1_context_randomize(g_secp, seed);
  }
  OPENSSL_cleanse(seed, sizeof(seed));
}
static inline secp256k1_context *secp_ctx(void) {
  (void)pthread_once(&g_secp_once, secp_init_once);
  return g_secp;
}

/* secp256k1 curve order n = FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE BAAEDCE6 AF48A03B BFD25E8C D0364141 */
static const unsigned char SECP256K1_N[32] = {
  0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFE,
  0xBA,0xAE,0xDC,0xE6,
  0xAF,0x48,0xA0,0x3B,
  0xBF,0xD2,0x5E,0x8C,
  0xD0,0x36,0x41,0x41
};

/* Serialize 32-bit big-endian */
static void ser32(uint32_t x, unsigned char out[4]) {
  out[0] = (unsigned char)((x >> 24) & 0xFF);
  out[1] = (unsigned char)((x >> 16) & 0xFF);
  out[2] = (unsigned char)((x >> 8) & 0xFF);
  out[3] = (unsigned char)(x & 0xFF);
}

static int bn_in_range_1_to_n_1(const BIGNUM *bn, const BIGNUM *n) {
  if (BN_is_zero(bn)) return 0;
  if (BN_is_negative(bn)) return 0;
  if (BN_cmp(bn, n) >= 0) return 0;
  return 1;
}

/* out = (a + b) mod n; return 1 on success, 0 on invalid (result zero) */
static int add_mod_n(const unsigned char a32[32], const unsigned char b32[32], unsigned char out32[32]) {
  int ok = 0;
  BN_CTX *ctx = BN_CTX_new();
  if (!ctx) return 0;
  BIGNUM *a = BN_bin2bn(a32, 32, NULL);
  BIGNUM *b = BN_bin2bn(b32, 32, NULL);
  BIGNUM *n = BN_bin2bn(SECP256K1_N, 32, NULL);
  BIGNUM *r = BN_new();
  if (!a || !b || !n || !r) goto done;
  if (!bn_in_range_1_to_n_1(a, n)) goto done;
  if (!bn_in_range_1_to_n_1(b, n)) goto done;
  if (!BN_mod_add(r, a, b, n, ctx)) goto done;
  if (BN_is_zero(r)) goto done;
  if (BN_bn2binpad(r, out32, 32) != 32) goto done;
  ok = 1;
 done:
  BN_free(a); BN_free(b); BN_free(n); BN_free(r); BN_CTX_free(ctx);
  return ok;
}

/* HMAC-SHA512 */
static int hmac_sha512(const unsigned char *key, size_t key_len,
                       const unsigned char *data, size_t data_len,
                       unsigned char out64[64]) {
  unsigned int len = 0;
  unsigned char *res = HMAC(EVP_sha512(), key, (int)key_len, data, data_len, out64, &len);
  return res != NULL && len == 64;
}

bool nostr_bip32_priv_from_master_seed(const uint8_t *seed, size_t seed_len, const uint32_t *path, size_t path_len, uint8_t out32[32]) {
  if (!seed || !out32) return false;
  unsigned char I[64];
  /* Master key: HMAC_SHA512("Bitcoin seed", seed) */
  static const unsigned char BITS_SEED[] = "Bitcoin seed";
  if (!hmac_sha512(BITS_SEED, sizeof("Bitcoin seed")-1, seed, seed_len, I)) return false;
  unsigned char k[32];
  unsigned char c[32];
  memcpy(k, I, 32);
  memcpy(c, I+32, 32);

  /* Check IL in 1..n-1 */
  {
    BIGNUM *n = BN_bin2bn(SECP256K1_N, 32, NULL);
    BIGNUM *il = BN_bin2bn(k, 32, NULL);
    int ok = n && il && bn_in_range_1_to_n_1(il, n);
    if (!ok) { BN_free(n); BN_free(il); return false; }
    BN_free(n); BN_free(il);
  }

  for (size_t i = 0; i < path_len; ++i) {
    uint32_t idx = path[i];
    unsigned char data[33 + 4];
    size_t data_len = 0;
    if (idx & 0x80000000u) {
      /* hardened: 0x00 || ser256(kpar) || ser32(i) */
      data[0] = 0x00;
      memcpy(&data[1], k, 32);
      ser32(idx, &data[33]);
      data_len = 1 + 32 + 4;
    } else {
      /* non-hardened: serP(point(kpar)) || ser32(i) */
      secp256k1_context *ctx = secp_ctx();
      if (!ctx) return false;
      secp256k1_pubkey pk;
      if (!secp256k1_ec_pubkey_create(ctx, &pk, k)) { return false; }
      size_t pub_len = 33;
      unsigned char pub[33];
      if (!secp256k1_ec_pubkey_serialize(ctx, pub, &pub_len, &pk, SECP256K1_EC_COMPRESSED)) { return false; }
      memcpy(data, pub, 33);
      ser32(idx, &data[33]);
      data_len = 33 + 4;
    }
    if (!hmac_sha512(c, 32, data, data_len, I)) return false;
    /* k' = (IL + k) mod n; c' = IR */
    if (!add_mod_n(k, I, k)) return false; /* I[0..31] is IL */
    memcpy(c, I+32, 32);
  }
  memcpy(out32, k, 32);
  OPENSSL_cleanse(I, sizeof(I));
  OPENSSL_cleanse((void*)k, sizeof(k));
  OPENSSL_cleanse((void*)c, sizeof(c));
  return true;
}
