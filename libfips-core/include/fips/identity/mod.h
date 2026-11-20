#ifndef FIPS_IDENTITY_MOD_H
#define FIPS_IDENTITY_MOD_H
#include <fips/host.h>
#ifdef __cplusplus
extern "C" {
#endif
/* All fallible calls preserve outputs on failure. Required pointers are caller
 * preconditions; byte/string inputs have explicit lengths and may be untrusted.
 * Context and identity constructors allocate; all other calls allocate nothing.
 * Objects are not copyable. Destroy them with their matching core destructor.
 * Caller supplies synchronization and host entropy; no implicit RNG or globals. */
typedef enum fips_identity_status {
    FIPS_IDENTITY_OK = 0,
    FIPS_IDENTITY_InvalidSecretKey,
    FIPS_IDENTITY_SignatureVerificationFailed,
    FIPS_IDENTITY_InvalidNodeAddrLength,
    FIPS_IDENTITY_InvalidAddressLength,
    FIPS_IDENTITY_InvalidAddressPrefix,
    FIPS_IDENTITY_Bech32Encode,
    FIPS_IDENTITY_Bech32Decode,
    FIPS_IDENTITY_InvalidNpubPrefix,
    FIPS_IDENTITY_InvalidNpubLength,
    FIPS_IDENTITY_NonCanonicalNpub,
    FIPS_IDENTITY_InvalidNsecPrefix,
    FIPS_IDENTITY_InvalidNsecLength,
    FIPS_IDENTITY_InvalidHex,
    FIPS_IDENTITY_NonCanonicalNsec,
    FIPS_IDENTITY_BufferTooSmall,
    FIPS_IDENTITY_AllocationFailed
} fips_identity_status;
typedef struct fips_secp fips_secp;
typedef struct fips_identity fips_identity;
typedef struct fips_peer_key { uint8_t bytes[32]; } fips_peer_key;
/* Validated by constructors/decoders. Do not modify bytes after validation. */
typedef struct fips_xonly_pubkey { uint8_t bytes[32]; } fips_xonly_pubkey;
typedef struct fips_node_addr { uint8_t bytes[16]; } fips_node_addr;
typedef struct fips_ipv6_address { uint8_t bytes[16]; } fips_ipv6_address;
typedef struct fips_node_prefix { uint8_t bytes[15]; } fips_node_prefix;
#define FIPS_IPV6_ADDRESS_PREFIX 0xfd
FIPS_CORE_API const char *fips_identity_error_name(fips_identity_status status);
/* Explicit context, unlike Rust's global OnceBox. Always blinded with exactly
 * 32 host bytes, with no unblinded fallback. Must outlive its identities. */
FIPS_CORE_API fips_identity_status fips_identity_init_secp(fips_entropy entropy, fips_secp **out);
FIPS_CORE_API void fips_identity_destroy_secp(fips_secp *secp);
#ifdef __cplusplus
}
#endif
#endif
