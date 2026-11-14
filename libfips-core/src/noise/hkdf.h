#ifndef FIPS_NOISE_HKDF_H
#define FIPS_NOISE_HKDF_H

/* Private HMAC-SHA256 and HKDF-SHA256 (RFC 2104, RFC 5869) over libsodium's
 * allocation-free crypto_auth_hmacsha256, shared with the RFC 4231/5869 tests.
 * Noise rev 34 section 4.3 HKDF(ck, ikm, n) is extract(salt = ck, ikm) then
 * expand(prk, info = "", 32 * n), which is also the hkdf crate's
 * Hkdf::new(Some(&ck), ikm).expand(&[], out) that upstream handshake.rs runs.
 * Not installed or exported. okm_len must not exceed 255 * 32 (the callers
 * here ask for 64). Every HMAC state, PRK and derived block is wiped before
 * returning; outputs are the caller's to wipe. Pointers may be NULL only with
 * a zero length. */
#include <sodium.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(crypto_auth_hmacsha256_BYTES == 32, "HMAC-SHA256 output size");

static inline void noise_hmac_sha256(const uint8_t *key, size_t key_len,
                                     const uint8_t *data, size_t len, uint8_t out[32])
{
    crypto_auth_hmacsha256_state state;
    crypto_auth_hmacsha256_init(&state, key, key_len);
    if (len) crypto_auth_hmacsha256_update(&state, data, (unsigned long long)len);
    crypto_auth_hmacsha256_final(&state, out);
    sodium_memzero(&state, sizeof(state));
}

static inline void noise_hkdf_extract(const uint8_t *salt, size_t salt_len,
                                      const uint8_t *ikm, size_t ikm_len, uint8_t prk[32])
{
    noise_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

static inline void noise_hkdf_expand(const uint8_t prk[32], const uint8_t *info,
                                     size_t info_len, uint8_t *okm, size_t okm_len)
{
    uint8_t block[32];
    size_t block_len = 0;
    uint8_t counter = 1;
    while (okm_len) {
        crypto_auth_hmacsha256_state state;
        size_t n = okm_len < sizeof(block) ? okm_len : sizeof(block);
        crypto_auth_hmacsha256_init(&state, prk, 32);
        if (block_len) crypto_auth_hmacsha256_update(&state, block, (unsigned long long)block_len);
        if (info_len) crypto_auth_hmacsha256_update(&state, info, (unsigned long long)info_len);
        crypto_auth_hmacsha256_update(&state, &counter, 1);
        crypto_auth_hmacsha256_final(&state, block);
        sodium_memzero(&state, sizeof(state));
        memcpy(okm, block, n);
        okm += n;
        okm_len -= n;
        block_len = sizeof(block);
        ++counter;
    }
    sodium_memzero(block, sizeof(block));
}

/* Noise HKDF with two outputs, as MixKey and Split use it. */
static inline void noise_hkdf2(const uint8_t ck[32], const uint8_t *ikm, size_t ikm_len,
                               uint8_t out[64])
{
    uint8_t prk[32];
    noise_hkdf_extract(ck, 32, ikm, ikm_len, prk);
    noise_hkdf_expand(prk, NULL, 0, out, 64);
    sodium_memzero(prk, sizeof(prk));
}
#endif
