#ifndef FIPS_IDENTITY_NODE_ADDR_H
#define FIPS_IDENTITY_NODE_ADDR_H
#include <fips/identity/mod.h>
#ifdef __cplusplus
extern "C" {
#endif
FIPS_CORE_API fips_identity_status fips_node_addr_from_slice(const uint8_t *bytes, size_t len, fips_node_addr *out);
FIPS_CORE_API fips_node_addr fips_node_addr_from_pubkey(const fips_xonly_pubkey *pubkey);
FIPS_CORE_API fips_node_addr fips_node_addr_from_prefix(fips_node_prefix prefix);
FIPS_CORE_API fips_node_prefix fips_node_addr_prefix(const fips_node_addr *addr);
/* Lowercase hex, NUL terminated; capacity >= 33 (12 for short_hex). */
FIPS_CORE_API fips_identity_status fips_node_addr_format(const fips_node_addr *addr, char *out, size_t capacity);
FIPS_CORE_API fips_identity_status fips_node_addr_short_hex(const fips_node_addr *addr, char *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
