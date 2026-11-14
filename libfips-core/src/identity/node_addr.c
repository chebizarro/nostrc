#include <fips/identity/node_addr.h>
#include <fips/identity/key.h>
#include <fips/identity/prefix.h>
#include <string.h>

fips_identity_status fips_node_addr_from_slice(const uint8_t *bytes, size_t len, fips_node_addr *out)
{
    if (len != 16) return FIPS_IDENTITY_InvalidNodeAddrLength;
    memcpy(out->bytes, bytes, 16);
    return FIPS_IDENTITY_OK;
}
fips_node_addr fips_node_addr_from_pubkey(const fips_xonly_pubkey *pubkey)
{
    fips_peer_key key = fips_peer_key_from_xonly(pubkey);
    return fips_peer_key_addr(&key);
}
fips_node_addr fips_node_addr_from_prefix(fips_node_prefix prefix)
{
    fips_node_addr addr = {{0}};
    memcpy(addr.bytes, prefix.bytes, 15);
    return addr;
}
fips_node_prefix fips_node_addr_prefix(const fips_node_addr *addr)
{
    return fips_node_prefix_from_bytes(addr->bytes);
}
static void hex(const uint8_t *bytes, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[2*i] = digits[bytes[i] >> 4]; out[2*i+1] = digits[bytes[i] & 15];
    }
}
fips_identity_status fips_node_addr_format(const fips_node_addr *addr, char *out, size_t capacity)
{
    if (capacity < 33) return FIPS_IDENTITY_BufferTooSmall;
    hex(addr->bytes, 16, out); out[32] = '\0';
    return FIPS_IDENTITY_OK;
}
fips_identity_status fips_node_addr_short_hex(const fips_node_addr *addr, char *out, size_t capacity)
{
    if (capacity < 12) return FIPS_IDENTITY_BufferTooSmall;
    hex(addr->bytes, 4, out); memcpy(out+8, "...", 4);
    return FIPS_IDENTITY_OK;
}
