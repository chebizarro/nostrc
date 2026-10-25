/* Deterministic 41-byte PNG transport prefix, not a decodable PNG image.
 * Wire: signature, IHDR(1x1 grayscale), CRC, IDAT length/type, ciphertext.
 * The address is SHA256(prefix || ciphertext). SPDX-License-Identifier: MIT */
#ifndef BLOSSOM_SHIM_H
#define BLOSSOM_SHIM_H
#include <blossom/types.h>
#include <blossom/capability.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BLOSSOM_PNG_SHIM_LEN 41
static inline size_t blossom_shim_encoded_len(size_t n) {
    return n > SIZE_MAX - BLOSSOM_PNG_SHIM_LEN ? 0 : n + BLOSSOM_PNG_SHIM_LEN;
}
/* Pure functions: no environment, cache, network or policy decisions. */
BLOSSOM_API blossom_error_t blossom_shim_wrap(const uint8_t *, size_t, uint8_t *, size_t);
BLOSSOM_API bool blossom_shim_detect(const uint8_t *, size_t);
/* Returns a borrowed view into the input. Invalid prefix/length is refused. */
BLOSSOM_API blossom_error_t blossom_shim_unwrap(const uint8_t *, size_t, const uint8_t **, size_t *);
BLOSSOM_API blossom_error_t blossom_shim_sha256(const uint8_t *, size_t, uint8_t out[32]);
#ifdef __cplusplus
}
#endif
#endif
