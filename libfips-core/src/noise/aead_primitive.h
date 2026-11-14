#ifndef FIPS_AEAD_PRIMITIVE_H
#define FIPS_AEAD_PRIMITIVE_H

/* Private full-nonce primitive, shared with RFC tests: section 2.8.2 has a
 * nonzero nonce prefix which the public Noise counter API cannot represent.
 * Not installed or exported. Bounds/initialization are checked by the caller. */
#include <fips/noise/aead.h>
#include <sodium.h>

_Static_assert(FIPS_AEAD_KEY_SIZE == crypto_aead_chacha20poly1305_ietf_KEYBYTES,
               "ChaChaPoly key size");
_Static_assert(FIPS_NOISE_NONCE_SIZE == crypto_aead_chacha20poly1305_ietf_NPUBBYTES,
               "ChaChaPoly nonce size");
_Static_assert(FIPS_NOISE_TAG_SIZE == crypto_aead_chacha20poly1305_ietf_ABYTES,
               "ChaChaPoly tag size");

static inline int fips_aead_seal_nonce(
    const fips_aead_key *key, const uint8_t nonce[FIPS_NOISE_NONCE_SIZE],
    const uint8_t *ad, size_t ad_len, const uint8_t *in, size_t len, uint8_t *out)
{
    return crypto_aead_chacha20poly1305_ietf_encrypt(
        out, NULL, in, (unsigned long long)len, ad, (unsigned long long)ad_len,
        NULL, nonce, key->bytes);
}

static inline int fips_aead_open_nonce(
    const fips_aead_key *key, const uint8_t nonce[FIPS_NOISE_NONCE_SIZE],
    const uint8_t *ad, size_t ad_len, const uint8_t *in, size_t len, uint8_t *out)
{
    int rc = crypto_aead_chacha20poly1305_ietf_decrypt(
        out, NULL, NULL, in, (unsigned long long)len, ad,
        (unsigned long long)ad_len, nonce, key->bytes);
    if (rc != 0) sodium_memzero(out, len - FIPS_NOISE_TAG_SIZE);
    return rc;
}
#endif
