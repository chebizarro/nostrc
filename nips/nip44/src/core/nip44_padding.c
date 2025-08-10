#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* NIP-44 calc_padded_len per spec pseudocode */
static size_t calc_padded_len(size_t unpadded_len) {
  if (unpadded_len <= 32) return 32;
  /* next power-of-two strictly greater than unpadded_len-1 */
  size_t x = (unpadded_len - 1);
  /* compute floor(log2(x)) and then next_power = 1 << (floor(log2(x)) + 1) */
  size_t lg = 0; while ((1ULL << (lg + 1)) <= x) lg++;
  size_t next_power = 1ULL << (lg + 1);
  size_t chunk = (next_power <= 256) ? 32 : (next_power / 8);
  /* round up to multiple of chunk */
  size_t nblocks = (unpadded_len + chunk - 1) / chunk;
  return nblocks * chunk;
}

/* Pad: [len_be:u16][plaintext][zeros], where total padded section after the 2-byte length
   is calc_padded_len(len). Thus overall buffer size is 2 + calc_padded_len(len). */
int nip44_pad(const uint8_t *in, size_t in_len, uint8_t **out_padded, size_t *out_padded_len) {
  if (!out_padded || !out_padded_len) return -1;
  if (in_len == 0 || in_len > 65535) return -1;
  size_t padded_section = calc_padded_len(in_len);
  size_t total = 2 + padded_section;
  uint8_t *buf = (uint8_t*)malloc(total);
  if (!buf) return -1;
  /* big-endian length */
  buf[0] = (uint8_t)((in_len >> 8) & 0xFF);
  buf[1] = (uint8_t)(in_len & 0xFF);
  if (in_len) memcpy(buf + 2, in, in_len);
  if (padded_section > in_len) memset(buf + 2 + in_len, 0, padded_section - in_len);
  *out_padded = buf;
  *out_padded_len = total;
  return 0;
}

/* Unpad and validate zeros after content */
int nip44_unpad(const uint8_t *padded, size_t padded_len, uint8_t **out, size_t *out_len) {
  if (!padded || padded_len < (2 + 32) || !out || !out_len) return -1;
  /* read big-endian length */
  size_t len = ((size_t)padded[0] << 8) | (size_t)padded[1];
  if (len == 0 || len > 65535) return -1;
  size_t expected_total = 2 + calc_padded_len(len);
  if (padded_len != expected_total) return -1;
  /* ensure zeros after content */
  for (size_t i = 2 + len; i < padded_len; i++) {
    if (padded[i] != 0) return -1;
  }
  uint8_t *plain = (uint8_t*)malloc(len);
  if (!plain) return -1;
  if (len) memcpy(plain, padded + 2, len);
  *out = plain;
  *out_len = len;
  return 0;
}
