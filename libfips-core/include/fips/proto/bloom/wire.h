#ifndef FIPS_PROTO_BLOOM_WIRE_H
#define FIPS_PROTO_BLOOM_WIRE_H
#include <fips/proto/bloom/core.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_FILTER_ANNOUNCE_HEADER_SIZE 11u
#define FIPS_FILTER_ANNOUNCE_V1_WIRE_SIZE 1035u
typedef struct fips_filter_announce {
    fips_bloom_filter filter;
    uint64_t sequence;
    uint8_t hash_count, size_class;
} fips_filter_announce;
FIPS_CORE_API void fips_filter_announce_init(fips_filter_announce *a, const fips_bloom_filter *filter, uint64_t sequence);
FIPS_CORE_API bool fips_filter_announce_is_valid(const fips_filter_announce *a);
FIPS_CORE_API bool fips_filter_announce_is_v1_compliant(const fips_filter_announce *a);
/* Encode includes 0x20. Decode consumes the body AFTER the dispatcher strips
 * 0x20, and requires exact length (including rejecting trailing bytes).
 * Decode permits nonzero k != 5, as Rust does; receive policy refuses it.
 * Encode allows classes 0..3 for testing/future wire forms; decode only v1.
 * Failures preserve cursor/output. Capacity/offset errors are cumulative. */
FIPS_CORE_API fips_status fips_filter_announce_encode(const fips_filter_announce *a, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_filter_announce_decode(fips_reader *r, fips_filter_announce *out, fips_proto_error *error);
#ifdef __cplusplus
}
#endif
#endif
