#include "internal.h"
#include <fips/identity/key.h>
#include <fips/identity/address.h>

fips_peer_key fips_peer_key_from_bytes(const uint8_t bytes[32])
{
    fips_peer_key key;
    memcpy(key.bytes, bytes, 32);
    return key;
}
fips_peer_key fips_peer_key_from_xonly(const fips_xonly_pubkey *pubkey)
{
    return fips_peer_key_from_bytes(pubkey->bytes);
}
fips_identity_status fips_peer_key_to_xonly(const fips_secp *secp, const fips_peer_key *key, fips_xonly_pubkey *out)
{
    secp256k1_xonly_pubkey parsed;
    if (!secp256k1_xonly_pubkey_parse(secp->ctx, &parsed, key->bytes))
        return FIPS_IDENTITY_InvalidSecretKey;
    memcpy(out->bytes, key->bytes, 32);
    return FIPS_IDENTITY_OK;
}
fips_node_addr fips_peer_key_addr(const fips_peer_key *key)
{
    uint8_t hash[32];
    identity_sha256(key->bytes, 32, hash);
    fips_node_addr addr;
    memcpy(addr.bytes, hash, 16);
    return addr;
}
fips_ipv6_address fips_peer_key_address(const fips_peer_key *key)
{
    fips_node_addr addr = fips_peer_key_addr(key);
    return fips_ipv6_address_from_node_addr(&addr);
}
