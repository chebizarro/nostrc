#ifndef FIPS_AEAD_PRIMITIVE_H
#define FIPS_AEAD_PRIMITIVE_H

/* Private full-nonce IETF ChaCha20-Poly1305 primitive (RFC 8439: 12-byte nonce,
 * 16-byte tag appended), shared with RFC tests: section 2.8.2 has a nonzero
 * nonce prefix which the public Noise counter API cannot represent. Not
 * installed or exported. Bounds/initialization are checked by the caller
 * (len <= FIPS_NOISE_MAX_MESSAGE_SIZE; open requires len >= tag size).
 *
 * The backend is fixed at build time by FIPS_AEAD_BACKEND (aead_primitive.c):
 * OpenSSL EVP (default) or libsodium. Both produce identical bytes. */
#include <fips/noise/aead.h>

/* Returns 0, or FIPS_AEAD_PRIMITIVE_ERR_CRYPTO with the out..out+len+16 region
 * zeroed (exact in-place, in == out, is allowed). */
int fips_aead_seal_nonce(
    const fips_aead_key *key, const uint8_t nonce[FIPS_NOISE_NONCE_SIZE],
    const uint8_t *ad, size_t ad_len, const uint8_t *in, size_t len, uint8_t *out);

/* Returns 0, FIPS_AEAD_PRIMITIVE_ERR_AUTH on tag mismatch, or
 * FIPS_AEAD_PRIMITIVE_ERR_CRYPTO on a backend failure. On any failure the
 * len - 16 plaintext bytes at out are zeroed and nothing else is written. */
int fips_aead_open_nonce(
    const fips_aead_key *key, const uint8_t nonce[FIPS_NOISE_NONCE_SIZE],
    const uint8_t *ad, size_t ad_len, const uint8_t *in, size_t len, uint8_t *out);

#define FIPS_AEAD_PRIMITIVE_ERR_AUTH (-1)
#define FIPS_AEAD_PRIMITIVE_ERR_CRYPTO (-2)

/* "openssl" or "sodium", for test/bench labels. */
const char *fips_aead_backend_name(void);
#endif
