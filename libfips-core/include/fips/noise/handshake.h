#ifndef FIPS_NOISE_HANDSHAKE_H
#define FIPS_NOISE_HANDSHAKE_H

#include <fips/identity/local.h>
#include <fips/noise/symmetric.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIPS_NOISE_PUBKEY_SIZE 33u
#define FIPS_NOISE_EPOCH_SIZE 8u
#define FIPS_NOISE_EPOCH_ENCRYPTED_SIZE (FIPS_NOISE_EPOCH_SIZE + FIPS_NOISE_TAG_SIZE)
#define FIPS_NOISE_HANDSHAKE_MSG1_SIZE (2u * FIPS_NOISE_PUBKEY_SIZE + FIPS_NOISE_TAG_SIZE + FIPS_NOISE_EPOCH_ENCRYPTED_SIZE)
#define FIPS_NOISE_HANDSHAKE_MSG2_SIZE (FIPS_NOISE_PUBKEY_SIZE + FIPS_NOISE_EPOCH_ENCRYPTED_SIZE)
#define FIPS_NOISE_XK_HANDSHAKE_MSG1_SIZE FIPS_NOISE_PUBKEY_SIZE
#define FIPS_NOISE_XK_HANDSHAKE_MSG2_SIZE FIPS_NOISE_HANDSHAKE_MSG2_SIZE
#define FIPS_NOISE_XK_HANDSHAKE_MSG3_SIZE (FIPS_NOISE_PUBKEY_SIZE + FIPS_NOISE_TAG_SIZE + FIPS_NOISE_EPOCH_ENCRYPTED_SIZE)

/* NoiseError variants at the upstream pin. Secp256k1 includes invalid host
 * entropy (no retry); MessageTooLarge also represents C output-capacity errors.
 * A handshake read requires EXACT length; any mismatch is MessageTooShort,
 * including overlong input, matching upstream. */
typedef enum fips_noise_status {
    FIPS_NOISE_OK = 0,
    FIPS_NOISE_HandshakeNotComplete,
    FIPS_NOISE_HandshakeAlreadyComplete,
    FIPS_NOISE_WrongState,
    FIPS_NOISE_InvalidPublicKey,
    FIPS_NOISE_DecryptionFailed,
    FIPS_NOISE_EncryptionFailed,
    FIPS_NOISE_MessageTooLarge,
    FIPS_NOISE_MessageTooShort,
    FIPS_NOISE_NonceOverflow,
    FIPS_NOISE_ReplayDetected,
    FIPS_NOISE_Secp256k1
} fips_noise_status;
typedef enum fips_handshake_role {
    FIPS_HANDSHAKE_INITIATOR, FIPS_HANDSHAKE_RESPONDER
} fips_handshake_role;
typedef enum fips_noise_pattern { FIPS_NOISE_IK, FIPS_NOISE_XK } fips_noise_pattern;
typedef enum fips_handshake_progress {
    FIPS_HANDSHAKE_INITIAL, FIPS_HANDSHAKE_MESSAGE1_DONE,
    FIPS_HANDSHAKE_MESSAGE2_DONE, FIPS_HANDSHAKE_COMPLETE
} fips_handshake_progress;

/* Caller-owned secret; fields are read-only outside this module. The blinded
 * context, static identity and entropy context are BORROWED and must outlive
 * every fork. No heap allocation; only ephemeral_secret, es and symmetric hold
 * owned secrets. Destroy every copy, including a displaced state when committing
 * a fork. Exclusive access per mutable state; no logging/implicit copies. */
typedef struct fips_handshake_state {
    fips_noise_pattern pattern;
    fips_handshake_role role;
    fips_handshake_progress progress;
    fips_symmetric_state symmetric;
    const fips_secp *secp;
    const fips_identity *static_keypair;
    fips_entropy entropy;
    uint8_t ephemeral_secret[32], ephemeral_public[FIPS_NOISE_PUBKEY_SIZE];
    uint8_t remote_static[FIPS_NOISE_PUBKEY_SIZE];
    uint8_t remote_ephemeral[FIPS_NOISE_PUBKEY_SIZE];
    uint8_t es[32]; /* IK initiator's msg1 es, mixed again in msg2 (NOT se). */
    uint8_t local_epoch[FIPS_NOISE_EPOCH_SIZE], remote_epoch[FIPS_NOISE_EPOCH_SIZE];
    bool has_ephemeral, has_remote_static, has_remote_ephemeral, has_es, has_remote_epoch;
} fips_handshake_state;

/* Constructors preserve out on error and draw no entropy. The ephemeral is
 * drawn in one 32-byte fill at write_1/write_2, in transcript order. The static
 * identity must belong to secp. Epoch is required, unlike upstream's late setter.
 * Initiators accept either valid compressed parity for the responder static:
 * only the <- s pre-message is normalized to even parity. */
FIPS_CORE_API fips_noise_status fips_handshake_new_initiator(fips_handshake_state *out,
    const fips_secp *secp, const fips_identity *key, const uint8_t remote_static[33],
    const uint8_t epoch[8], fips_entropy entropy);
FIPS_CORE_API fips_noise_status fips_handshake_new_responder(fips_handshake_state *out,
    const fips_secp *secp, const fips_identity *key, const uint8_t epoch[8], fips_entropy entropy);
FIPS_CORE_API fips_noise_status fips_handshake_new_xk_initiator(fips_handshake_state *out,
    const fips_secp *secp, const fips_identity *key, const uint8_t remote_static[33],
    const uint8_t epoch[8], fips_entropy entropy);
FIPS_CORE_API fips_noise_status fips_handshake_new_xk_responder(fips_handshake_state *out,
    const fips_secp *secp, const fips_identity *key, const uint8_t epoch[8], fips_entropy entropy);

/* Upstream pattern-specific entrypoints; wrong role/pattern/progress returns
 * WrongState before work. Output bytes/length are unchanged on errors. Capacity
 * errors draw no entropy and change no state. Other raw read/write failures may
 * advance state, as upstream: discard that state. NEVER retry a raw failed read.
 * For reads of retained states, use Fork and commit only on success.
 * Required pointers are caller preconditions; input may be NULL only for len 0.
 * Buffers and states must not overlap (except the explicit fork self-copy). */
#define FIPS_HANDSHAKE_MESSAGES(suffix) \
FIPS_CORE_API fips_noise_status fips_handshake_write_##suffix(fips_handshake_state *state, \
    uint8_t *out, size_t cap, size_t *len); \
FIPS_CORE_API fips_noise_status fips_handshake_read_##suffix(fips_handshake_state *state, \
    const uint8_t *input, size_t len)
FIPS_HANDSHAKE_MESSAGES(message_1);
FIPS_HANDSHAKE_MESSAGES(message_2);
FIPS_HANDSHAKE_MESSAGES(xk_message_1);
FIPS_HANDSHAKE_MESSAGES(xk_message_2);
FIPS_HANDSHAKE_MESSAGES(xk_message_3);
#undef FIPS_HANDSHAKE_MESSAGES

/* Hash is available at every step, not an authenticated channel binding: pinned
 * FIPS v1 uses EMPTY AEAD AAD, not h. XK msg1 has NO authentication at all, and
 * flipping a compressed ephemeral's parity leaves x-only DH unchanged. */
FIPS_CORE_API const uint8_t *fips_handshake_hash(const fips_handshake_state *state);
FIPS_CORE_API const char *fips_handshake_progress_name(fips_handshake_progress progress);
/* Byte copy, including padding; self-copy is a no-op. dst must be uninitialized
 * or already destroyed. Each copy owns its secrets, but not the borrowed keys. */
FIPS_CORE_API void fips_handshake_fork(const fips_handshake_state *src, fips_handshake_state *dst);
/* src and dst must be distinct; equal pointers return WrongState unchanged.
 * Read on a local copy, publish dst only on success, wipe local on EVERY path.
 * Both src (including padding) and dst are unchanged on failure. On success src
 * is still byte-identical; caller explicitly destroys/replaces it to commit. */
FIPS_CORE_API fips_noise_status fips_handshake_fork_msg2(const fips_handshake_state *src,
    const uint8_t *input, size_t len, fips_handshake_state *dst);
FIPS_CORE_API fips_noise_status fips_handshake_fork_xk2(const fips_handshake_state *src,
    const uint8_t *input, size_t len, fips_handshake_state *dst);
FIPS_CORE_API fips_noise_status fips_handshake_fork_xk3(const fips_handshake_state *src,
    const uint8_t *input, size_t len, fips_handshake_state *dst);
/* into_session stand-in: split directionally, export final metadata, then wipe
 * state on success. On failure state and outputs are unchanged. Outputs must
 * not overlap each other or state; send == recv is rejected as WrongState.
 * The caller owns and must destroy the two returned keys. */
FIPS_CORE_API fips_noise_status fips_handshake_finish(fips_handshake_state *state,
    fips_aead_key *send, fips_aead_key *recv, uint8_t final_h[32],
    uint8_t remote_static[33], uint8_t remote_epoch[8]);
FIPS_CORE_API void fips_handshake_destroy(fips_handshake_state *state);

#ifdef __cplusplus
}
#endif
#endif
