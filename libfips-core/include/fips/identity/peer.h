#ifndef FIPS_IDENTITY_PEER_H
#define FIPS_IDENTITY_PEER_H
#include <fips/identity/mod.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Public-only copyable value; construct with these APIs, then treat immutable.
 * Full compressed key is cached; x-only construction assumes even parity. */
typedef struct fips_peer_identity {
    fips_xonly_pubkey pubkey;
    uint8_t pubkey_full[33];
    fips_node_addr node_addr;
    fips_ipv6_address address;
} fips_peer_identity;
FIPS_CORE_API fips_identity_status fips_peer_identity_from_pubkey(const fips_secp *secp, const fips_xonly_pubkey *pubkey, fips_peer_identity *out);
FIPS_CORE_API fips_identity_status fips_peer_identity_from_pubkey_full(const fips_secp *secp, const uint8_t compressed[33], fips_peer_identity *out);
FIPS_CORE_API fips_identity_status fips_peer_identity_from_npub(const fips_secp *secp, const char *text, size_t len, fips_peer_identity *out);
FIPS_CORE_API fips_identity_status fips_peer_identity_npub(const fips_peer_identity *peer, char *out, size_t capacity);
/* npub1xxxx...xxxx, capacity >= 17. */
FIPS_CORE_API fips_identity_status fips_peer_identity_short_npub(const fips_peer_identity *peer, char *out, size_t capacity);
FIPS_CORE_API fips_identity_status fips_peer_identity_verify(const fips_secp *secp, const fips_peer_identity *peer, const uint8_t *data, size_t len, const uint8_t signature[64]);
#ifdef __cplusplus
}
#endif
#endif
