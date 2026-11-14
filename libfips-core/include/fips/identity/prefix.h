#ifndef FIPS_IDENTITY_PREFIX_H
#define FIPS_IDENTITY_PREFIX_H
#include <fips/identity/mod.h>
#ifdef __cplusplus
extern "C" {
#endif
FIPS_CORE_API fips_node_prefix fips_node_prefix_from_bytes(const uint8_t bytes[15]);
/* Deliberately ignores octets[0], including outside fd00::/8. */
FIPS_CORE_API fips_node_prefix fips_node_prefix_from_octets(const uint8_t octets[16]);
#ifdef __cplusplus
}
#endif
#endif
