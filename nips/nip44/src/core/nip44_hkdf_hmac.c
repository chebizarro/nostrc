#include <stdint.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>

static int evp_hmac_sha256(const uint8_t *key, size_t key_len,
                           const uint8_t *d1, size_t l1,
                           const uint8_t *d2, size_t l2,
                           uint8_t out[32]) {
  int ok = 0;
  EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
  if (!mac) return -1;
  EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
  if (!ctx) { EVP_MAC_free(mac); return -1; }
  OSSL_PARAM params[] = {
    OSSL_PARAM_construct_utf8_string(OSSL_ALG_PARAM_DIGEST, (char*)"SHA256", 0),
    OSSL_PARAM_construct_end()
  };
  if (EVP_MAC_init(ctx, key, key_len, params) != 1) goto done;
  if (d1 && l1 && EVP_MAC_update(ctx, d1, l1) != 1) goto done;
  if (d2 && l2 && EVP_MAC_update(ctx, d2, l2) != 1) goto done;
  size_t outl = 0;
  if (EVP_MAC_final(ctx, out, &outl, 32) != 1) goto done;
  if (outl != 32) goto done;
  ok = 1;
done:
  EVP_MAC_CTX_free(ctx);
  EVP_MAC_free(mac);
  return ok ? 0 : -1;
}

void nip44_hkdf_extract(const uint8_t *salt, size_t salt_len,
                        const uint8_t *ikm, size_t ikm_len,
                        uint8_t prk_out[32]) {
  (void)evp_hmac_sha256(salt, salt_len, ikm, ikm_len, NULL, 0, prk_out);
}

void nip44_hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t info_len,
                       uint8_t okm_out[], size_t okm_len) {
  /* N = ceil(L/HashLen) where HashLen=32, here L can be arbitrary but we call with small L */
  size_t n = (okm_len + 31) / 32;
  uint8_t t[32];
  uint8_t prev[32]; size_t prev_len = 0;
  size_t pos = 0;
  for (size_t i = 1; i <= n; i++) {
    /* HMAC(PRK, T(prev) || info || i) */
    uint8_t ctr = (uint8_t)i;
    /* Build data buffer segments via two calls */
    if (evp_hmac_sha256(prk, 32,
                        prev_len ? prev : NULL, prev_len,
                        NULL, 0, t) != 0) {
      /* fallback assemble contiguous buffer when segments >2 needed */
    }
    /* Now extend with info and counter in a second pass over same key */
    /* We need single HMAC over concatenation, so re-run with full message */
    size_t tmp_len = prev_len + info_len + 1;
    uint8_t *tmp = (uint8_t*)OPENSSL_malloc(tmp_len);
    if (!tmp) return;
    size_t off = 0;
    if (prev_len) { memcpy(tmp + off, prev, prev_len); off += prev_len; }
    if (info && info_len) { memcpy(tmp + off, info, info_len); off += info_len; }
    tmp[off++] = ctr;
    if (evp_hmac_sha256(prk, 32, tmp, tmp_len, NULL, 0, t) != 0) { OPENSSL_free(tmp); return; }
    OPENSSL_free(tmp);
    size_t c = (pos + 32 <= okm_len) ? 32 : (okm_len - pos);
    memcpy(okm_out + pos, t, c);
    memcpy(prev, t, 32);
    prev_len = 32;
    pos += c;
  }
  OPENSSL_cleanse(prev, sizeof(prev));
}

void nip44_hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *data1, size_t len1,
                       const uint8_t *data2, size_t len2,
                       uint8_t mac_out[32]) {
  /* Single-pass HMAC over concatenation of up to two segments */
  if (len1 && len2) {
    size_t tot = len1 + len2;
    uint8_t *tmp = (uint8_t*)OPENSSL_malloc(tot);
    if (!tmp) return;
    memcpy(tmp, data1, len1);
    memcpy(tmp + len1, data2, len2);
    (void)evp_hmac_sha256(key, key_len, tmp, tot, NULL, 0, mac_out);
    OPENSSL_free(tmp);
  } else {
    (void)evp_hmac_sha256(key, key_len,
                          data1, len1,
                          data2, len2,
                          mac_out);
  }
}
