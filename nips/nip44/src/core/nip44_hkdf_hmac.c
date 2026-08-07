#include <stdint.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>

static int evp_hmac_sha256(const uint8_t *key, size_t key_len,
                           const uint8_t *d1, size_t l1,
                           const uint8_t *d2, size_t l2,
                           const uint8_t *d3, size_t l3,
                           uint8_t out[32]) {
  int ok = 0;
  size_t out_len = 0;
  EVP_MAC *mac = NULL;
  EVP_MAC_CTX *ctx = NULL;

  if (!key || !out || (l1 && !d1) || (l2 && !d2) || (l3 && !d3)) return -1;

  mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
  if (!mac) goto done;
  ctx = EVP_MAC_CTX_new(mac);
  if (!ctx) goto done;

  OSSL_PARAM params[] = {
    OSSL_PARAM_construct_utf8_string(OSSL_ALG_PARAM_DIGEST, (char *)"SHA256", 0),
    OSSL_PARAM_construct_end()
  };
  if (EVP_MAC_init(ctx, key, key_len, params) != 1) goto done;
  if (l1 && EVP_MAC_update(ctx, d1, l1) != 1) goto done;
  if (l2 && EVP_MAC_update(ctx, d2, l2) != 1) goto done;
  if (l3 && EVP_MAC_update(ctx, d3, l3) != 1) goto done;
  if (EVP_MAC_final(ctx, out, &out_len, 32) != 1 || out_len != 32) goto done;
  ok = 1;

done:
  if (!ok) OPENSSL_cleanse(out, 32);
  EVP_MAC_CTX_free(ctx);
  EVP_MAC_free(mac);
  return ok ? 0 : -1;
}

int nip44_hkdf_extract(const uint8_t *salt, size_t salt_len,
                       const uint8_t *ikm, size_t ikm_len,
                       uint8_t prk_out[32]) {
  if (!salt || !ikm || !prk_out) return -1;
  return evp_hmac_sha256(salt, salt_len, ikm, ikm_len,
                         NULL, 0, NULL, 0, prk_out);
}

int nip44_hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t info_len,
                      uint8_t okm_out[], size_t okm_len) {
  uint8_t t[32] = {0};
  size_t t_len = 0;
  size_t pos = 0;
  int rc = -1;

  if (!prk || !okm_out || (info_len && !info) || okm_len == 0 ||
      okm_len > 255u * 32u) {
    return -1;
  }
  memset(okm_out, 0, okm_len);

  const size_t blocks = (okm_len + 31u) / 32u;
  for (size_t i = 1; i <= blocks; ++i) {
    const uint8_t counter = (uint8_t)i;
    if (evp_hmac_sha256(prk, 32,
                        t_len ? t : NULL, t_len,
                        info, info_len,
                        &counter, 1, t) != 0) {
      goto done;
    }
    const size_t take = okm_len - pos < sizeof(t) ? okm_len - pos : sizeof(t);
    memcpy(okm_out + pos, t, take);
    pos += take;
    t_len = sizeof(t);
  }
  rc = 0;

done:
  OPENSSL_cleanse(t, sizeof(t));
  if (rc != 0) OPENSSL_cleanse(okm_out, okm_len);
  return rc;
}

int nip44_hmac_sha256(const uint8_t *key, size_t key_len,
                      const uint8_t *data1, size_t len1,
                      const uint8_t *data2, size_t len2,
                      uint8_t mac_out[32]) {
  return evp_hmac_sha256(key, key_len, data1, len1,
                         data2, len2, NULL, 0, mac_out);
}
