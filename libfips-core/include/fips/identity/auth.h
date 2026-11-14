#ifndef FIPS_IDENTITY_AUTH_H
#define FIPS_IDENTITY_AUTH_H
#include <fips/identity/mod.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct fips_auth_challenge { uint8_t bytes[32]; } fips_auth_challenge;
typedef struct fips_auth_response {
    fips_xonly_pubkey pubkey;
    uint64_t timestamp;
    uint8_t signature[64];
} fips_auth_response;
FIPS_CORE_API fips_auth_challenge fips_auth_challenge_from_bytes(const uint8_t bytes[32]);
FIPS_CORE_API fips_auth_challenge fips_auth_challenge_generate(fips_entropy entropy);
/* SHA256("fips-auth-v1" || challenge || timestamp_be64). No wire serialization
 * of AuthResponse is defined upstream; these are values, not packed wire structs.
 * Verification proves key ownership only; host enforces freshness/replay policy. */
FIPS_CORE_API fips_identity_status fips_identity_sign_challenge(const fips_identity *identity, const fips_auth_challenge *challenge, uint64_t timestamp, fips_auth_response *out);
FIPS_CORE_API fips_identity_status fips_auth_challenge_verify(const fips_secp *secp, const fips_auth_challenge *challenge, const fips_auth_response *response, fips_node_addr *out);
#ifdef __cplusplus
}
#endif
#endif
