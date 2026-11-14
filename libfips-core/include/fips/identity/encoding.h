#ifndef FIPS_IDENTITY_ENCODING_H
#define FIPS_IDENTITY_ENCODING_H
#include <fips/identity/mod.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_NIP19_CAPACITY 64
/* Encoders emit 63 lowercase characters plus NUL. Decoders accept any ASCII
 * letter case (including mixed), but only original bech32, zero-padding NIP-19.
 * No URI prefixes, TLV forms, whitespace, embedded NUL or bech32m accepted.
 * Secret outputs/text remain caller-owned sensitive buffers: wipe after use. */
FIPS_CORE_API fips_identity_status fips_encode_npub(const fips_xonly_pubkey *key, char *out, size_t capacity);
FIPS_CORE_API fips_identity_status fips_decode_npub(const fips_secp *secp, const char *text, size_t len, fips_xonly_pubkey *out);
FIPS_CORE_API fips_identity_status fips_encode_nsec(const fips_secp *secp, const uint8_t secret[32], char *out, size_t capacity);
FIPS_CORE_API fips_identity_status fips_decode_nsec(const fips_secp *secp, const char *text, size_t len, uint8_t out[32]);
FIPS_CORE_API fips_identity_status fips_decode_secret(const fips_secp *secp, const char *text, size_t len, uint8_t out[32]);
#ifdef __cplusplus
}
#endif
#endif
