#include "internal.h"
#include <fips/identity/node_addr.h>
#include <fips/identity/address.h>
#include <fips/identity/encoding.h>
#include <stdlib.h>

fips_identity_status fips_identity_from_secret_bytes(const fips_secp *secp, fips_entropy entropy, const uint8_t secret[32], fips_identity **out)
{
    if (!secp256k1_ec_seckey_verify(secp->ctx, secret)) return FIPS_IDENTITY_InvalidSecretKey;
    fips_identity *id = malloc(sizeof(*id));
    if (!id) return FIPS_IDENTITY_AllocationFailed;
    id->secp = secp; id->entropy = entropy;
    secp256k1_xonly_pubkey xonly;
    secp256k1_pubkey full;
    size_t len = 33;
    if (!secp256k1_keypair_create(secp->ctx, &id->keypair, secret) ||
        !secp256k1_keypair_xonly_pub(secp->ctx, &xonly, NULL, &id->keypair) ||
        !secp256k1_xonly_pubkey_serialize(secp->ctx, id->pubkey.bytes, &xonly) ||
        !secp256k1_keypair_pub(secp->ctx, &full, &id->keypair) ||
        !secp256k1_ec_pubkey_serialize(secp->ctx, id->full, &len, &full, SECP256K1_EC_COMPRESSED)) {
        fips_identity_destroy(id); return FIPS_IDENTITY_InvalidSecretKey;
    }
    id->node_addr = fips_node_addr_from_pubkey(&id->pubkey);
    id->address = fips_ipv6_address_from_node_addr(&id->node_addr);
    *out = id;
    return FIPS_IDENTITY_OK;
}
fips_identity_status fips_identity_from_secret_str(const fips_secp *secp, fips_entropy entropy, const char *text, size_t len, fips_identity **out)
{
    uint8_t secret[32];
    fips_identity_status status = fips_decode_secret(secp, text, len, secret);
    if (status == FIPS_IDENTITY_OK) status = fips_identity_from_secret_bytes(secp, entropy, secret, out);
    sodium_memzero(secret, sizeof(secret));
    return status;
}
fips_identity_status fips_identity_generate(const fips_secp *secp, fips_entropy entropy, fips_identity **out)
{
    uint8_t secret[32];
    entropy.fill(entropy.ctx, secret, 32);
    fips_identity_status status = fips_identity_from_secret_bytes(secp, entropy, secret, out);
    sodium_memzero(secret, sizeof(secret));
    return status;
}
void fips_identity_destroy(fips_identity *id)
{
    if (!id) return;
    sodium_memzero(id, sizeof(*id)); free(id);
}
fips_xonly_pubkey fips_identity_pubkey(const fips_identity *id) { return id->pubkey; }
void fips_identity_pubkey_full(const fips_identity *id, uint8_t out[33]) { memcpy(out, id->full, 33); }
fips_node_addr fips_identity_node_addr(const fips_identity *id) { return id->node_addr; }
fips_ipv6_address fips_identity_address(const fips_identity *id) { return id->address; }
void fips_identity_export_secret(const fips_identity *id, uint8_t out[32])
{
    int ok = secp256k1_keypair_sec(id->secp->ctx, out, &id->keypair);
    (void)ok; /* Infallible for the constructor-validated keypair. */
}
fips_identity_status fips_identity_npub(const fips_identity *id, char *out, size_t capacity)
{
    return fips_encode_npub(&id->pubkey, out, capacity);
}
fips_identity_status identity_sign_digest(const fips_identity *id, const uint8_t digest[32], uint8_t out[64])
{
    uint8_t aux[32], signature[64];
    id->entropy.fill(id->entropy.ctx, aux, 32);
    int ok = secp256k1_schnorrsig_sign32(id->secp->ctx, signature, digest, &id->keypair, aux);
    sodium_memzero(aux, sizeof(aux));
    if (!ok) return FIPS_IDENTITY_InvalidSecretKey;
    memcpy(out, signature, 64);
    return FIPS_IDENTITY_OK;
}
fips_identity_status fips_identity_sign(const fips_identity *id, const uint8_t *data, size_t len, uint8_t signature[64])
{
    uint8_t digest[32];
    identity_sha256(data, len, digest);
    return identity_sign_digest(id, digest, signature);
}
