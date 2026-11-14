#ifndef FIPS_NOISE_AEAD_H
#define FIPS_NOISE_AEAD_H

#include <fips/host.h>
#include <fips/noise/mod.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIPS_AEAD_KEY_SIZE 32u

typedef enum fips_aead_status {
    FIPS_AEAD_OK = FIPS_OK,
    FIPS_AEAD_ERR_RANGE = FIPS_ERR_RANGE,
    FIPS_AEAD_ERR_AUTH = 2,
    FIPS_AEAD_ERR_CRYPTO = 3
} fips_aead_status;

/* Caller-owned secret. Do not log or implicitly copy; destroy every owned copy.
 * Initialized keys may be borrowed concurrently; init/destroy require exclusive
 * access. A destroyed key must not be used until initialized again. */
typedef struct fips_aead_key {
    uint8_t bytes[FIPS_AEAD_KEY_SIZE];
} fips_aead_key;

/* Lazy initialization: calls sodium_init() on key construction, with no local
 * once flag. Hosted libsodium provides thread-safe, idempotent initialization.
 * Unlike seal/open, sodium_init may use OS services/entropy; hosts requiring a
 * strictly IO-free core boundary must initialize libsodium before entering core.
 * On init failure key is unchanged. bytes may point to key->bytes. */
FIPS_CORE_API fips_aead_status fips_aead_key_init(
    fips_aead_key *key, const uint8_t bytes[FIPS_AEAD_KEY_SIZE]);
FIPS_CORE_API void fips_aead_key_destroy(fips_aead_key *key);

/* Wire format: four zero bytes, then counter as a little-endian u64. */
FIPS_CORE_API void fips_aead_nonce(uint64_t counter,
                                 uint8_t out[FIPS_NOISE_NONCE_SIZE]);

/* Seal writes ciphertext || 16-byte tag. The caller MUST NOT reuse a counter
 * with the same key (including across copies/workers), nor allow it to wrap.
 * This primitive tracks no counters/replay state; CipherState owns that policy.
 * Both calls allocate nothing and accept NULL ad/input only for zero length.
 * out must be non-NULL even for empty plaintext; exact in-place input == out is
 * supported, but other overlap, including AD/key/out_len with out, is forbidden.
 * Required pointers and initialized keys are caller preconditions (host.h).
 * RANGE (length/space) errors leave all output untouched; out_len changes only
 * on success. open AUTH errors zero exactly len - TAG_SIZE output bytes (zero
 * bytes for a truncated tag), never returning unauthenticated plaintext. This
 * strengthens upstream's unspecified in-place failure contents. */
FIPS_CORE_API fips_aead_status fips_aead_seal(
    const fips_aead_key *key, uint64_t counter, const uint8_t *ad, size_t ad_len,
    const uint8_t *plaintext, size_t len, uint8_t *out, size_t out_cap,
    size_t *out_len);
FIPS_CORE_API fips_aead_status fips_aead_open(
    const fips_aead_key *key, uint64_t counter, const uint8_t *ad, size_t ad_len,
    const uint8_t *ciphertext, size_t len, uint8_t *out, size_t out_cap,
    size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif
