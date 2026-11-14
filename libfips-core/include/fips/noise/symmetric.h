#ifndef FIPS_NOISE_SYMMETRIC_H
#define FIPS_NOISE_SYMMETRIC_H

#include <fips/noise/aead.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pinned upstream noise/cipher.rs PROTOCOL_NAME_{IK,XK}. Both are 36 bytes, so
 * their initial h is SHA-256(name); a name of 32 bytes or fewer is zero-padded. */
#define FIPS_NOISE_PROTOCOL_NAME_IK "Noise_IK_secp256k1_ChaChaPoly_SHA256"
#define FIPS_NOISE_PROTOCOL_NAME_XK "Noise_XK_secp256k1_ChaChaPoly_SHA256"
#define FIPS_NOISE_PROTOCOL_NAME_SIZE 36u
#define FIPS_NOISE_HASH_SIZE 32u

typedef enum fips_symmetric_status {
    FIPS_SYMMETRIC_OK = FIPS_AEAD_OK,
    /* Upstream MessageTooLarge/MessageTooShort, plus insufficient out_cap. */
    FIPS_SYMMETRIC_ERR_RANGE = FIPS_AEAD_ERR_RANGE,
    FIPS_SYMMETRIC_ERR_AUTH = FIPS_AEAD_ERR_AUTH,     /* DecryptionFailed */
    FIPS_SYMMETRIC_ERR_CRYPTO = FIPS_AEAD_ERR_CRYPTO, /* EncryptionFailed, key init */
    FIPS_SYMMETRIC_ERR_NONCE_OVERFLOW = 4
} fips_symmetric_status;

/* The handshake-phase CipherState: key is meaningful iff has_key (zero
 * otherwise), nonce is the next counter. The transport CipherState with its
 * replay and rekey policy is a separate port. */
typedef struct fips_symmetric_cipher {
    fips_aead_key key;
    uint64_t nonce;
    bool has_key;
} fips_symmetric_cipher;

/* Upstream SymmetricState: ck is the chaining key, h the handshake hash
 * (upstream handshake_hash(); read it directly). Caller-owned secret plain
 * data with no pointers: do not log it, destroy every copy, and treat the
 * fields as read-only outside this module. Exclusive access per state. */
typedef struct fips_symmetric_state {
    uint8_t ck[FIPS_NOISE_HASH_SIZE];
    uint8_t h[FIPS_NOISE_HASH_SIZE];
    fips_symmetric_cipher cipher;
} fips_symmetric_state;

/* h = protocol_name zero-padded to 32 bytes, or SHA-256(protocol_name) when
 * longer; ck = h; the cipher is empty. protocol_name may be NULL only for len 0. */
FIPS_CORE_API void fips_symmetric_init(fips_symmetric_state *state,
                                     const uint8_t *protocol_name, size_t len);
/* h = SHA-256(h || data). data may be NULL only when len is zero. */
FIPS_CORE_API void fips_symmetric_mix_hash(fips_symmetric_state *state,
                                         const uint8_t *data, size_t len);
/* (ck, k) = HKDF(ck, input_key_material, 2); the cipher is keyed with k at
 * counter 0. ERR_CRYPTO comes from key construction (fips_aead_key_init) and
 * leaves the state unchanged. input_key_material may be NULL only for len 0. */
FIPS_CORE_API fips_symmetric_status fips_symmetric_mix_key(
    fips_symmetric_state *state, const uint8_t *input_key_material, size_t len);
/* Without a key, out is the plaintext (len bytes). With a key, out is
 * seal(counter, ad = "", plaintext) (len + 16 bytes) at the cipher's counter,
 * which is consumed. Then h = SHA-256(h || out). ERR_RANGE (len > 65519 with a
 * key, or out_cap too small) changes nothing, and so does ERR_NONCE_OVERFLOW;
 * ERR_CRYPTO has consumed the counter. Exact in-place out == plaintext is
 * supported; other overlap is not. out must be non-NULL even for len 0. */
FIPS_CORE_API fips_symmetric_status fips_symmetric_encrypt_and_hash(
    fips_symmetric_state *state, const uint8_t *plaintext, size_t len,
    uint8_t *out, size_t out_cap, size_t *out_len);
/* Without a key, out is the ciphertext (len bytes). With a key, len < 16
 * (upstream MessageTooShort), len > 65535 or out_cap < len - 16 is ERR_RANGE
 * and changes nothing; otherwise the counter is consumed and the message
 * opened. ERR_AUTH leaves h unchanged, zeroes len - 16 bytes of out
 * (fips_aead_open) and has consumed the counter, exactly as upstream: a read
 * that may fail is run on a copy (Fork) and the copy is discarded. On success
 * h = SHA-256(h || ciphertext). Exact in-place out == ciphertext is supported. */
FIPS_CORE_API fips_symmetric_status fips_symmetric_decrypt_and_hash(
    fips_symmetric_state *state, const uint8_t *ciphertext, size_t len,
    uint8_t *out, size_t out_cap, size_t *out_len);
/* (k1, k2) = HKDF(ck, "", 2): the initiator-to-responder key, then the
 * responder-to-initiator key, each a fresh caller-owned AEAD key the caller
 * destroys. The state is unchanged and may be split again. k1 and k2 must be
 * distinct objects: k1 == k2 is ERR_RANGE and writes nothing. On ERR_CRYPTO
 * neither key is left initialized. */
FIPS_CORE_API fips_symmetric_status fips_symmetric_split(
    const fips_symmetric_state *state, fips_aead_key *k1, fips_aead_key *k2);
/* The Fork copy. The state is plain data with no pointers, so this is a byte
 * copy (*dst = *src is equivalent; dst == src is a no-op); both copies must be
 * destroyed. */
FIPS_CORE_API void fips_symmetric_copy(fips_symmetric_state *dst,
                                     const fips_symmetric_state *src);
/* Wipes ck, h and the cipher key (the whole struct). The state is then empty
 * and must be initialized again before use. */
FIPS_CORE_API void fips_symmetric_destroy(fips_symmetric_state *state);

#ifdef __cplusplus
}
#endif
#endif
