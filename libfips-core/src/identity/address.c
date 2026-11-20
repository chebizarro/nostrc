#include <fips/identity/address.h>
#include <fips/identity/prefix.h>
#include <string.h>

fips_identity_status fips_ipv6_address_from_slice(const uint8_t *bytes, size_t len, fips_ipv6_address *out)
{
    if (len != 16) return FIPS_IDENTITY_InvalidAddressLength;
    if (bytes[0] != FIPS_IPV6_ADDRESS_PREFIX) return FIPS_IDENTITY_InvalidAddressPrefix;
    memcpy(out->bytes, bytes, 16);
    return FIPS_IDENTITY_OK;
}
fips_ipv6_address fips_ipv6_address_from_node_addr(const fips_node_addr *addr)
{
    fips_ipv6_address address;
    address.bytes[0] = FIPS_IPV6_ADDRESS_PREFIX;
    memcpy(address.bytes+1, addr->bytes, 15);
    return address;
}
fips_node_prefix fips_ipv6_address_prefix(const fips_ipv6_address *addr)
{
    return fips_node_prefix_from_octets(addr->bytes);
}
fips_identity_status fips_ipv6_address_format(const fips_ipv6_address *addr, char *out, size_t capacity)
{
    uint16_t words[8];
    size_t best = 8, longest = 1;
    for (size_t i = 0; i < 8; ++i)
        words[i] = (uint16_t)((uint16_t)addr->bytes[2*i] << 8 | addr->bytes[2*i+1]);
    for (size_t i = 0; i < 8;) {
        if (words[i]) { ++i; continue; }
        size_t start = i;
        while (i < 8 && !words[i]) ++i;
        if (i-start > longest) { best = start; longest = i-start; }
    }
    char text[40]; size_t n = 0;
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 8;) {
        if (i == best) { text[n++] = ':'; text[n++] = ':'; i += longest; continue; }
        if (n && text[n-1] != ':') text[n++] = ':';
        uint16_t word = words[i++];
        int shift = 12;
        while (shift && !(word >> shift)) shift -= 4;
        for (; shift >= 0; shift -= 4) text[n++] = digits[(word >> shift) & 15];
    }
    text[n] = '\0';
    if (capacity <= n) return FIPS_IDENTITY_BufferTooSmall;
    memcpy(out, text, n+1);
    return FIPS_IDENTITY_OK;
}
