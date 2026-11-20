#ifndef FIPS_NOISE_ECDH_H
#define FIPS_NOISE_ECDH_H
#include <fips/identity/mod.h>
#ifdef __cplusplus
extern "C" {
#endif
/* SHA-256 of the shared point's 32-byte big-endian x-coordinate; discard y.
 * Borrows the identity layer's host-blinded context. Accepts static or ephemeral
 * secret scalars and compressed public keys (02/03 || x), in either role.
 * Required pointers must be non-NULL. Invalid scalar/public bytes return
 * FIPS_IDENTITY_InvalidSecretKey and leave out unchanged. No allocation or RNG.
 * The caller owns and must wipe secret_key and the successful output. */
FIPS_CORE_API fips_identity_status fips_noise_ecdh(const fips_secp *ctx,
    const uint8_t secret_key[32], const uint8_t public_key[33], uint8_t out[32]);
#ifdef FIPS_TEST_HOOKS
/* Per-thread calls, including rejected keys. Absent from production builds. */
FIPS_CORE_API uint64_t fips_noise_dh_ops(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
