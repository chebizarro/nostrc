#ifndef FIPS_UTILS_ONLINK_H
#define FIPS_UTILS_ONLINK_H
#include <fips/host.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Network-order octets; width must be 4 or 16. No OS address types or parsing.
 * IPv4 uses the first four bytes; the remaining bytes are ignored. */
typedef struct fips_ip_addr { uint8_t bytes[16]; uint8_t width; } fips_ip_addr;
typedef struct fips_onlink_prefix {
    fips_ip_addr net;
    uint8_t len;
    bool has_index;
    uint32_t index;
} fips_onlink_prefix;
typedef struct fips_onlink_prefixes {
    fips_onlink_prefix *entries;
    size_t count, discarded;
    bool failed;
} fips_onlink_prefixes;
/* Build into caller-owned storage (disjoint from inputs/out). Zero lengths are
 * counted/discarded; others clamp to canonical family width. IPv4-mapped IPv6
 * becomes IPv4 BEFORE clamping, matching Rust (no subtraction of 96).
 * Invalid address width/capacity returns RANGE, leaving out/storage untouched.
 * NULL input/storage permitted for zero length/capacity. Objects/storage must
 * outlive queries; fields private-by-contract. No allocation; read_system is host.
 */
FIPS_CORE_API fips_status fips_onlink_from_indexed(fips_onlink_prefixes *out,
    fips_onlink_prefix *storage, size_t capacity,
    const fips_onlink_prefix *entries, size_t count);
/* Same inputs, but all interface indexes are treated as unknown. */
FIPS_CORE_API fips_status fips_onlink_from_pairs(fips_onlink_prefixes *out,
    fips_onlink_prefix *storage, size_t capacity,
    const fips_onlink_prefix *entries, size_t count);
FIPS_CORE_API fips_onlink_prefixes fips_onlink_failed(void);
FIPS_CORE_API bool fips_onlink_read_failed(const fips_onlink_prefixes *set);
FIPS_CORE_API size_t fips_onlink_discarded(const fips_onlink_prefixes *set);
FIPS_CORE_API bool fips_onlink_is_empty(const fips_onlink_prefixes *set);
/* Query addresses must have valid widths. NULL interfaces allowed when n == 0. */
FIPS_CORE_API bool fips_onlink_contains(const fips_onlink_prefixes *set, fips_ip_addr ip);
FIPS_CORE_API bool fips_onlink_contains_on(const fips_onlink_prefixes *set,
    fips_ip_addr ip, const uint32_t *interfaces, size_t n);
FIPS_CORE_API bool fips_onlink_share_link(const fips_onlink_prefixes *set,
    fips_ip_addr a, fips_ip_addr b);
FIPS_CORE_API bool fips_onlink_read_suspect(size_t considered, const fips_onlink_prefixes *set);
#ifdef __cplusplus
}
#endif
#endif
