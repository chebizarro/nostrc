#include <fips/noise/aead.h>
#include "aead_primitive.h"
#include <limits.h>
#include <string.h>

fips_aead_status fips_aead_key_init(fips_aead_key *key, const uint8_t bytes[32])
{
    /* libsodium owns its initialization synchronization; no racy global flag. */
    if (sodium_init() < 0) return FIPS_AEAD_ERR_CRYPTO;
    memmove(key->bytes, bytes, sizeof(key->bytes));
    return FIPS_AEAD_OK;
}

void fips_aead_key_destroy(fips_aead_key *key)
{
    sodium_memzero(key, sizeof(*key));
}

void fips_aead_nonce(uint64_t counter, uint8_t out[FIPS_NOISE_NONCE_SIZE])
{
    memset(out, 0, 4);
    for (unsigned i = 0; i < 8; ++i) out[4 + i] = (uint8_t)(counter >> (8 * i));
}

fips_aead_status fips_aead_seal(
    const fips_aead_key *key, uint64_t counter, const uint8_t *ad, size_t ad_len,
    const uint8_t *plaintext, size_t len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (len > FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE ||
        out_cap < len + FIPS_NOISE_TAG_SIZE || ad_len > ULLONG_MAX)
        return FIPS_AEAD_ERR_RANGE;
    uint8_t nonce[FIPS_NOISE_NONCE_SIZE];
    fips_aead_nonce(counter, nonce);
    if (fips_aead_seal_nonce(key, nonce, ad, ad_len, plaintext, len, out) != 0)
        return FIPS_AEAD_ERR_CRYPTO;
    *out_len = len + FIPS_NOISE_TAG_SIZE;
    return FIPS_AEAD_OK;
}

fips_aead_status fips_aead_open(
    const fips_aead_key *key, uint64_t counter, const uint8_t *ad, size_t ad_len,
    const uint8_t *ciphertext, size_t len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (len > FIPS_NOISE_MAX_MESSAGE_SIZE || ad_len > ULLONG_MAX)
        return FIPS_AEAD_ERR_RANGE;
    if (len < FIPS_NOISE_TAG_SIZE) return FIPS_AEAD_ERR_AUTH;
    size_t plain_len = len - FIPS_NOISE_TAG_SIZE;
    if (out_cap < plain_len) return FIPS_AEAD_ERR_RANGE;
    uint8_t nonce[FIPS_NOISE_NONCE_SIZE];
    fips_aead_nonce(counter, nonce);
    if (fips_aead_open_nonce(key, nonce, ad, ad_len, ciphertext, len, out) != 0)
        return FIPS_AEAD_ERR_AUTH;
    *out_len = plain_len;
    return FIPS_AEAD_OK;
}
