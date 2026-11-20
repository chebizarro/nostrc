#ifndef FIPS_NOISE_CIPHER_H
#define FIPS_NOISE_CIPHER_H

#include <fips/noise/handshake.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caller-owned secret, no allocation. Fields are read-only outside this module.
 * Initialize from an already initialized AEAD key (borrowed, not consumed).
 * Destroy every owned copy; cleared/destroyed values cannot encrypt/decrypt.
 * Required pointers, lifetimes and synchronization follow host.h. No overlap
 * with state/key/AD/length is allowed; exact input == output is supported.
 * Empty input/AD may be NULL. out is required even for empty plaintext.
 * Constructors/clones/moves require an uninitialized or destroyed destination. */
typedef struct fips_cipher_state {
    fips_aead_key key;
    uint64_t counter;
    bool has_key;
} fips_cipher_state;

FIPS_CORE_API void fips_cipher_state_init(fips_cipher_state *out, const fips_aead_key *key);
FIPS_CORE_API uint64_t fips_cipher_state_counter(const fips_cipher_state *state);
/* Reserve a unique send nonce; MAX is never issued. out is unchanged on failure. */
FIPS_CORE_API fips_noise_status fips_cipher_state_take_counter(fips_cipher_state *state,
    uint64_t *out);
/* Auto-counter seal reserves before crypto, never rolling back a used nonce.
 * Length/capacity failures do not reserve. Length outputs change only on success.
 * MessageTooLarge covers output capacity and AD length, as in handshake.h.
 * Auth failure wipes the plaintext region, as in aead.h; no length is returned. */
FIPS_CORE_API fips_noise_status fips_cipher_state_seal(fips_cipher_state *state,
    const uint8_t *ad, size_t ad_len, const uint8_t *plaintext, size_t len,
    uint8_t *out, size_t cap, size_t *out_len);
/* Worker seal: counter MUST have been uniquely reserved under this key, across
 * every clone. Does not read/advance state->counter. MAX returns NonceOverflow. */
FIPS_CORE_API fips_noise_status fips_cipher_state_seal_with_counter(const fips_cipher_state *state,
    uint64_t counter, const uint8_t *ad, size_t ad_len, const uint8_t *plaintext,
    size_t len, uint8_t *out, size_t cap, size_t *out_len);
/* Explicit-counter open never advances the local counter; replay is session policy. */
FIPS_CORE_API fips_noise_status fips_cipher_state_open(const fips_cipher_state *state,
    uint64_t counter, const uint8_t *ad, size_t ad_len, const uint8_t *ciphertext,
    size_t len, uint8_t *out, size_t cap, size_t *out_len);
/* Move every byte to a distinct out then non-elidably wipe the source slot.
 * Equal pointers return WrongState unchanged. Moved secrets belong to out.
 * Clearing only erases this slot, not copies elsewhere (including registers). */
FIPS_CORE_API fips_noise_status fips_cipher_state_take_cleared(fips_cipher_state *slot,
    fips_cipher_state *out);
FIPS_CORE_API void fips_cipher_state_clear_slot(fips_cipher_state *slot);
FIPS_CORE_API void fips_cipher_state_destroy(fips_cipher_state *state);
#ifdef FIPS_TEST_HOOKS
/* Test-only overflow setup; absent from the production library. */
FIPS_CORE_API void fips_cipher_state_test_set_counter(fips_cipher_state *state, uint64_t counter);
#endif

#ifdef __cplusplus
}
#endif
#endif
