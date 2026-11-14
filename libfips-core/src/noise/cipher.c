#include <fips/noise/cipher.h>
#include <limits.h>
#include <sodium.h>
#include <string.h>

void fips_cipher_state_init(fips_cipher_state *out, const fips_aead_key *key)
{
    memset(out, 0, sizeof(*out));
    memcpy(&out->key, key, sizeof(*key));
    out->has_key = true;
}

uint64_t fips_cipher_state_counter(const fips_cipher_state *state)
{ return state->counter; }

fips_noise_status fips_cipher_state_take_counter(fips_cipher_state *state, uint64_t *out)
{
    if (!state->has_key) return FIPS_NOISE_WrongState;
    if (state->counter == UINT64_MAX) return FIPS_NOISE_NonceOverflow;
    *out = state->counter++;
    return FIPS_NOISE_OK;
}

static fips_noise_status seal_bounds(size_t ad_len, size_t len, size_t cap)
{
    if (len > FIPS_NOISE_MAX_MESSAGE_SIZE - FIPS_NOISE_TAG_SIZE ||
        cap < len + FIPS_NOISE_TAG_SIZE || ad_len > ULLONG_MAX)
        return FIPS_NOISE_MessageTooLarge;
    return FIPS_NOISE_OK;
}

fips_noise_status fips_cipher_state_seal_with_counter(const fips_cipher_state *state,
    uint64_t counter, const uint8_t *ad, size_t ad_len, const uint8_t *plaintext,
    size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    if (!state->has_key) return FIPS_NOISE_WrongState;
    fips_noise_status rc = seal_bounds(ad_len, len, cap);
    if (rc != FIPS_NOISE_OK) return rc;
    if (counter == UINT64_MAX) return FIPS_NOISE_NonceOverflow;
    fips_aead_status a = fips_aead_seal(&state->key, counter, ad, ad_len,
                                      plaintext, len, out, cap, out_len);
    return a == FIPS_AEAD_OK ? FIPS_NOISE_OK : FIPS_NOISE_EncryptionFailed;
}

fips_noise_status fips_cipher_state_seal(fips_cipher_state *state,
    const uint8_t *ad, size_t ad_len, const uint8_t *plaintext, size_t len,
    uint8_t *out, size_t cap, size_t *out_len)
{
    if (!state->has_key) return FIPS_NOISE_WrongState;
    fips_noise_status rc = seal_bounds(ad_len, len, cap);
    if (rc != FIPS_NOISE_OK) return rc;
    uint64_t counter;
    rc = fips_cipher_state_take_counter(state, &counter);
    if (rc != FIPS_NOISE_OK) return rc;
    return fips_cipher_state_seal_with_counter(state, counter, ad, ad_len,
                                             plaintext, len, out, cap, out_len);
}

fips_noise_status fips_cipher_state_open(const fips_cipher_state *state,
    uint64_t counter, const uint8_t *ad, size_t ad_len, const uint8_t *ciphertext,
    size_t len, uint8_t *out, size_t cap, size_t *out_len)
{
    if (!state->has_key) return FIPS_NOISE_WrongState;
    if (len > FIPS_NOISE_MAX_MESSAGE_SIZE || ad_len > ULLONG_MAX)
        return FIPS_NOISE_MessageTooLarge;
    if (len < FIPS_NOISE_TAG_SIZE) return FIPS_NOISE_MessageTooShort;
    if (cap < len - FIPS_NOISE_TAG_SIZE) return FIPS_NOISE_MessageTooLarge;
    fips_aead_status a = fips_aead_open(&state->key, counter, ad, ad_len,
                                      ciphertext, len, out, cap, out_len);
    return a == FIPS_AEAD_OK ? FIPS_NOISE_OK : FIPS_NOISE_DecryptionFailed;
}

fips_noise_status fips_cipher_state_take_cleared(fips_cipher_state *slot, fips_cipher_state *out)
{
    if (slot == out) return FIPS_NOISE_WrongState;
    memcpy(out, slot, sizeof(*out));
    fips_cipher_state_clear_slot(slot);
    return FIPS_NOISE_OK;
}

void fips_cipher_state_clear_slot(fips_cipher_state *slot)
{ sodium_memzero(slot, sizeof(*slot)); }

void fips_cipher_state_destroy(fips_cipher_state *state)
{ fips_cipher_state_clear_slot(state); }

#ifdef FIPS_TEST_HOOKS
void fips_cipher_state_test_set_counter(fips_cipher_state *state, uint64_t counter)
{ state->counter = counter; }
#endif
