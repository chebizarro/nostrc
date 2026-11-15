#ifndef FIPS_REKEY_TIME_INTERNAL_H
#define FIPS_REKEY_TIME_INTERNAL_H
#include <fips/host.h>
#include <limits.h>
#include <stdbool.h>
static inline fips_duration rk_secs(uint64_t s) { return (fips_duration){s, 0}; }
static inline int rk_dcmp(fips_duration a, fips_duration b) {
    if (a.secs != b.secs)
        return a.secs < b.secs ? -1 : 1;
    return (a.nanos > b.nanos) - (a.nanos < b.nanos);
}
static inline fips_duration rk_dmin(fips_duration a, fips_duration b) {
    return rk_dcmp(a, b) < 0 ? a : b;
}
static inline fips_duration rk_dmax(fips_duration a, fips_duration b) {
    return rk_dcmp(a, b) > 0 ? a : b;
}
static inline uint64_t rk_uadd(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
static inline uint64_t rk_umul(uint64_t a, uint64_t b) {
    return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}
static inline uint8_t rk_u8add(uint8_t a, uint64_t b) {
    return b > (uint64_t)(UINT8_MAX - a) ? UINT8_MAX : (uint8_t)(a + b);
}
static inline fips_duration rk_add(fips_duration a, fips_duration b) {
    uint64_t carry = (uint64_t)(a.nanos + b.nanos) / 1000000000;
    if (b.secs > UINT64_MAX - a.secs || carry > UINT64_MAX - a.secs - b.secs)
        return (fips_duration){UINT64_MAX, 999999999};
    return (fips_duration){a.secs + b.secs + carry, (a.nanos + b.nanos) % 1000000000};
}
static inline fips_duration rk_sub(fips_duration a, fips_duration b) {
    if (rk_dcmp(a, b) < 0)
        return rk_secs(0);
    bool borrow = a.nanos < b.nanos;
    return (fips_duration){a.secs - b.secs - (uint64_t)borrow,
                           (uint32_t)((uint64_t)a.nanos + (borrow ? 1000000000 : 0) - b.nanos)};
}
static inline fips_duration rk_half(fips_duration d) {
    return (fips_duration){d.secs / 2, (uint32_t)(((d.secs % 2) * 1000000000 + d.nanos) / 2)};
}
static inline uint64_t rk_millis(fips_duration d) {
    return rk_uadd(rk_umul(d.secs, 1000), d.nanos / 1000000);
}
static inline fips_instant rk_later(fips_instant t, fips_duration d) {
    fips_instant out;
    return t.ops->checked_add(t, d, &out) == FIPS_OK ? out : t;
}
static inline fips_duration rk_since(fips_instant now, fips_instant then) {
    return now.ops->saturating_duration_since(now, then);
}
static inline bool rk_before(fips_instant a, fips_instant b) { return a.ops->compare(a, b) < 0; }
static inline bool rk_due(fips_instant t, fips_instant now) { return t.ops && !rk_before(now, t); }
static inline fips_instant rk_min(fips_instant a, fips_instant b) {
    return !a.ops ? b : !b.ops ? a : rk_before(a, b) ? a : b;
}
static inline fips_instant rk_max(fips_instant a, fips_instant b) {
    return !a.ops ? b : !b.ops ? a : rk_before(a, b) ? b : a;
}
#endif
