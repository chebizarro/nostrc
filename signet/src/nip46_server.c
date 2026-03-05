/* SPDX-License-Identifier: MIT
 *
 * nip46_server.c - NIP-46 request handling (Phase 6).
 *
 * Implements a NIP-46 bunker request handler with:
 * - Replay protection via SignetReplayCache
 * - NIP-04 encrypt/decrypt (ECDH(secp256k1) + AES-256-CBC) for request/response
 * - Policy gating via SignetPolicyEngine
 * - Supported methods: sign_event, get_public_key, nip04_encrypt, nip04_decrypt,
 *   get_relays, ping
 * - Outer response event creation/signing (NIP-01 event id + BIP340 schnorr sig)
 * - Audit logging (JSONL) without secret material or plaintext leakage
 *
 * Notes:
 * - This file deliberately avoids depending on internal libnostr event structs.
 * - It signs events using a minimal BIP340 implementation (OpenSSL bignum/EC).
 * - It does not implement NIP-46 session/connect state machine in this phase.
 */

#include "signet/nip46_server.h"

#include "signet/relay_pool.h"
#include "signet/policy_engine.h"
#include "signet/key_store.h"
#include "signet/replay_cache.h"
#include "signet/audit_logger.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <json-glib/json-glib.h>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#define SIGNET_NIP46_KIND_CIPHERTEXT 24133

/* ------------------------------ small helpers ----------------------------- */

static void signet_memzero(void *p, size_t n) {
  if (p && n) OPENSSL_cleanse(p, n);
}

static char *signet_strdup0(const char *s) {
  if (!s || s[0] == '\0') return NULL;
  return g_strdup(s);
}

static gboolean signet_streq0(const char *a, const char *b) {
  if (!a || !b) return FALSE;
  return strcmp(a, b) == 0;
}

static gboolean signet_is_hex_char(char c) {
  return ((c >= '0' && c <= '9') ||
          (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'));
}

static int signet_hex_nibble(char c) {
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
  if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
  return -1;
}

static bool signet_hex_decode(const char *hex, uint8_t *out, size_t out_len) {
  if (!hex || !out) return false;
  size_t hex_len = strlen(hex);
  if (hex_len != out_len * 2) return false;

  for (size_t i = 0; i < out_len; i++) {
    int hi = signet_hex_nibble(hex[i * 2]);
    int lo = signet_hex_nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static char *signet_hex_encode(const uint8_t *in, size_t in_len) {
  static const char *hex = "0123456789abcdef";
  if (!in) return NULL;

  char *out = g_malloc0(in_len * 2 + 1);
  if (!out) return NULL;

  for (size_t i = 0; i < in_len; i++) {
    out[i * 2] = hex[(in[i] >> 4) & 0xF];
    out[i * 2 + 1] = hex[in[i] & 0xF];
  }
  out[in_len * 2] = '\0';
  return out;
}

static void signet_sha256(const uint8_t *in, size_t in_len, uint8_t out32[32]) {
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, in, in_len);
  SHA256_Final(out32, &ctx);
}

static void signet_tagged_hash(const char *tag, const uint8_t *msg, size_t msg_len, uint8_t out32[32]) {
  /* BIP340 tagged hash: SHA256(SHA256(tag)||SHA256(tag)||msg) */
  uint8_t tag_hash[32];
  signet_sha256((const uint8_t *)tag, strlen(tag), tag_hash);

  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, tag_hash, sizeof(tag_hash));
  SHA256_Update(&ctx, tag_hash, sizeof(tag_hash));
  SHA256_Update(&ctx, msg, msg_len);
  SHA256_Final(out32, &ctx);

  signet_memzero(tag_hash, sizeof(tag_hash));
}

/* ------------------------------ secp256k1 EC ------------------------------ */

static EC_GROUP *signet_secp256k1_group(void) {
  static gsize once = 0;
  static EC_GROUP *group = NULL;

  if (g_once_init_enter(&once)) {
    EC_GROUP *g = EC_GROUP_new_by_curve_name(NID_secp256k1);
    if (g) {
      EC_GROUP_set_asn1_flag(g, OPENSSL_EC_NAMED_CURVE);
      EC_GROUP_set_point_conversion_form(g, POINT_CONVERSION_COMPRESSED);
    }
    group = g;
    g_once_init_leave(&once, 1);
  }

  return group;
}

static bool signet_bn_to_32be(const BIGNUM *bn, uint8_t out32[32]) {
  if (!bn || !out32) return false;
  int n = BN_num_bytes(bn);
  if (n < 0 || n > 32) return false;

  memset(out32, 0, 32);
  if (n == 0) return true;

  uint8_t tmp[32];
  memset(tmp, 0, sizeof(tmp));
  BN_bn2binpad(bn, tmp, 32);
  memcpy(out32, tmp, 32);
  signet_memzero(tmp, sizeof(tmp));
  return true;
}

static bool signet_bytes_to_bn32(const uint8_t in32[32], BIGNUM *out) {
  if (!in32 || !out) return false;
  BIGNUM *r = BN_bin2bn(in32, 32, out);
  return (r == out);
}

static EC_POINT *signet_point_from_xonly_even_y(const EC_GROUP *group,
                                                const uint8_t x32[32],
                                                BN_CTX *ctx,
                                                char **out_err) {
  if (out_err) *out_err = NULL;
  if (!group || !x32 || !ctx) {
    if (out_err) *out_err = g_strdup("internal error: missing curve parameters");
    return NULL;
  }

  BIGNUM *x = BN_new();
  BIGNUM *y = BN_new();
  BIGNUM *p = BN_new();
  BIGNUM *a = BN_new();
  BIGNUM *b = BN_new();
  BIGNUM *rhs = BN_new();
  BIGNUM *tmp = BN_new();

  if (!x || !y || !p || !a || !b || !rhs || !tmp) {
    if (out_err) *out_err = g_strdup("OOM");
    goto fail;
  }

  if (!EC_GROUP_get_curve(group, p, a, b, ctx)) {
    if (out_err) *out_err = g_strdup("failed to get curve");
    goto fail;
  }

  if (!signet_bytes_to_bn32(x32, x)) {
    if (out_err) *out_err = g_strdup("invalid pubkey");
    goto fail;
  }

  /* rhs = x^3 + 7 mod p (secp256k1: y^2 = x^3 + 7) */
  if (!BN_mod_sqr(tmp, x, p, ctx)) goto fail;
  if (!BN_mod_mul(rhs, tmp, x, p, ctx)) goto fail; /* x^3 */
  if (!BN_add(rhs, rhs, b)) goto fail;
  if (!BN_mod(rhs, rhs, p, ctx)) goto fail;

  if (!BN_mod_sqrt(y, rhs, p, ctx)) {
    if (out_err) *out_err = g_strdup("invalid pubkey (no sqrt)");
    goto fail;
  }

  /* enforce even y */
  if (BN_is_odd(y)) {
    if (!BN_sub(y, p, y)) goto fail;
  }

  EC_POINT *pt = EC_POINT_new(group);
  if (!pt) {
    if (out_err) *out_err = g_strdup("OOM");
    goto fail;
  }

  if (!EC_POINT_set_affine_coordinates(group, pt, x, y, ctx)) {
    EC_POINT_free(pt);
    if (out_err) *out_err = g_strdup("invalid pubkey (point set failed)");
    goto fail;
  }

  BN_free(x);
  BN_free(y);
  BN_free(p);
  BN_free(a);
  BN_free(b);
  BN_free(rhs);
  BN_free(tmp);
  return pt;

fail:
  BN_free(x);
  BN_free(y);
  BN_free(p);
  BN_free(a);
  BN_free(b);
  BN_free(rhs);
  BN_free(tmp);
  return NULL;
}

static bool signet_pubkey_xonly_from_seckey32(const EC_GROUP *group,
                                              const uint8_t sk32[32],
                                              uint8_t out_pk32[32],
                                              bool *out_y_even) {
  if (!group || !sk32 || !out_pk32) return false;

  bool ok = false;
  BN_CTX *ctx = BN_CTX_new();
  BIGNUM *d = BN_new();
  BIGNUM *x = BN_new();
  BIGNUM *y = BN_new();
  EC_POINT *P = NULL;

  if (!ctx || !d || !x || !y) goto done;

  if (!signet_bytes_to_bn32(sk32, d)) goto done;

  P = EC_POINT_new(group);
  if (!P) goto done;

  if (!EC_POINT_mul(group, P, d, NULL, NULL, ctx)) goto done;
  if (!EC_POINT_get_affine_coordinates(group, P, x, y, ctx)) goto done;

  if (!signet_bn_to_32be(x, out_pk32)) goto done;

  if (out_y_even) *out_y_even = BN_is_odd(y) ? false : true;

  ok = true;

done:
  EC_POINT_free(P);
  BN_free(d);
  BN_free(x);
  BN_free(y);
  BN_CTX_free(ctx);
  return ok;
}

/* ----------------------------- BIP340 schnorr ----------------------------- */

static bool signet_bip340_sign(const uint8_t seckey32_in[32],
                               const uint8_t msg32[32],
                               uint8_t out_sig64[64],
                               uint8_t out_pubkey32[32],
                               char **out_err) {
  if (out_err) *out_err = NULL;
  if (!seckey32_in || !msg32 || !out_sig64 || !out_pubkey32) {
    if (out_err) *out_err = g_strdup("internal error");
    return false;
  }

  const EC_GROUP *group = signet_secp256k1_group();
  if (!group) {
    if (out_err) *out_err = g_strdup("secp256k1 unavailable");
    return false;
  }

  bool ok = false;
  BN_CTX *ctx = BN_CTX_new();
  BIGNUM *n = BN_new();
  BIGNUM *d = BN_new();
  BIGNUM *k = BN_new();
  BIGNUM *e = BN_new();
  BIGNUM *r_bn = BN_new();
  BIGNUM *x = BN_new();
  BIGNUM *y = BN_new();
  EC_POINT *P = NULL;
  EC_POINT *R = NULL;

  uint8_t aux[32];
  uint8_t d_bytes[32];
  uint8_t pk32[32];
  uint8_t nonce_hash[32];
  uint8_t chall_hash[32];

  memset(aux, 0, sizeof(aux));
  memset(d_bytes, 0, sizeof(d_bytes));
  memset(pk32, 0, sizeof(pk32));
  memset(nonce_hash, 0, sizeof(nonce_hash));
  memset(chall_hash, 0, sizeof(chall_hash));

  if (!ctx || !n || !d || !k || !e || !r_bn || !x || !y) goto done;
  if (!EC_GROUP_get_order(group, n, ctx)) goto done;

  if (!signet_hex_decode("00", NULL, 0)) { /* no-op for static analyzers */ }

  /* d = int(seckey) */
  if (!signet_bytes_to_bn32(seckey32_in, d)) {
    if (out_err) *out_err = g_strdup("invalid secret key");
    goto done;
  }
  if (BN_is_zero(d) || BN_is_negative(d) || BN_cmp(d, n) >= 0) {
    if (out_err) *out_err = g_strdup("invalid secret key range");
    goto done;
  }

  /* P = d*G, derive x-only pubkey; if y odd, d = n - d (BIP340 key normalization) */
  P = EC_POINT_new(group);
  if (!P) goto done;
  if (!EC_POINT_mul(group, P, d, NULL, NULL, ctx)) goto done;
  if (!EC_POINT_get_affine_coordinates(group, P, x, y, ctx)) goto done;

  if (!signet_bn_to_32be(x, pk32)) goto done;

  if (BN_is_odd(y)) {
    /* d = n - d */
    if (!BN_sub(d, n, d)) goto done;
    /* recompute P not required for x-only; keep pk32 (x unchanged) */
  }

  memcpy(out_pubkey32, pk32, 32);

  if (!signet_bn_to_32be(d, d_bytes)) goto done;

  if (RAND_bytes(aux, (int)sizeof(aux)) != 1) {
    /* If RNG fails, fall back to deterministic aux from msg (still avoids logging secrets). */
    signet_sha256(msg32, 32, aux);
  }

  /* nonce = int(tagged_hash("BIP0340/nonce", (d_bytes XOR tagged_hash("BIP0340/aux", aux)) || pk || msg)) mod n */
  uint8_t aux_hash[32];
  signet_tagged_hash("BIP0340/aux", aux, 32, aux_hash);

  uint8_t t[32];
  for (size_t i = 0; i < 32; i++) t[i] = (uint8_t)(d_bytes[i] ^ aux_hash[i]);

  uint8_t nonce_input[32 + 32 + 32];
  memcpy(nonce_input, t, 32);
  memcpy(nonce_input + 32, pk32, 32);
  memcpy(nonce_input + 64, msg32, 32);

  signet_tagged_hash("BIP0340/nonce", nonce_input, sizeof(nonce_input), nonce_hash);

  if (!signet_bytes_to_bn32(nonce_hash, k)) goto done;
  if (!BN_mod(k, k, n, ctx)) goto done;
  if (BN_is_zero(k)) {
    if (out_err) *out_err = g_strdup("nonce generation failed");
    goto done;
  }

  /* R = k*G; if y_R odd, k = n - k */
  R = EC_POINT_new(group);
  if (!R) goto done;
  if (!EC_POINT_mul(group, R, k, NULL, NULL, ctx)) goto done;
  if (!EC_POINT_get_affine_coordinates(group, R, x, y, ctx)) goto done;

  if (BN_is_odd(y)) {
    if (!BN_sub(k, n, k)) goto done;
    /* x unchanged */
  }

  if (!BN_copy(r_bn, x)) goto done;

  uint8_t r32[32];
  if (!signet_bn_to_32be(r_bn, r32)) goto done;

  /* e = int(tagged_hash("BIP0340/challenge", r || pk || msg)) mod n */
  uint8_t chall_input[32 + 32 + 32];
  memcpy(chall_input, r32, 32);
  memcpy(chall_input + 32, pk32, 32);
  memcpy(chall_input + 64, msg32, 32);

  signet_tagged_hash("BIP0340/challenge", chall_input, sizeof(chall_input), chall_hash);

  if (!signet_bytes_to_bn32(chall_hash, e)) goto done;
  if (!BN_mod(e, e, n, ctx)) goto done;

  /* s = (k + e*d) mod n */
  if (!BN_mod_mul(e, e, d, n, ctx)) goto done; /* reuse e as e*d */
  if (!BN_mod_add(e, e, k, n, ctx)) goto done; /* reuse e as s */

  uint8_t s32[32];
  if (!signet_bn_to_32be(e, s32)) goto done;

  memcpy(out_sig64, r32, 32);
  memcpy(out_sig64 + 32, s32, 32);

  signet_memzero(r32, sizeof(r32));
  signet_memzero(s32, sizeof(s32));

  ok = true;

done:
  signet_memzero(aux, sizeof(aux));
  signet_memzero(d_bytes, sizeof(d_bytes));
  signet_memzero(nonce_hash, sizeof(nonce_hash));
  signet_memzero(chall_hash, sizeof(chall_hash));

  BN_free(n);
  BN_free(d);
  BN_free(k);
  BN_free(e);
  BN_free(r_bn);
  BN_free(x);
  BN_free(y);
  EC_POINT_free(P);
  EC_POINT_free(R);
  BN_CTX_free(ctx);

  if (!ok && out_err && !*out_err) *out_err = g_strdup("signing failed");
  return ok;
}

/* ----------------------------- NIP-04 helpers ----------------------------- */

static bool signet_nip04_derive_aes_key(const char *our_sk_hex,
                                       const char *their_pubkey_hex,
                                       uint8_t out_key32[32],
                                       char **out_err) {
  if (out_err) *out_err = NULL;
  if (!our_sk_hex || !their_pubkey_hex || !out_key32) {
    if (out_err) *out_err = g_strdup("internal error");
    return false;
  }

  if (strlen(our_sk_hex) != 64 || strlen(their_pubkey_hex) != 64) {
    if (out_err) *out_err = g_strdup("invalid key length");
    return false;
  }
  for (size_t i = 0; i < 64; i++) {
    if (!signet_is_hex_char(our_sk_hex[i]) || !signet_is_hex_char(their_pubkey_hex[i])) {
      if (out_err) *out_err = g_strdup("invalid hex key");
      return false;
    }
  }

  const EC_GROUP *group = signet_secp256k1_group();
  if (!group) {
    if (out_err) *out_err = g_strdup("secp256k1 unavailable");
    return false;
  }

  uint8_t sk32[32];
  uint8_t pkx32[32];
  memset(sk32, 0, sizeof(sk32));
  memset(pkx32, 0, sizeof(pkx32));

  if (!signet_hex_decode(our_sk_hex, sk32, 32)) {
    if (out_err) *out_err = g_strdup("invalid secret key");
    signet_memzero(sk32, sizeof(sk32));
    return false;
  }
  if (!signet_hex_decode(their_pubkey_hex, pkx32, 32)) {
    if (out_err) *out_err = g_strdup("invalid pubkey");
    signet_memzero(sk32, sizeof(sk32));
    signet_memzero(pkx32, sizeof(pkx32));
    return false;
  }

  BN_CTX *ctx = BN_CTX_new();
  BIGNUM *d = BN_new();
  BIGNUM *sx = BN_new();
  BIGNUM *sy = BN_new();
  EC_POINT *Q = NULL;
  EC_POINT *S = NULL;

  bool ok = false;

  if (!ctx || !d || !sx || !sy) {
    if (out_err) *out_err = g_strdup("OOM");
    goto done;
  }

  if (!signet_bytes_to_bn32(sk32, d)) {
    if (out_err) *out_err = g_strdup("invalid secret key");
    goto done;
  }

  char *perr = NULL;
  Q = signet_point_from_xonly_even_y(group, pkx32, ctx, &perr);
  if (!Q) {
    if (out_err) *out_err = perr ? perr : g_strdup("invalid pubkey");
    else g_free(perr);
    goto done;
  }

  S = EC_POINT_new(group);
  if (!S) {
    if (out_err) *out_err = g_strdup("OOM");
    goto done;
  }

  if (!EC_POINT_mul(group, S, NULL, Q, d, ctx)) {
    if (out_err) *out_err = g_strdup("ECDH failed");
    goto done;
  }

  if (!EC_POINT_get_affine_coordinates(group, S, sx, sy, ctx)) {
    if (out_err) *out_err = g_strdup("ECDH failed");
    goto done;
  }

  uint8_t shared_x32[32];
  memset(shared_x32, 0, sizeof(shared_x32));
  if (!signet_bn_to_32be(sx, shared_x32)) {
    if (out_err) *out_err = g_strdup("ECDH failed");
    goto done;
  }

  signet_sha256(shared_x32, 32, out_key32);

  signet_memzero(shared_x32, sizeof(shared_x32));
  ok = true;

done:
  signet_memzero(sk32, sizeof(sk32));
  signet_memzero(pkx32, sizeof(pkx32));

  EC_POINT_free(Q);
  EC_POINT_free(S);
  BN_free(d);
  BN_free(sx);
  BN_free(sy);
  BN_CTX_free(ctx);

  if (!ok && out_err && !*out_err) *out_err = g_strdup("ECDH failed");
  return ok;
}

static bool signet_aes256cbc_encrypt_b64(const uint8_t key32[32],
                                        const uint8_t iv16[16],
                                        const uint8_t *plaintext,
                                        size_t plaintext_len,
                                        char **out_b64_cipher,
                                        char **out_err) {
  if (out_err) *out_err = NULL;
  if (out_b64_cipher) *out_b64_cipher = NULL;

  if (!key32 || !iv16 || (!plaintext && plaintext_len != 0) || !out_b64_cipher) {
    if (out_err) *out_err = g_strdup("internal error");
    return false;
  }

  bool ok = false;
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    if (out_err) *out_err = g_strdup("OOM");
    return false;
  }

  int outl1 = 0;
  int outl2 = 0;
  size_t cap = plaintext_len + 32; /* padding */
  uint8_t *cipher = g_malloc0(cap);
  if (!cipher) {
    if (out_err) *out_err = g_strdup("OOM");
    goto done;
  }

  if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key32, iv16) != 1) {
    if (out_err) *out_err = g_strdup("encrypt init failed");
    goto done;
  }

  if (EVP_EncryptUpdate(ctx, cipher, &outl1, plaintext, (int)plaintext_len) != 1) {
    if (out_err) *out_err = g_strdup("encrypt failed");
    goto done;
  }

  if (EVP_EncryptFinal_ex(ctx, cipher + outl1, &outl2) != 1) {
    if (out_err) *out_err = g_strdup("encrypt final failed");
    goto done;
  }

  {
    gsize b64_len = 0;
    char *b64 = g_base64_encode(cipher, (gsize)(outl1 + outl2));
    (void)b64_len;
    if (!b64) {
      if (out_err) *out_err = g_strdup("base64 failed");
      goto done;
    }
    *out_b64_cipher = b64;
  }

  ok = true;

done:
  if (cipher) {
    signet_memzero(cipher, cap);
    g_free(cipher);
  }
  EVP_CIPHER_CTX_free(ctx);

  if (!ok && out_err && !*out_err) *out_err = g_strdup("encrypt failed");
  return ok;
}

static bool signet_aes256cbc_decrypt_b64(const uint8_t key32[32],
                                        const uint8_t iv16[16],
                                        const char *b64_cipher,
                                        uint8_t **out_plain,
                                        size_t *out_plain_len,
                                        char **out_err) {
  if (out_err) *out_err = NULL;
  if (out_plain) *out_plain = NULL;
  if (out_plain_len) *out_plain_len = 0;

  if (!key32 || !iv16 || !b64_cipher || !out_plain || !out_plain_len) {
    if (out_err) *out_err = g_strdup("internal error");
    return false;
  }

  bool ok = false;

  gsize cipher_len = 0;
  guchar *cipher = g_base64_decode(b64_cipher, &cipher_len);
  if (!cipher || cipher_len == 0) {
    if (out_err) *out_err = g_strdup("invalid base64");
    if (cipher) g_free(cipher);
    return false;
  }

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    if (out_err) *out_err = g_strdup("OOM");
    goto done;
  }

  uint8_t *plain = g_malloc0(cipher_len + 32);
  if (!plain) {
    if (out_err) *out_err = g_strdup("OOM");
    goto done;
  }

  int outl1 = 0;
  int outl2 = 0;

  if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key32, iv16) != 1) {
    if (out_err) *out_err = g_strdup("decrypt init failed");
    goto done;
  }

  if (EVP_DecryptUpdate(ctx, plain, &outl1, cipher, (int)cipher_len) != 1) {
    if (out_err) *out_err = g_strdup("decrypt failed");
    goto done;
  }

  if (EVP_DecryptFinal_ex(ctx, plain + outl1, &outl2) != 1) {
    if (out_err) *out_err = g_strdup("decrypt final failed");
    goto done;
  }

  *out_plain = plain;
  *out_plain_len = (size_t)(outl1 + outl2);
  plain = NULL;
  ok = true;

done:
  if (ctx) EVP_CIPHER_CTX_free(ctx);
  if (cipher) {
    signet_memzero(cipher, cipher_len);
    g_free(cipher);
  }
  if (!ok && out_plain && *out_plain) {
    signet_memzero(*out_plain, *out_plain_len);
    g_free(*out_plain);
    *out_plain = NULL;
    *out_plain_len = 0;
  }

  if (!ok && out_err && !*out_err) *out_err = g_strdup("decrypt failed");
  return ok;
}

static bool signet_nip04_decrypt(const char *our_sk_hex,
                                const char *their_pubkey_hex,
                                const char *ciphertext,
                                char **out_plaintext,
                                char **out_err) {
  if (out_err) *out_err = NULL;
  if (out_plaintext) *out_plaintext = NULL;

  if (!our_sk_hex || !their_pubkey_hex || !ciphertext || !out_plaintext) {
    if (out_err) *out_err = g_strdup("internal error");
    return false;
  }

  const char *q = strstr(ciphertext, "?iv=");
  if (!q) {
    if (out_err) *out_err = g_strdup("invalid ciphertext format (missing iv)");
    return false;
  }

  char *b64_cipher = g_strndup(ciphertext, (gsize)(q - ciphertext));
  char *b64_iv = g_strdup(q + 4);
  if (!b64_cipher || !b64_iv) {
    g_free(b64_cipher);
    g_free(b64_iv);
    if (out_err) *out_err = g_strdup("OOM");
    return false;
  }

  gsize iv_len = 0;
  guchar *iv = g_base64_decode(b64_iv, &iv_len);
  if (!iv || iv_len != 16) {
    if (out_err) *out_err = g_strdup("invalid iv");
    if (iv) g_free(iv);
    g_free(b64_cipher);
    g_free(b64_iv);
    return false;
  }

  uint8_t key32[32];
  memset(key32, 0, sizeof(key32));
  char *kerr = NULL;
  if (!signet_nip04_derive_aes_key(our_sk_hex, their_pubkey_hex, key32, &kerr)) {
    if (out_err) *out_err = kerr ? kerr : g_strdup("key derivation failed");
    else g_free(kerr);
    signet_memzero(key32, sizeof(key32));
    signet_memzero(iv, iv_len);
    g_free(iv);
    g_free(b64_cipher);
    g_free(b64_iv);
    return false;
  }
  g_free(kerr);

  uint8_t *plain = NULL;
  size_t plain_len = 0;
  char *derr = NULL;
  bool ok = signet_aes256cbc_decrypt_b64(key32, (const uint8_t *)iv, b64_cipher, &plain, &plain_len, &derr);

  signet_memzero(key32, sizeof(key32));
  signet_memzero(iv, iv_len);
  g_free(iv);
  g_free(b64_cipher);
  g_free(b64_iv);

  if (!ok) {
    if (out_err) *out_err = derr ? derr : g_strdup("decrypt failed");
    else g_free(derr);
    if (plain) {
      signet_memzero(plain, plain_len);
      g_free(plain);
    }
    return false;
  }
  g_free(derr);

  /* Ensure nul-terminated string */
  char *s = g_malloc0(plain_len + 1);
  if (!s) {
    signet_memzero(plain, plain_len);
    g_free(plain);
    if (out_err) *out_err = g_strdup("OOM");
    return false;
  }
  memcpy(s, plain, plain_len);
  s[plain_len] = '\0';

  signet_memzero(plain, plain_len);
  g_free(plain);

  *out_plaintext = s;
  return true;
}

static bool signet_nip04_encrypt(const char *our_sk_hex,
                                const char *their_pubkey_hex,
                                const char *plaintext,
                                char **out_ciphertext,
                                char **out_err) {
  if (out_err) *out_err = NULL;
  if (out_ciphertext) *out_ciphertext = NULL;

  if (!our_sk_hex || !their_pubkey_hex || !plaintext || !out_ciphertext) {
    if (out_err) *out_err = g_strdup("internal error");
    return false;
  }

  uint8_t key32[32];
  memset(key32, 0, sizeof(key32));

  char *kerr = NULL;
  if (!signet_nip04_derive_aes_key(our_sk_hex, their_pubkey_hex, key32, &kerr)) {
    if (out_err) *out_err = kerr ? kerr : g_strdup("key derivation failed");
    else g_free(kerr);
    signet_memzero(key32, sizeof(key32));
    return false;
  }
  g_free(kerr);

  uint8_t iv16[16];
  memset(iv16, 0, sizeof(iv16));
  if (RAND_bytes(iv16, (int)sizeof(iv16)) != 1) {
    /* If RNG fails, derive a deterministic iv from plaintext hash (not ideal, but avoids hard failure). */
    uint8_t h[32];
    signet_sha256((const uint8_t *)plaintext, strlen(plaintext), h);
    memcpy(iv16, h, 16);
    signet_memzero(h, sizeof(h));
  }

  char *b64_cipher = NULL;
  char *eerr = NULL;
  bool ok = signet_aes256cbc_encrypt_b64(key32, iv16,
                                         (const uint8_t *)plaintext, strlen(plaintext),
                                         &b64_cipher, &eerr);
  signet_memzero(key32, sizeof(key32));
  if (!ok) {
    if (out_err) *out_err = eerr ? eerr : g_strdup("encrypt failed");
    else g_free(eerr);
    signet_memzero(iv16, sizeof(iv16));
    return false;
  }
  g_free(eerr);

  char *b64_iv = g_base64_encode(iv16, sizeof(iv16));
  signet_memzero(iv16, sizeof(iv16));
  if (!b64_iv) {
    if (out_err) *out_err = g_strdup("base64 failed");
    g_free(b64_cipher);
    return false;
  }

  char *out = g_strdup_printf("%s?iv=%s", b64_cipher, b64_iv);
  g_free(b64_cipher);
  g_free(b64_iv);

  if (!out) {
    if (out_err) *out_err = g_strdup("OOM");
    return false;
  }

  *out_ciphertext = out;
  return true;
}

/* -------------------------- NIP-46 request parsing ------------------------ */

typedef struct {
  char *id;
  char *method;
  GPtrArray *params; /* array of (char*) */
} SignetNip46Request;

static void signet_nip46_request_clear(SignetNip46Request *r) {
  if (!r) return;
  g_clear_pointer(&r->id, g_free);
  g_clear_pointer(&r->method, g_free);
  if (r->params) {
    for (guint i = 0; i < r->params->len; i++) {
      g_free(g_ptr_array_index(r->params, i));
    }
    g_ptr_array_free(r->params, TRUE);
    r->params = NULL;
  }
}

static bool signet_json_get_string_member(JsonObject *o, const char *name, const char **out) {
  if (!o || !name || !out) return false;
  *out = NULL;
  if (!json_object_has_member(o, name)) return false;
  *out = json_object_get_string_member(o, name);
  return (*out != NULL);
}

static bool signet_nip46_parse_request_json(const char *json,
                                            SignetNip46Request *out_req,
                                            char **out_err) {
  if (out_err) *out_err = NULL;
  if (!json || !out_req) {
    if (out_err) *out_err = g_strdup("internal error");
    return false;
  }

  memset(out_req, 0, sizeof(*out_req));

  JsonParser *p = json_parser_new();
  if (!p) {
    if (out_err) *out_err = g_strdup("OOM");
    return false;
  }

  GError *gerr = NULL;
  if (!json_parser_load_from_data(p, json, -1, &gerr)) {
    if (out_err) *out_err = g_strdup(gerr && gerr->message ? gerr->message : "invalid JSON");
    if (gerr) g_error_free(gerr);
    g_object_unref(p);
    return false;
  }

  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    if (out_err) *out_err = g_strdup("request must be a JSON object");
    g_object_unref(p);
    return false;
  }

  JsonObject *o = json_node_get_object(root);
  const char *id = NULL;
  const char *method = NULL;

  if (!signet_json_get_string_member(o, "id", &id) || !id || id[0] == '\0') {
    if (out_err) *out_err = g_strdup("missing request id");
    g_object_unref(p);
    return false;
  }
  if (!signet_json_get_string_member(o, "method", &method) || !method || method[0] == '\0') {
    if (out_err) *out_err = g_strdup("missing method");
    g_object_unref(p);
    return false;
  }

  out_req->id = g_strdup(id);
  out_req->method = g_strdup(method);
  out_req->params = g_ptr_array_new_with_free_func(g_free);
  if (!out_req->id || !out_req->method || !out_req->params) {
    if (out_err) *out_err = g_strdup("OOM");
    signet_nip46_request_clear(out_req);
    g_object_unref(p);
    return false;
  }

  if (json_object_has_member(o, "params")) {
    JsonNode *pn = json_object_get_member(o, "params");
    if (!pn || !JSON_NODE_HOLDS_ARRAY(pn)) {
      if (out_err) *out_err = g_strdup("params must be an array");
      signet_nip46_request_clear(out_req);
      g_object_unref(p);
      return false;
    }
    JsonArray *a = json_node_get_array(pn);
    for (guint i = 0; i < json_array_get_length(a); i++) {
      const char *sv = json_array_get_string_element(a, i);
      if (!sv) {
        if (out_err) *out_err = g_strdup("params elements must be strings");
        signet_nip46_request_clear(out_req);
        g_object_unref(p);
        return false;
      }
      g_ptr_array_add(out_req->params, g_strdup(sv));
    }
  }

  g_object_unref(p);
  return true;
}

/* ------------------------------ event signing ----------------------------- */

static bool signet_build_event_serialization_json(const char *pubkey_hex,
                                                  int64_t created_at,
                                                  int kind,
                                                  JsonArray *tags,
                                                  const char *content,
                                                  char **out_ser_json,
                                                  char **out_err) {
  if (out_err) *out_err = NULL;
  if (out_ser_json) *out_ser_json = NULL;

  if (!pubkey_hex || !content || !out_ser_json) {
    if (out_err) *out_err = g_strdup("internal error");
    return false;
  }

  JsonBuilder *b = json_builder_new();
  if (!b) {
    if (out_err) *out_err = g_strdup("OOM");
    return false;
  }

  json_builder_begin_array(b);

  json_builder_add_int_value(b, 0);
  json_builder_add_string_value(b, pubkey_hex);
  json_builder_add_int_value(b, (gint64)created_at);
  json_builder_add_int_value(b, (gint64)kind);

  /* tags */
  json_builder_begin_array(b);
  if (tags) {
    for (guint i = 0; i < json_array_get_length(tags); i++) {
      JsonNode *tn = json_array_get_element(tags, i);
      if (!tn || !JSON_NODE_HOLDS_ARRAY(tn)) continue;
      JsonArray *ta = json_node_get_array(tn);

      json_builder_begin_array(b);
      for (guint j = 0; j < json_array_get_length(ta); j++) {
        const char *tv = json_array_get_string_element(ta, j);
        if (tv) json_builder_add_string_value(b, tv);
        else json_builder_add_string_value(b, "");
      }
      json_builder_end_array(b);
    }
  }
  json_builder_end_array(b);

  json_builder_add_string_value(b, content);

  json_builder_end_array(b);

  JsonGenerator *g = json_generator_new();
  if (!g) {
    g_object_unref(b);
    if (out_err) *out_err = g_strdup("OOM");
    return false;
  }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  json_generator_set_pretty(g, FALSE);

  char *s = json_generator_to_data(g, NULL);

  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);

  if (!s) {
    if (out_err) *out_err = g_strdup("serialization failed");
    return false;
  }

  *out_ser_json = s; /* g_free */
  return true;
}

static bool signet_compute_event_id_hex_from_fields(const char *pubkey_hex,
                                                    int64_t created_at,
                                                    int kind,
                                                    JsonArray *tags,
                                                    const char *content,
                                                    char **out_id_hex,
                                                    char **out_err) {
  if (out_err) *out_err = NULL;
  if (out_id_hex) *out_id_hex = NULL;

  if (!out_id_hex) return false;

  char *ser = NULL;
  char *serr = NULL;
  if (!signet_build_event_serialization_json(pubkey_hex, created_at, kind, tags, content, &ser, &serr)) {
    if (out_err) *out_err = serr ? serr : g_strdup("failed to serialize");
    else g_free(serr);
    return false;
  }
  g_free(serr);

  uint8_t id32[32];
  signet_sha256((const uint8_t *)ser, strlen(ser), id32);

  char *id_hex = signet_hex_encode(id32, sizeof(id32));
  signet_memzero(id32, sizeof(id32));
  g_free(ser);

  if (!id_hex) {
    if (out_err) *out_err = g_strdup("OOM");
    return false;
  }

  *out_id_hex = id_hex;
  return true;
}

static bool signet_sign_event_id_hex_bip340(const char *seckey_hex,
                                            const char *event_id_hex,
                                            char **out_sig_hex,
                                            char **out_pubkey_hex,
                                            char **out_err) {
  if (out_err) *out_err = NULL;
  if (out_sig_hex) *out_sig_hex = NULL;
  if (out_pubkey_hex) *out_pubkey_hex = NULL;

  if (!seckey_hex || !event_id_hex || !out_sig_hex || !out_pubkey_hex) {
    if (out_err) *out_err = g_strdup("internal error");
    return false;
  }

  uint8_t sk32[32];
  uint8_t msg32[32];
  memset(sk32, 0, sizeof(sk32));
  memset(msg32, 0, sizeof(msg32));

  if (strlen(seckey_hex) != 64 || !signet_hex_decode(seckey_hex, sk32, 32)) {
    if (out_err) *out_err = g_strdup("invalid signer secret key");
    signet_memzero(sk32, sizeof(sk32));
    return false;
  }

  if (strlen(event_id_hex) != 64 || !signet_hex_decode(event_id_hex, msg32, 32)) {
    if (out_err) *out_err = g_strdup("invalid event id");
    signet_memzero(sk32, sizeof(sk32));
    signet_memzero(msg32, sizeof(msg32));
    return false;
  }

  uint8_t sig64[64];
  uint8_t pk32[32];
  memset(sig64, 0, sizeof(sig64));
  memset(pk32, 0, sizeof(pk32));

  char *serr = NULL;
  bool ok = signet_bip340_sign(sk32, msg32, sig64, pk32, &serr);

  signet_memzero(sk32, sizeof(sk32));
  signet_memzero(msg32, sizeof(msg32));

  if (!ok) {
    if (out_err) *out_err = serr ? serr : g_strdup("signature failed");
    else g_free(serr);
    signet_memzero(sig64, sizeof(sig64));
    signet_memzero(pk32, sizeof(pk32));
    return false;
  }
  g_free(serr);

  char *sig_hex = signet_hex_encode(sig64, sizeof(sig64));
  char *pub_hex = signet_hex_encode(pk32, sizeof(pk32));

  signet_memzero(sig64, sizeof(sig64));
  signet_memzero(pk32, sizeof(pk32));

  if (!sig_hex || !pub_hex) {
    g_free(sig_hex);
    g_free(pub_hex);
    if (out_err) *out_err = g_strdup("OOM");
    return false;
  }

  *out_sig_hex = sig_hex;
  *out_pubkey_hex = pub_hex;
  return true;
}

/* ---------------------------- NIP-46 responses ---------------------------- */

static char *signet_nip46_build_response_json(const char *req_id,
                                              const char *result_string_or_null,
                                              const char *error_string_or_null) {
  JsonBuilder *b = json_builder_new();
  if (!b) return NULL;

  json_builder_begin_object(b);

  json_builder_set_member_name(b, "id");
  json_builder_add_string_value(b, req_id ? req_id : "");

  if (error_string_or_null) {
    json_builder_set_member_name(b, "error");
    json_builder_add_string_value(b, error_string_or_null);
  } else {
    json_builder_set_member_name(b, "result");
    json_builder_add_string_value(b, result_string_or_null ? result_string_or_null : "");
  }

  json_builder_end_object(b);

  JsonGenerator *g = json_generator_new();
  if (!g) {
    g_object_unref(b);
    return NULL;
  }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  json_generator_set_pretty(g, FALSE);

  char *out = json_generator_to_data(g, NULL);

  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);

  return out; /* g_free */
}

/* ------------------------------ audit helpers ----------------------------- */

static void signet_audit_nip46(SignetAuditLogger *audit,
                               int64_t now,
                               const char *identity,
                               const char *client_pubkey_hex,
                               const char *outer_event_id_hex,
                               const char *request_id,
                               const char *method,
                               int event_kind,
                               const char *decision,
                               const char *reason_code,
                               const char *status,
                               const char *code) {
  if (!audit) return;

  JsonBuilder *b = json_builder_new();
  if (!b) return;

  json_builder_begin_object(b);

  json_builder_set_member_name(b, "ts");
  json_builder_add_int_value(b, (gint64)now);

  json_builder_set_member_name(b, "component");
  json_builder_add_string_value(b, "nip46_server");

  if (identity) {
    json_builder_set_member_name(b, "identity");
    json_builder_add_string_value(b, identity);
  }

  if (client_pubkey_hex) {
    json_builder_set_member_name(b, "client_pubkey");
    json_builder_add_string_value(b, client_pubkey_hex);
  }

  if (outer_event_id_hex) {
    json_builder_set_member_name(b, "outer_event_id");
    json_builder_add_string_value(b, outer_event_id_hex);
  }

  if (request_id) {
    json_builder_set_member_name(b, "request_id");
    json_builder_add_string_value(b, request_id);
  }

  if (method) {
    json_builder_set_member_name(b, "method");
    json_builder_add_string_value(b, method);
  }

  if (event_kind >= 0) {
    json_builder_set_member_name(b, "event_kind");
    json_builder_add_int_value(b, (gint64)event_kind);
  }

  if (decision) {
    json_builder_set_member_name(b, "decision");
    json_builder_add_string_value(b, decision);
  }

  if (reason_code) {
    json_builder_set_member_name(b, "reason_code");
    json_builder_add_string_value(b, reason_code);
  }

  if (status) {
    json_builder_set_member_name(b, "status");
    json_builder_add_string_value(b, status);
  }

  if (code) {
    json_builder_set_member_name(b, "code");
    json_builder_add_string_value(b, code);
  }

  json_builder_end_object(b);

  JsonGenerator *g = json_generator_new();
  if (!g) {
    g_object_unref(b);
    return;
  }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  json_generator_set_pretty(g, FALSE);

  char *out = json_generator_to_data(g, NULL);

  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);

  if (!out) return;

  (void)signet_audit_log_json(audit, SIGNET_AUDIT_EVENT_SIGN_REQUEST, out);
  g_free(out);
}

/* ------------------------------ sign_event op ----------------------------- */

static bool signet_json_event_extract_kind(const char *event_json, int *out_kind) {
  if (out_kind) *out_kind = -1;
  if (!event_json || !out_kind) return false;

  JsonParser *p = json_parser_new();
  if (!p) return false;

  GError *err = NULL;
  if (!json_parser_load_from_data(p, event_json, -1, &err)) {
    if (err) g_error_free(err);
    g_object_unref(p);
    return false;
  }

  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(p);
    return false;
  }

  JsonObject *o = json_node_get_object(root);
  if (!o || !json_object_has_member(o, "kind")) {
    g_object_unref(p);
    return false;
  }

  *out_kind = (int)json_object_get_int_member(o, "kind");

  g_object_unref(p);
  return true;
}

static char *signet_sign_event_json_with_seckey(const char *seckey_hex,
                                                const char *expected_pubkey_hex,
                                                const char *event_json,
                                                char **out_err) {
  if (out_err) *out_err = NULL;
  if (!seckey_hex || !event_json) {
    if (out_err) *out_err = g_strdup("internal error");
    return NULL;
  }

  JsonParser *p = json_parser_new();
  if (!p) {
    if (out_err) *out_err = g_strdup("OOM");
    return NULL;
  }

  GError *gerr = NULL;
  if (!json_parser_load_from_data(p, event_json, -1, &gerr)) {
    if (out_err) *out_err = g_strdup(gerr && gerr->message ? gerr->message : "invalid event JSON");
    if (gerr) g_error_free(gerr);
    g_object_unref(p);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    if (out_err) *out_err = g_strdup("event must be a JSON object");
    g_object_unref(p);
    return NULL;
  }

  JsonObject *o = json_node_get_object(root);
  if (!o) {
    if (out_err) *out_err = g_strdup("event must be a JSON object");
    g_object_unref(p);
    return NULL;
  }

  if (!json_object_has_member(o, "created_at") || !json_object_has_member(o, "kind") ||
      !json_object_has_member(o, "content")) {
    if (out_err) *out_err = g_strdup("event missing required fields");
    g_object_unref(p);
    return NULL;
  }

  int64_t created_at = (int64_t)json_object_get_int_member(o, "created_at");
  int kind = (int)json_object_get_int_member(o, "kind");
  const char *content = json_object_get_string_member(o, "content");
  if (!content) content = "";

  JsonArray *tags = NULL;
  if (json_object_has_member(o, "tags")) {
    JsonNode *tn = json_object_get_member(o, "tags");
    if (tn && JSON_NODE_HOLDS_ARRAY(tn)) tags = json_node_get_array(tn);
  }

  /* Derive pubkey from secret key; ensure it matches expected_pubkey_hex (if provided). */
  char *derived_sig_hex = NULL;
  char *derived_pub_hex = NULL;
  char *sign_err = NULL;

  /* First compute event id using the derived pubkey (canonical Nostr signing behavior). */
  /* We need the derived pubkey hex, so temporarily sign a dummy hash? No: derive pubkey via bip340_sign helper. */
  /* We'll derive pubkey x-only directly from seckey using EC multiply. */
  uint8_t sk32[32];
  uint8_t pk32[32];
  memset(sk32, 0, sizeof(sk32));
  memset(pk32, 0, sizeof(pk32));

  const EC_GROUP *group = signet_secp256k1_group();
  if (!group) {
    if (out_err) *out_err = g_strdup("secp256k1 unavailable");
    g_object_unref(p);
    return NULL;
  }

  if (strlen(seckey_hex) != 64 || !signet_hex_decode(seckey_hex, sk32, 32)) {
    if (out_err) *out_err = g_strdup("invalid secret key");
    signet_memzero(sk32, sizeof(sk32));
    g_object_unref(p);
    return NULL;
  }

  bool y_even = false;
  if (!signet_pubkey_xonly_from_seckey32(group, sk32, pk32, &y_even)) {
    if (out_err) *out_err = g_strdup("failed to derive pubkey");
    signet_memzero(sk32, sizeof(sk32));
    signet_memzero(pk32, sizeof(pk32));
    g_object_unref(p);
    return NULL;
  }

  char *derived_pubkey_hex = signet_hex_encode(pk32, 32);
  signet_memzero(pk32, sizeof(pk32));
  signet_memzero(sk32, sizeof(sk32));

  if (!derived_pubkey_hex) {
    if (out_err) *out_err = g_strdup("OOM");
    g_object_unref(p);
    return NULL;
  }

  if (expected_pubkey_hex && expected_pubkey_hex[0] != '\0' &&
      g_ascii_strcasecmp(expected_pubkey_hex, derived_pubkey_hex) != 0) {
    if (out_err) *out_err = g_strdup("remote signer pubkey does not match secret key");
    g_free(derived_pubkey_hex);
    g_object_unref(p);
    return NULL;
  }

  char *event_id_hex = NULL;
  char *id_err = NULL;
  if (!signet_compute_event_id_hex_from_fields(derived_pubkey_hex, created_at, kind, tags, content,
                                               &event_id_hex, &id_err)) {
    if (out_err) *out_err = id_err ? id_err : g_strdup("failed to compute event id");
    else g_free(id_err);
    g_free(derived_pubkey_hex);
    g_object_unref(p);
    return NULL;
  }
  g_free(id_err);

  if (!signet_sign_event_id_hex_bip340(seckey_hex, event_id_hex,
                                       &derived_sig_hex, &derived_pub_hex, &sign_err)) {
    if (out_err) *out_err = sign_err ? sign_err : g_strdup("failed to sign event");
    else g_free(sign_err);
    g_free(event_id_hex);
    g_free(derived_pubkey_hex);
    g_object_unref(p);
    return NULL;
  }
  g_free(sign_err);

  /* Build signed event object (ensure pubkey/id/sig set; preserve tags/content/created_at/kind). */
  JsonBuilder *b = json_builder_new();
  if (!b) {
    if (out_err) *out_err = g_strdup("OOM");
    g_free(event_id_hex);
    g_free(derived_sig_hex);
    g_free(derived_pub_hex);
    g_free(derived_pubkey_hex);
    g_object_unref(p);
    return NULL;
  }

  json_builder_begin_object(b);

  json_builder_set_member_name(b, "id");
  json_builder_add_string_value(b, event_id_hex);

  json_builder_set_member_name(b, "pubkey");
  json_builder_add_string_value(b, derived_pubkey_hex);

  json_builder_set_member_name(b, "created_at");
  json_builder_add_int_value(b, (gint64)created_at);

  json_builder_set_member_name(b, "kind");
  json_builder_add_int_value(b, (gint64)kind);

  json_builder_set_member_name(b, "tags");
  json_builder_begin_array(b);
  if (tags) {
    for (guint i = 0; i < json_array_get_length(tags); i++) {
      JsonNode *tn = json_array_get_element(tags, i);
      if (!tn || !JSON_NODE_HOLDS_ARRAY(tn)) continue;
      JsonArray *ta = json_node_get_array(tn);

      json_builder_begin_array(b);
      for (guint j = 0; j < json_array_get_length(ta); j++) {
        const char *tv = json_array_get_string_element(ta, j);
        json_builder_add_string_value(b, tv ? tv : "");
      }
      json_builder_end_array(b);
    }
  }
  json_builder_end_array(b);

  json_builder_set_member_name(b, "content");
  json_builder_add_string_value(b, content ? content : "");

  json_builder_set_member_name(b, "sig");
  json_builder_add_string_value(b, derived_sig_hex);

  json_builder_end_object(b);

  JsonGenerator *gen = json_generator_new();
  if (!gen) {
    if (out_err) *out_err = g_strdup("OOM");
    g_object_unref(b);
    g_free(event_id_hex);
    g_free(derived_sig_hex);
    g_free(derived_pub_hex);
    g_free(derived_pubkey_hex);
    g_object_unref(p);
    return NULL;
  }

  JsonNode *out_root = json_builder_get_root(b);
  json_generator_set_root(gen, out_root);
  json_generator_set_pretty(gen, FALSE);

  char *signed_event_json = json_generator_to_data(gen, NULL);

  json_node_free(out_root);
  g_object_unref(gen);
  g_object_unref(b);

  g_free(event_id_hex);
  g_free(derived_sig_hex);
  g_free(derived_pub_hex);
  g_free(derived_pubkey_hex);

  g_object_unref(p);

  if (!signed_event_json) {
    if (out_err) *out_err = g_strdup("failed to build signed event JSON");
    return NULL;
  }

  return signed_event_json; /* g_free */
}

/* ------------------------------ server object ----------------------------- */

struct SignetNip46Server {
  SignetRelayPool *relays;
  SignetPolicyEngine *policy;
  SignetKeyStore *keys;
  SignetReplayCache *replay;
  SignetAuditLogger *audit;

  char *identity;

  GMutex mu;
};

SignetNip46Server *signet_nip46_server_new(SignetRelayPool *relays,
                                           SignetPolicyEngine *policy,
                                           SignetKeyStore *keys,
                                           SignetReplayCache *replay,
                                           SignetAuditLogger *audit,
                                           const SignetNip46ServerConfig *cfg) {
  if (!cfg || !cfg->identity) return NULL;

  SignetNip46Server *s = (SignetNip46Server *)calloc(1, sizeof(*s));
  if (!s) return NULL;

  g_mutex_init(&s->mu);

  s->relays = relays;
  s->policy = policy;
  s->keys = keys;
  s->replay = replay;
  s->audit = audit;

  s->identity = strdup(cfg->identity);
  if (!s->identity) {
    g_mutex_clear(&s->mu);
    free(s);
    return NULL;
  }

  return s;
}

void signet_nip46_server_free(SignetNip46Server *s) {
  if (!s) return;
  g_mutex_clear(&s->mu);
  free(s->identity);
  free(s);
}

static char *signet_build_outer_response_event_json(const char *remote_signer_seckey_hex,
                                                    const char *remote_signer_pubkey_hex,
                                                    const char *client_pubkey_hex,
                                                    const char *request_outer_event_id_hex,
                                                    int64_t now,
                                                    const char *encrypted_content,
                                                    char **out_err) {
  if (out_err) *out_err = NULL;

  if (!remote_signer_seckey_hex || !remote_signer_pubkey_hex || !client_pubkey_hex || !encrypted_content) {
    if (out_err) *out_err = g_strdup("internal error");
    return NULL;
  }

  /* tags: [["p", client], ["e", request_outer_event_id]] (second is optional) */
  JsonBuilder *tags_b = json_builder_new();
  if (!tags_b) {
    if (out_err) *out_err = g_strdup("OOM");
    return NULL;
  }

  json_builder_begin_array(tags_b);

  json_builder_begin_array(tags_b);
  json_builder_add_string_value(tags_b, "p");
  json_builder_add_string_value(tags_b, client_pubkey_hex);
  json_builder_end_array(tags_b);

  if (request_outer_event_id_hex && request_outer_event_id_hex[0] != '\0') {
    json_builder_begin_array(tags_b);
    json_builder_add_string_value(tags_b, "e");
    json_builder_add_string_value(tags_b, request_outer_event_id_hex);
    json_builder_end_array(tags_b);
  }

  json_builder_end_array(tags_b);

  JsonNode *tags_root = json_builder_get_root(tags_b);
  JsonArray *tags_arr = NULL;
  if (tags_root && JSON_NODE_HOLDS_ARRAY(tags_root)) tags_arr = json_node_get_array(tags_root);

  char *event_id_hex = NULL;
  char *id_err = NULL;
  if (!signet_compute_event_id_hex_from_fields(remote_signer_pubkey_hex,
                                               now,
                                               SIGNET_NIP46_KIND_CIPHERTEXT,
                                               tags_arr,
                                               encrypted_content,
                                               &event_id_hex,
                                               &id_err)) {
    if (out_err) *out_err = id_err ? id_err : g_strdup("failed to compute response event id");
    else g_free(id_err);
    json_node_free(tags_root);
    g_object_unref(tags_b);
    return NULL;
  }
  g_free(id_err);

  char *sig_hex = NULL;
  char *derived_pub_hex = NULL;
  char *sig_err = NULL;
  if (!signet_sign_event_id_hex_bip340(remote_signer_seckey_hex, event_id_hex,
                                       &sig_hex, &derived_pub_hex, &sig_err)) {
    if (out_err) *out_err = sig_err ? sig_err : g_strdup("failed to sign response event");
    else g_free(sig_err);
    g_free(event_id_hex);
    json_node_free(tags_root);
    g_object_unref(tags_b);
    return NULL;
  }
  g_free(sig_err);

  /* Ensure remote_signer_pubkey_hex matches derived */
  if (g_ascii_strcasecmp(remote_signer_pubkey_hex, derived_pub_hex) != 0) {
    if (out_err) *out_err = g_strdup("remote signer pubkey does not match secret key");
    g_free(event_id_hex);
    g_free(sig_hex);
    g_free(derived_pub_hex);
    json_node_free(tags_root);
    g_object_unref(tags_b);
    return NULL;
  }

  JsonBuilder *b = json_builder_new();
  if (!b) {
    if (out_err) *out_err = g_strdup("OOM");
    g_free(event_id_hex);
    g_free(sig_hex);
    g_free(derived_pub_hex);
    json_node_free(tags_root);
    g_object_unref(tags_b);
    return NULL;
  }

  json_builder_begin_object(b);

  json_builder_set_member_name(b, "id");
  json_builder_add_string_value(b, event_id_hex);

  json_builder_set_member_name(b, "pubkey");
  json_builder_add_string_value(b, remote_signer_pubkey_hex);

  json_builder_set_member_name(b, "created_at");
  json_builder_add_int_value(b, (gint64)now);

  json_builder_set_member_name(b, "kind");
  json_builder_add_int_value(b, (gint64)SIGNET_NIP46_KIND_CIPHERTEXT);

  json_builder_set_member_name(b, "tags");
  json_builder_begin_array(b);
  /* rebuild tags from tags_arr */
  if (tags_arr) {
    for (guint i = 0; i < json_array_get_length(tags_arr); i++) {
      JsonNode *tn = json_array_get_element(tags_arr, i);
      if (!tn || !JSON_NODE_HOLDS_ARRAY(tn)) continue;
      JsonArray *ta = json_node_get_array(tn);

      json_builder_begin_array(b);
      for (guint j = 0; j < json_array_get_length(ta); j++) {
        const char *tv = json_array_get_string_element(ta, j);
        json_builder_add_string_value(b, tv ? tv : "");
      }
      json_builder_end_array(b);
    }
  }
  json_builder_end_array(b);

  json_builder_set_member_name(b, "content");
  json_builder_add_string_value(b, encrypted_content);

  json_builder_set_member_name(b, "sig");
  json_builder_add_string_value(b, sig_hex);

  json_builder_end_object(b);

  JsonGenerator *gen = json_generator_new();
  if (!gen) {
    if (out_err) *out_err = g_strdup("OOM");
    g_object_unref(b);
    g_free(event_id_hex);
    g_free(sig_hex);
    g_free(derived_pub_hex);
    json_node_free(tags_root);
    g_object_unref(tags_b);
    return NULL;
  }

  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);

  char *out = json_generator_to_data(gen, NULL);

  json_node_free(root);
  g_object_unref(gen);
  g_object_unref(b);

  g_free(event_id_hex);
  g_free(sig_hex);
  g_free(derived_pub_hex);

  json_node_free(tags_root);
  g_object_unref(tags_b);

  if (!out) {
    if (out_err) *out_err = g_strdup("failed to build response event JSON");
    return NULL;
  }

  return out; /* g_free */
}

bool signet_nip46_server_handle_event(SignetNip46Server *s,
                                      const char *remote_signer_pubkey_hex,
                                      const char *remote_signer_secret_key_hex,
                                      const char *client_pubkey_hex,
                                      const char *ciphertext,
                                      int64_t created_at,
                                      const char *event_id_hex,
                                      int64_t now) {
  if (!s) return false;
  if (!remote_signer_pubkey_hex || !remote_signer_secret_key_hex || !client_pubkey_hex || !ciphertext) return false;

  g_mutex_lock(&s->mu);

  /* 1) Replay check first (by outer event id) */
  SignetReplayResult rr = SIGNET_REPLAY_OK;
  if (s->replay && event_id_hex && event_id_hex[0] != '\0') {
    rr = signet_replay_check_and_mark(s->replay, event_id_hex, created_at, now);
  }

  /* 2) Decrypt request content (NIP-04) */
  char *plain = NULL;
  char *dec_err = NULL;
  bool dec_ok = signet_nip04_decrypt(remote_signer_secret_key_hex, client_pubkey_hex, ciphertext, &plain, &dec_err);

  /* If decrypt fails, we can only return a generic error (no request id). */
  SignetNip46Request req;
  memset(&req, 0, sizeof(req));

  char *parse_err = NULL;
  bool parse_ok = false;
  if (dec_ok) {
    parse_ok = signet_nip46_parse_request_json(plain, &req, &parse_err);
  }

  const char *method = parse_ok ? req.method : NULL;
  const char *req_id = parse_ok ? req.id : NULL;

  /* Determine event_kind for policy checks (only meaningful for sign_event) */
  int event_kind = -1;
  if (parse_ok && method && strcmp(method, "sign_event") == 0 && req.params && req.params->len >= 1) {
    const char *evt_json = (const char *)g_ptr_array_index(req.params, 0);
    (void)signet_json_event_extract_kind(evt_json, &event_kind);
  }

  /* 3) Policy check */
  SignetPolicyResult pres;
  memset(&pres, 0, sizeof(pres));
  pres.decision = SIGNET_POLICY_DECISION_DENY;
  pres.expires_at = 0;
  pres.reason_code = "policy_engine_missing";

  bool policy_ok = false;
  if (s->policy && parse_ok && method) {
    policy_ok = signet_policy_engine_eval(s->policy,
                                          s->identity,
                                          client_pubkey_hex,
                                          method,
                                          event_kind,
                                          now,
                                          &pres);
    if (!policy_ok) {
      pres.decision = SIGNET_POLICY_DECISION_DENY;
      pres.reason_code = "policy_eval_error";
    }
  } else if (parse_ok && method) {
    /* No policy engine => deny by default */
    policy_ok = true;
    pres.decision = SIGNET_POLICY_DECISION_DENY;
    pres.reason_code = "policy_engine_missing";
  }

  /* 4) Decide early errors (replay, decrypt, parse, deny) */
  const char *decision = "deny";
  const char *status = "error";
  const char *code = "internal_error";

  char *result = NULL; /* heap string result (for response.result) */
  char *err_str = NULL; /* heap string for response.error */
  bool allow = false;

  if (rr == SIGNET_REPLAY_DUPLICATE) {
    code = "replay_duplicate";
    err_str = g_strdup("replay rejected (duplicate)");
  } else if (rr == SIGNET_REPLAY_TOO_OLD) {
    code = "replay_too_old";
    err_str = g_strdup("replay rejected (too old)");
  } else if (rr == SIGNET_REPLAY_TOO_FAR_IN_FUTURE) {
    code = "replay_in_future";
    err_str = g_strdup("replay rejected (in future)");
  } else if (!dec_ok) {
    code = "decrypt_failed";
    err_str = g_strdup("decrypt failed");
  } else if (!parse_ok) {
    code = "invalid_request";
    err_str = g_strdup(parse_err ? parse_err : "invalid request");
  } else if (!policy_ok) {
    code = "policy_error";
    err_str = g_strdup("policy evaluation error");
  } else if (pres.decision != SIGNET_POLICY_DECISION_ALLOW) {
    code = "policy_denied";
    err_str = g_strdup(pres.reason_code ? pres.reason_code : "denied");
  } else {
    allow = true;
  }

  if (allow) {
    decision = "allow";

    /* 5) Execute method */
    if (strcmp(method, "ping") == 0) {
      result = g_strdup("pong");
      status = "ok";
      code = "ok";
    } else if (strcmp(method, "get_public_key") == 0) {
      /* Derive pubkey from secret key to avoid mismatches */
      uint8_t sk32[32];
      uint8_t pk32[32];
      memset(sk32, 0, sizeof(sk32));
      memset(pk32, 0, sizeof(pk32));

      const EC_GROUP *group = signet_secp256k1_group();
      if (!group) {
        err_str = g_strdup("secp256k1 unavailable");
        status = "error";
        code = "crypto_unavailable";
      } else if (strlen(remote_signer_secret_key_hex) != 64 ||
                 !signet_hex_decode(remote_signer_secret_key_hex, sk32, 32) ||
                 !signet_pubkey_xonly_from_seckey32(group, sk32, pk32, NULL)) {
        err_str = g_strdup("invalid secret key");
        status = "error";
        code = "invalid_key";
      } else {
        char *pub_hex = signet_hex_encode(pk32, 32);
        if (!pub_hex) {
          err_str = g_strdup("OOM");
          status = "error";
          code = "oom";
        } else {
          /* Ensure it matches passed pubkey, otherwise fail hard */
          if (g_ascii_strcasecmp(remote_signer_pubkey_hex, pub_hex) != 0) {
            err_str = g_strdup("remote signer pubkey does not match secret key");
            g_free(pub_hex);
            status = "error";
            code = "key_mismatch";
          } else {
            result = pub_hex;
            status = "ok";
            code = "ok";
          }
        }
      }

      signet_memzero(sk32, sizeof(sk32));
      signet_memzero(pk32, sizeof(pk32));
    } else if (strcmp(method, "sign_event") == 0) {
      if (!req.params || req.params->len < 1) {
        err_str = g_strdup("sign_event requires event JSON param");
        status = "error";
        code = "invalid_params";
      } else {
        const char *evt_json = (const char *)g_ptr_array_index(req.params, 0);
        char *serr = NULL;

        /* Execute signing using the remote signer secret key (custody key is expected to be supplied by caller). */
        char *signed_evt = signet_sign_event_json_with_seckey(remote_signer_secret_key_hex,
                                                              remote_signer_pubkey_hex,
                                                              evt_json,
                                                              &serr);
        if (!signed_evt) {
          err_str = serr ? serr : g_strdup("sign_event failed");
          status = "error";
          code = "sign_failed";
        } else {
          result = signed_evt; /* contains signed event JSON as a string result */
          status = "ok";
          code = "ok";
          g_free(serr);
        }
      }
    } else if (strcmp(method, "nip04_encrypt") == 0) {
      if (!req.params || req.params->len < 2) {
        err_str = g_strdup("nip04_encrypt requires [pubkey, plaintext]");
        status = "error";
        code = "invalid_params";
      } else {
        const char *peer = (const char *)g_ptr_array_index(req.params, 0);
        const char *pt = (const char *)g_ptr_array_index(req.params, 1);

        char *ct = NULL;
        char *eerr = NULL;
        if (!signet_nip04_encrypt(remote_signer_secret_key_hex, peer, pt, &ct, &eerr)) {
          err_str = eerr ? eerr : g_strdup("encrypt failed");
          status = "error";
          code = "encrypt_failed";
        } else {
          result = ct;
          status = "ok";
          code = "ok";
          g_free(eerr);
        }
      }
    } else if (strcmp(method, "nip04_decrypt") == 0) {
      if (!req.params || req.params->len < 2) {
        err_str = g_strdup("nip04_decrypt requires [pubkey, ciphertext]");
        status = "error";
        code = "invalid_params";
      } else {
        const char *peer = (const char *)g_ptr_array_index(req.params, 0);
        const char *ct = (const char *)g_ptr_array_index(req.params, 1);

        char *pt = NULL;
        char *derr = NULL;
        if (!signet_nip04_decrypt(remote_signer_secret_key_hex, peer, ct, &pt, &derr)) {
          err_str = derr ? derr : g_strdup("decrypt failed");
          status = "error";
          code = "decrypt_failed";
        } else {
          result = pt;
          status = "ok";
          code = "ok";
          g_free(derr);
        }
      }
    } else if (strcmp(method, "get_relays") == 0) {
      /* RelayPool currently does not expose relay list; return empty array JSON (as string result). */
      result = g_strdup("[]");
      status = "ok";
      code = "ok";
    } else {
      err_str = g_strdup("unsupported method");
      status = "error";
      code = "unsupported_method";
    }
  }

  /* 6) Audit decision (never include plaintext/keys) */
  signet_audit_nip46(s->audit,
                     now,
                     s->identity,
                     client_pubkey_hex,
                     event_id_hex,
                     req_id,
                     method ? method : "unknown",
                     event_kind,
                     decision,
                     pres.reason_code ? pres.reason_code : "n/a",
                     status,
                     code);

  /* 7) Build response JSON payload (encrypted) */
  char *resp_json = NULL;
  if (err_str) {
    resp_json = signet_nip46_build_response_json(req_id, NULL, err_str);
  } else {
    resp_json = signet_nip46_build_response_json(req_id, result, NULL);
  }

  /* 8) Encrypt response */
  char *enc_resp = NULL;
  char *enc_err = NULL;
  bool enc_ok = false;

  if (resp_json) {
    enc_ok = signet_nip04_encrypt(remote_signer_secret_key_hex, client_pubkey_hex, resp_json, &enc_resp, &enc_err);
  }

  /* 9) Build outer response event JSON (signed) and publish */
  bool published = false;
  if (enc_ok && enc_resp) {
    char *outer_err = NULL;
    char *outer_evt = signet_build_outer_response_event_json(remote_signer_secret_key_hex,
                                                             remote_signer_pubkey_hex,
                                                             client_pubkey_hex,
                                                             event_id_hex,
                                                             now,
                                                             enc_resp,
                                                             &outer_err);
    if (outer_evt) {
      if (s->relays) {
        published = (signet_relay_pool_publish_event_json(s->relays, outer_evt) == 0);
      }
      g_free(outer_evt);
    } else {
      /* If we cannot build response event, we still consider the request "handled" but not published. */
      g_free(outer_err);
    }
  }

  /* cleanup sensitive-ish material (plaintext request, decrypted results) */
  if (plain) {
    signet_memzero(plain, strlen(plain));
    g_free(plain);
  }

  if (resp_json) {
    signet_memzero(resp_json, strlen(resp_json));
    g_free(resp_json);
  }

  if (enc_resp) {
    /* ciphertext is not secret; no need to wipe, but safe to clear anyway */
    signet_memzero(enc_resp, strlen(enc_resp));
    g_free(enc_resp);
  }

  g_free(enc_err);

  if (result) {
    /* may contain plaintext result for nip04_decrypt; wipe before free */
    signet_memzero(result, strlen(result));
    g_free(result);
  }

  if (err_str) g_free(err_str);

  if (dec_err) g_free(dec_err);
  if (parse_err) g_free(parse_err);

  signet_nip46_request_clear(&req);

  g_mutex_unlock(&s->mu);

  /* We handled the event; return whether we managed to publish a response. */
  return published;
}