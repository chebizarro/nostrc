#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>

int nip44_base64_encode(const uint8_t *buf, size_t len, char **out_b64) {
  if (!buf || !out_b64 || len == 0 || len > (size_t)(INT_MAX / 4) * 3) return -1;
  *out_b64 = NULL;

  const size_t encoded_len = 4 * ((len + 2) / 3);
  char *out = (char *)malloc(encoded_len + 1);
  if (!out) return -1;

  const int written = EVP_EncodeBlock((unsigned char *)out, buf, (int)len);
  if (written < 0 || (size_t)written != encoded_len) {
    OPENSSL_cleanse(out, encoded_len + 1);
    free(out);
    return -1;
  }
  out[encoded_len] = '\0';
  *out_b64 = out;
  return 0;
}

int nip44_base64_decode(const char *b64, uint8_t **out_buf, size_t *out_len) {
  if (!b64 || !out_buf || !out_len) return -1;
  *out_buf = NULL;
  *out_len = 0;

  const size_t in_len = strlen(b64);
  if (in_len == 0 || (in_len % 4) != 0 || in_len > INT_MAX) return -1;

  size_t padding = 0;
  if (b64[in_len - 1] == '=') padding++;
  if (b64[in_len - 2] == '=') padding++;
  for (size_t i = 0; i < in_len - padding; ++i) {
    const unsigned char c = (unsigned char)b64[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '+' || c == '/')) {
      return -1;
    }
  }
  for (size_t i = in_len - padding; i < in_len; ++i) {
    if (b64[i] != '=') return -1;
  }

  const size_t cap = (in_len / 4) * 3;
  uint8_t *out = (uint8_t *)malloc(cap ? cap : 1);
  if (!out) return -1;

  const int decoded = EVP_DecodeBlock(out, (const unsigned char *)b64, (int)in_len);
  if (decoded < 0 || (size_t)decoded < padding) {
    OPENSSL_cleanse(out, cap);
    free(out);
    return -1;
  }
  const size_t decoded_len = (size_t)decoded - padding;

  /* Re-encoding must reproduce the complete input exactly. This rejects
   * trailing junk, embedded padding, and non-canonical unused bits. */
  char *canonical = NULL;
  if (nip44_base64_encode(out, decoded_len, &canonical) != 0 ||
      strcmp(canonical, b64) != 0) {
    free(canonical);
    OPENSSL_cleanse(out, cap);
    free(out);
    return -1;
  }
  free(canonical);

  *out_buf = out;
  *out_len = decoded_len;
  return 0;
}
