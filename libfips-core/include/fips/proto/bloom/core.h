#ifndef FIPS_PROTO_BLOOM_CORE_H
#define FIPS_PROTO_BLOOM_CORE_H
#include <fips/proto/codec.h>
#include <fips/identity/node_addr.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_BLOOM_DEFAULT_BITS 8192u
#define FIPS_BLOOM_DEFAULT_HASH_COUNT 5u
#define FIPS_BLOOM_V1_SIZE_CLASS 1u
#define FIPS_BLOOM_ACCEPT_FPR_CAP 0.20
#define FIPS_BLOOM_ECHO_MIN_BITS 128u
#define FIPS_BLOOM_SENT_HISTORY 4u
#define FIPS_BLOOM_ECHO_WARN_MS UINT64_C(300000)
/* Bounded replacement for Vec: all four wire size classes, plus smaller
 * byte-aligned custom filters. Public fields are read-only after construction.
 * Required pointers must be valid; buffers must not overlap the object.
 * Fallible calls preserve outputs and cursors on failure; error may be NULL
 * and is unchanged on success. No allocation, clocks or IO. */
#define FIPS_BLOOM_MAX_BYTES 4096u
typedef struct fips_bloom_filter {
    uint8_t bits[FIPS_BLOOM_MAX_BYTES];
    size_t num_bits;
    uint8_t hash_count;
} fips_bloom_filter;
FIPS_CORE_API void fips_bloom_init(fips_bloom_filter *f);
/* BadBloom for upstream parameter errors, MessageTooLong for the C bound. */
FIPS_CORE_API fips_status fips_bloom_with_params(fips_bloom_filter *out, size_t num_bits, uint8_t hash_count, fips_proto_error *error);
FIPS_CORE_API fips_status fips_bloom_from_bytes(fips_bloom_filter *out, const uint8_t *bytes, size_t len, uint8_t hash_count, fips_proto_error *error);
/* Raw filter wire form is exactly num_bits/8 bytes, no header. Decode uses
 * the entire remaining reader, with the hash count supplied out of band. */
FIPS_CORE_API fips_status fips_bloom_encode(const fips_bloom_filter *f, fips_writer *w, fips_proto_error *error);
FIPS_CORE_API fips_status fips_bloom_decode(fips_reader *r, uint8_t hash_count, fips_bloom_filter *out, fips_proto_error *error);
FIPS_CORE_API void fips_bloom_insert_bytes(fips_bloom_filter *f, const uint8_t *bytes, size_t len);
FIPS_CORE_API bool fips_bloom_contains_bytes(const fips_bloom_filter *f, const uint8_t *bytes, size_t len);
FIPS_CORE_API void fips_bloom_insert(fips_bloom_filter *f, const fips_node_addr *addr);
FIPS_CORE_API bool fips_bloom_contains(const fips_bloom_filter *f, const fips_node_addr *addr);
/* As upstream, merge checks size, NOT hash count. It is an OR of bits. */
FIPS_CORE_API fips_status fips_bloom_merge(fips_bloom_filter *f, const fips_bloom_filter *other, fips_proto_error *error);
FIPS_CORE_API fips_status fips_bloom_union(const fips_bloom_filter *a, const fips_bloom_filter *b, fips_bloom_filter *out, fips_proto_error *error);
FIPS_CORE_API void fips_bloom_clear(fips_bloom_filter *f);
FIPS_CORE_API bool fips_bloom_equal(const fips_bloom_filter *a, const fips_bloom_filter *b);
FIPS_CORE_API size_t fips_bloom_count_ones(const fips_bloom_filter *f);
FIPS_CORE_API bool fips_bloom_is_empty(const fips_bloom_filter *f);
FIPS_CORE_API double fips_bloom_fill_ratio(const fips_bloom_filter *f);
FIPS_CORE_API double fips_bloom_fpr(const fips_bloom_filter *f);
/* false is Rust None (saturated or above cap); preserves out. Formula uses
 * fips_powi, not pow, and the binary64 C log documented in README.md. */
FIPS_CORE_API bool fips_bloom_estimated_count(const fips_bloom_filter *f, double max_fpr, double *out);
FIPS_CORE_API bool fips_bloom_overlap(const fips_bloom_filter *f, const fips_bloom_filter *sent, double *out);
FIPS_CORE_API double fips_bloom_echo_bound(const fips_bloom_filter *f, double threshold);
FIPS_CORE_API bool fips_bloom_echoes(const fips_bloom_filter *f, const fips_bloom_filter *sent, double threshold);
#ifdef __cplusplus
}
#endif
#endif
