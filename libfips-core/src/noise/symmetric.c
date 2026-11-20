#include <fips/noise/symmetric.h>
#include "hkdf.h"
#include <string.h>

_Static_assert(sizeof(FIPS_NOISE_PROTOCOL_NAME_IK) - 1 == FIPS_NOISE_PROTOCOL_NAME_SIZE,
               "IK protocol name size");
_Static_assert(sizeof(FIPS_NOISE_PROTOCOL_NAME_XK) - 1 == FIPS_NOISE_PROTOCOL_NAME_SIZE,
               "XK protocol name size");
_Static_assert(FIPS_NOISE_HASH_SIZE == crypto_hash_sha256_BYTES, "SHA-256 size");
_Static_assert(FIPS_NOISE_HASH_SIZE == FIPS_AEAD_KEY_SIZE, "HKDF outputs are AEAD keys");
_Static_assert(FIPS_SYMMETRIC_ERR_CRYPTO == (int)FIPS_AEAD_ERR_CRYPTO &&
               FIPS_SYMMETRIC_ERR_AUTH == (int)FIPS_AEAD_ERR_AUTH &&
               FIPS_SYMMETRIC_ERR_RANGE == (int)FIPS_AEAD_ERR_RANGE &&
               FIPS_SYMMETRIC_OK == (int)FIPS_AEAD_OK, "AEAD statuses pass through");

static fips_symmetric_status from_aead(fips_aead_status rc)
{
    return (fips_symmetric_status)rc;
}

/* SHA-256(a || b); wipes its state like identity_sha256. out may alias a. */
static void sha256_pair(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len,
                        uint8_t out[FIPS_NOISE_HASH_SIZE])
{
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    if (a_len) crypto_hash_sha256_update(&state, a, (unsigned long long)a_len);
    if (b_len) crypto_hash_sha256_update(&state, b, (unsigned long long)b_len);
    crypto_hash_sha256_final(&state, out);
    sodium_memzero(&state, sizeof(state));
}

void fips_symmetric_init(fips_symmetric_state *state, const uint8_t *protocol_name, size_t len)
{
    memset(state, 0, sizeof(*state)); /* Empty cipher: no key, counter 0. */
    if (len <= FIPS_NOISE_HASH_SIZE) {
        if (len) memcpy(state->h, protocol_name, len);
    } else {
        sha256_pair(protocol_name, len, NULL, 0, state->h);
    }
    memcpy(state->ck, state->h, sizeof(state->ck));
}

void fips_symmetric_mix_hash(fips_symmetric_state *state, const uint8_t *data, size_t len)
{
    sha256_pair(state->h, sizeof(state->h), data, len, state->h);
}

fips_symmetric_status fips_symmetric_mix_key(fips_symmetric_state *state,
                                             const uint8_t *input_key_material, size_t len)
{
    uint8_t out[64];
    fips_symmetric_status rc;
    noise_hkdf2(state->ck, input_key_material, len, out);
    /* Key first: on failure the key is unchanged and ck has not moved. */
    rc = from_aead(fips_aead_key_init(&state->cipher.key, out + FIPS_NOISE_HASH_SIZE));
    if (rc == FIPS_SYMMETRIC_OK) {
        memcpy(state->ck, out, FIPS_NOISE_HASH_SIZE);
        state->cipher.nonce = 0;
        state->cipher.has_key = true;
    }
    sodium_memzero(out, sizeof(out));
    return rc;
}

/* Upstream advance_nonce: the reserved counter, or NonceOverflow at u64::MAX. */
static fips_symmetric_status advance(fips_symmetric_cipher *cipher, uint64_t *counter)
{
    if (cipher->nonce == UINT64_MAX) return FIPS_SYMMETRIC_ERR_NONCE_OVERFLOW;
    *counter = cipher->nonce;
    cipher->nonce += 1;
    return FIPS_SYMMETRIC_OK;
}

/* Upstream CipherState without a key: the bytes pass through unchanged. */
static fips_symmetric_status passthrough(fips_symmetric_state *state, const uint8_t *in,
                                         size_t len, uint8_t *out, size_t out_cap,
                                         size_t *out_len)
{
    if (out_cap < len) return FIPS_SYMMETRIC_ERR_RANGE;
    if (len) memmove(out, in, len);
    fips_symmetric_mix_hash(state, out, len);
    *out_len = len;
    return FIPS_SYMMETRIC_OK;
}

fips_symmetric_status fips_symmetric_encrypt_and_hash(
    fips_symmetric_state *state, const uint8_t *plaintext, size_t len,
    uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint64_t counter;
    fips_symmetric_status rc;
    if (!state->cipher.has_key)
        return passthrough(state, plaintext, len, out, out_cap, out_len);
    if (len > FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE ||
        out_cap < len + FIPS_NOISE_TAG_SIZE)
        return FIPS_SYMMETRIC_ERR_RANGE;
    rc = advance(&state->cipher, &counter);
    if (rc != FIPS_SYMMETRIC_OK) return rc;
    rc = from_aead(fips_aead_seal(&state->cipher.key, counter, NULL, 0, plaintext, len,
                                  out, out_cap, out_len));
    if (rc != FIPS_SYMMETRIC_OK) return rc;
    fips_symmetric_mix_hash(state, out, *out_len);
    return FIPS_SYMMETRIC_OK;
}

fips_symmetric_status fips_symmetric_decrypt_and_hash(
    fips_symmetric_state *state, const uint8_t *ciphertext, size_t len,
    uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t next_h[FIPS_NOISE_HASH_SIZE];
    uint64_t counter;
    fips_symmetric_status rc;
    if (!state->cipher.has_key)
        return passthrough(state, ciphertext, len, out, out_cap, out_len);
    if (len < FIPS_NOISE_TAG_SIZE || len > FIPS_NOISE_MAX_MESSAGE_SIZE ||
        out_cap < len - FIPS_NOISE_TAG_SIZE)
        return FIPS_SYMMETRIC_ERR_RANGE;
    rc = advance(&state->cipher, &counter);
    if (rc != FIPS_SYMMETRIC_OK) return rc;
    /* Hash the ciphertext before open overwrites it in place; commit on success. */
    sha256_pair(state->h, sizeof(state->h), ciphertext, len, next_h);
    rc = from_aead(fips_aead_open(&state->cipher.key, counter, NULL, 0, ciphertext, len,
                                  out, out_cap, out_len));
    if (rc == FIPS_SYMMETRIC_OK) memcpy(state->h, next_h, sizeof(next_h));
    sodium_memzero(next_h, sizeof(next_h));
    return rc;
}

fips_symmetric_status fips_symmetric_split(const fips_symmetric_state *state,
                                           fips_aead_key *k1, fips_aead_key *k2)
{
    uint8_t out[64];
    fips_symmetric_status rc;
    if (k1 == k2) return FIPS_SYMMETRIC_ERR_RANGE; /* Would silently drop k1. */
    noise_hkdf2(state->ck, NULL, 0, out);
    rc = from_aead(fips_aead_key_init(k1, out));
    if (rc == FIPS_SYMMETRIC_OK) {
        rc = from_aead(fips_aead_key_init(k2, out + FIPS_NOISE_HASH_SIZE));
        if (rc != FIPS_SYMMETRIC_OK) fips_aead_key_destroy(k1);
    }
    sodium_memzero(out, sizeof(out));
    return rc;
}

void fips_symmetric_copy(fips_symmetric_state *dst, const fips_symmetric_state *src)
{
    /* Plain data, so assignment would do; memcpy also copies the padding, which
     * keeps a fork byte-identical to its original (tests compare whole structs). */
    if (dst != src) memcpy(dst, src, sizeof(*dst));
}

void fips_symmetric_destroy(fips_symmetric_state *state)
{
    sodium_memzero(state, sizeof(*state));
}
