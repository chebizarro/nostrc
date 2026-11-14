#ifndef FIPS_IDENTITY_KEY_H
#define FIPS_IDENTITY_KEY_H
#include <fips/identity/mod.h>
#ifdef __cplusplus
extern "C" {
#endif
/* PeerKey accepts all 32-byte values, even off-curve coordinates. */
FIPS_CORE_API fips_peer_key fips_peer_key_from_bytes(const uint8_t bytes[32]);
FIPS_CORE_API fips_peer_key fips_peer_key_from_xonly(const fips_xonly_pubkey *pubkey);
FIPS_CORE_API fips_identity_status fips_peer_key_to_xonly(const fips_secp *secp, const fips_peer_key *key, fips_xonly_pubkey *out);
FIPS_CORE_API fips_node_addr fips_peer_key_addr(const fips_peer_key *key);
FIPS_CORE_API fips_ipv6_address fips_peer_key_address(const fips_peer_key *key);
#ifdef __cplusplus
}
#endif
#endif
