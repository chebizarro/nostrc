#ifndef FIPS_IDENTITY_LOCAL_H
#define FIPS_IDENTITY_LOCAL_H
#include <fips/identity/mod.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Opaque, noncopyable owner of a keypair. Creation allocates once, destroy wipes
 * the entire object with sodium_memzero before freeing. No secret-bearing debug
 * or printable struct. Entropy context is borrowed and must outlive the identity.
 * Generation draws once; invalid host output returns InvalidSecretKey, no retry. */
FIPS_CORE_API fips_identity_status fips_identity_from_secret_bytes(const fips_secp *secp, fips_entropy entropy, const uint8_t secret[32], fips_identity **out);
FIPS_CORE_API fips_identity_status fips_identity_from_secret_str(const fips_secp *secp, fips_entropy entropy, const char *text, size_t len, fips_identity **out);
FIPS_CORE_API fips_identity_status fips_identity_generate(const fips_secp *secp, fips_entropy entropy, fips_identity **out);
FIPS_CORE_API void fips_identity_destroy(fips_identity *identity);
FIPS_CORE_API fips_xonly_pubkey fips_identity_pubkey(const fips_identity *identity);
FIPS_CORE_API void fips_identity_pubkey_full(const fips_identity *identity, uint8_t out[33]);
FIPS_CORE_API fips_node_addr fips_identity_node_addr(const fips_identity *identity);
FIPS_CORE_API fips_ipv6_address fips_identity_address(const fips_identity *identity);
/* Explicit sensitive export for host key persistence/Noise: caller must wipe. */
FIPS_CORE_API void fips_identity_export_secret(const fips_identity *identity, uint8_t out[32]);
FIPS_CORE_API fips_identity_status fips_identity_npub(const fips_identity *identity, char *out, size_t capacity);
/* SHA256(data), BIP-340 with 32 host auxiliary bytes per call. */
FIPS_CORE_API fips_identity_status fips_identity_sign(const fips_identity *identity, const uint8_t *data, size_t len, uint8_t signature[64]);
#ifdef __cplusplus
}
#endif
#endif
