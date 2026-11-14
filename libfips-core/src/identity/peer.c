#include "internal.h"
#include <fips/identity/peer.h>
#include <fips/identity/key.h>
#include <fips/identity/node_addr.h>
#include <fips/identity/address.h>
#include <fips/identity/encoding.h>

fips_identity_status fips_peer_identity_from_pubkey_full(const fips_secp *secp, const uint8_t compressed[33], fips_peer_identity *out)
{
    secp256k1_pubkey full;
    secp256k1_xonly_pubkey xonly;
    fips_peer_identity peer;
    if (!secp256k1_ec_pubkey_parse(secp->ctx, &full, compressed, 33) ||
        !secp256k1_xonly_pubkey_from_pubkey(secp->ctx, &xonly, NULL, &full) ||
        !secp256k1_xonly_pubkey_serialize(secp->ctx, peer.pubkey.bytes, &xonly))
        return FIPS_IDENTITY_InvalidSecretKey;
    memcpy(peer.pubkey_full, compressed, 33);
    peer.node_addr = fips_node_addr_from_pubkey(&peer.pubkey);
    peer.address = fips_ipv6_address_from_node_addr(&peer.node_addr);
    *out = peer;
    return FIPS_IDENTITY_OK;
}
fips_identity_status fips_peer_identity_from_pubkey(const fips_secp *secp, const fips_xonly_pubkey *pubkey, fips_peer_identity *out)
{
    uint8_t full[33]; full[0] = 2; memcpy(full+1, pubkey->bytes, 32);
    return fips_peer_identity_from_pubkey_full(secp, full, out);
}
fips_identity_status fips_peer_identity_from_npub(const fips_secp *secp, const char *text, size_t len, fips_peer_identity *out)
{
    fips_xonly_pubkey pubkey;
    fips_identity_status status = fips_decode_npub(secp, text, len, &pubkey);
    if (status != FIPS_IDENTITY_OK) return status;
    return fips_peer_identity_from_pubkey(secp, &pubkey, out);
}
fips_identity_status fips_peer_identity_npub(const fips_peer_identity *peer, char *out, size_t capacity)
{
    return fips_encode_npub(&peer->pubkey, out, capacity);
}
fips_identity_status fips_peer_identity_short_npub(const fips_peer_identity *peer, char *out, size_t capacity)
{
    if (capacity < 17) return FIPS_IDENTITY_BufferTooSmall;
    char full[64];
    fips_identity_status status = fips_peer_identity_npub(peer, full, sizeof(full));
    if (status != FIPS_IDENTITY_OK) return status;
    memcpy(out, full, 9); memcpy(out+9, "...", 3); memcpy(out+12, full+59, 4); out[16] = '\0';
    return FIPS_IDENTITY_OK;
}
fips_identity_status fips_peer_identity_verify(const fips_secp *secp, const fips_peer_identity *peer, const uint8_t *data, size_t len, const uint8_t signature[64])
{
    secp256k1_xonly_pubkey pubkey;
    uint8_t digest[32]; identity_sha256(data, len, digest);
    if (!secp256k1_xonly_pubkey_parse(secp->ctx, &pubkey, peer->pubkey.bytes) ||
        !secp256k1_schnorrsig_verify(secp->ctx, signature, digest, 32, &pubkey))
        return FIPS_IDENTITY_SignatureVerificationFailed;
    return FIPS_IDENTITY_OK;
}
