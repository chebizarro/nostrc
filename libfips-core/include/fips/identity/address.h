#ifndef FIPS_IDENTITY_ADDRESS_H
#define FIPS_IDENTITY_ADDRESS_H
#include <fips/identity/mod.h>
#ifdef __cplusplus
extern "C" {
#endif
FIPS_CORE_API fips_identity_status fips_ipv6_address_from_slice(const uint8_t *bytes, size_t len, fips_ipv6_address *out);
FIPS_CORE_API fips_ipv6_address fips_ipv6_address_from_node_addr(const fips_node_addr *addr);
FIPS_CORE_API fips_node_prefix fips_ipv6_address_prefix(const fips_ipv6_address *addr);
/* RFC 5952 lowercase, longest zero run (first on ties), NUL terminated. */
FIPS_CORE_API fips_identity_status fips_ipv6_address_format(const fips_ipv6_address *addr, char *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
