#ifndef FIPS_STP_INTERNAL_H
#define FIPS_STP_INTERNAL_H
#include <fips/proto/stp/declaration.h>
#include <string.h>
#include <limits.h>
static inline int addr_cmp(fips_node_addr a, fips_node_addr b) {
    return memcmp(a.bytes, b.bytes, 16);
}
static inline uint64_t sat_add(uint64_t a, uint64_t b) {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}
static inline uint64_t sat_sub(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }
static inline uint64_t secs_ms(uint64_t s) { return s > UINT64_MAX / 1000 ? UINT64_MAX : s * 1000; }
static inline uint64_t stp_ms(fips_instant origin, fips_instant now) {
    fips_duration d = now.ops->saturating_duration_since(now, origin);
    return sat_add(secs_ms(d.secs), d.nanos / 1000000);
}
static inline fips_status wire_error(fips_proto_error *e, fips_proto_error_variant v) {
    if (e) {
        *e = (fips_proto_error){.variant = v};
    }
    return FIPS_ERR_RANGE;
}
static inline fips_status writer_require(fips_writer *w, size_t n, fips_proto_error *e) {
    if (n > fips_writer_remaining(w)) {
        if (e) {
            *e =
                (fips_proto_error){.variant = FIPS_PROTO_MessageTooLong,
                                   .as.message_too_long = {
                                       w->capacity, n > SIZE_MAX - w->pos ? SIZE_MAX : w->pos + n}};
        }
        return FIPS_ERR_RANGE;
    }
    return FIPS_OK;
}
#endif
