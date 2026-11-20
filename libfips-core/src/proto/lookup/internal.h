#ifndef FIPS_LOOKUP_INTERNAL_H
#define FIPS_LOOKUP_INTERNAL_H
#include <fips/proto/lookup/mod.h>
#include <string.h>
static inline int lookup_addr_cmp(fips_node_addr a, fips_node_addr b) { return memcmp(a.bytes, b.bytes, 16); }
static inline uint64_t lookup_elapsed(uint64_t now, uint64_t then) { return now > then ? now - then : 0; }
static inline uint64_t lookup_add(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
static inline uint64_t lookup_millis(uint64_t secs) { return secs > UINT64_MAX / 1000 ? UINT64_MAX : secs * 1000; }
static inline fips_status lookup_error(fips_proto_error *e, fips_proto_error_variant v) {
    if (e) *e = (fips_proto_error){.variant = v};
    return FIPS_ERR_RANGE;
}
static inline fips_status lookup_writer_require(const fips_writer *w, size_t n, fips_proto_error *e) {
    if (n <= w->capacity - w->pos) return FIPS_OK;
    if (e) *e = (fips_proto_error){.variant = FIPS_PROTO_MessageTooLong,
        .as.message_too_long = {w->capacity, n > SIZE_MAX - w->pos ? SIZE_MAX : w->pos + n}};
    return FIPS_ERR_RANGE;
}
#endif
