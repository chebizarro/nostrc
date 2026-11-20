#ifndef FIPS_IDENTITY_INTERNAL_H
#define FIPS_IDENTITY_INTERNAL_H
#include <fips/identity/local.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>
#include <sodium.h>
#include <stdbool.h>
#include <string.h>
struct fips_secp { secp256k1_context *ctx; void *storage; };
struct fips_identity {
    secp256k1_keypair keypair;
    const fips_secp *secp;
    fips_entropy entropy;
    fips_xonly_pubkey pubkey;
    uint8_t full[33];
    fips_node_addr node_addr;
    fips_ipv6_address address;
};
/* libsodium's allocation-free SHA-256 primitive requires no initialization or
 * random source, unlike sodium_init. No crypto RNG API is used in this module. */
static inline void identity_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    if (len) crypto_hash_sha256_update(&state, data, (unsigned long long)len);
    crypto_hash_sha256_final(&state, out);
    sodium_memzero(&state, sizeof(state));
}
fips_identity_status identity_sign_digest(const fips_identity *id, const uint8_t digest[32], uint8_t out[64]);
#endif
