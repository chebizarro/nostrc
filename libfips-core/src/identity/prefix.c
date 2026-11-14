#include <fips/identity/prefix.h>
#include <string.h>
fips_node_prefix fips_node_prefix_from_bytes(const uint8_t bytes[15])
{
    fips_node_prefix prefix;
    memcpy(prefix.bytes, bytes, 15);
    return prefix;
}
fips_node_prefix fips_node_prefix_from_octets(const uint8_t octets[16])
{
    return fips_node_prefix_from_bytes(octets+1);
}
