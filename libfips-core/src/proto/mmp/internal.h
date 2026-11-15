#ifndef FIPS_MMP_INTERNAL_H
#define FIPS_MMP_INTERNAL_H
#include <stdint.h>
#include <limits.h>
static inline uint64_t mmp_sub(uint64_t a, uint64_t b) { return a > b ? a-b : 0; }
static inline uint64_t mmp_add(uint64_t a, uint64_t b) { return b > UINT64_MAX-a ? UINT64_MAX : a+b; }
static inline uint32_t mmp_add32(uint32_t a, uint32_t b) { return b > UINT32_MAX-a ? UINT32_MAX : a+b; }
static inline uint64_t mmp_clamp(uint64_t x, uint64_t lo, uint64_t hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline double mmp_clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
/* Rust float-to-integer casts saturate, and NaN becomes zero. */
static inline uint64_t mmp_u64(double x) { return !(x > 0.0) ? 0 : x >= 0x1p64 ? UINT64_MAX : (uint64_t)x; }
static inline uint16_t mmp_u16(double x) { return !(x > 0.0) ? 0 : x >= 65535.0 ? UINT16_MAX : (uint16_t)x; }
static inline int32_t mmp_i32(uint32_t x) { return x <= INT32_MAX ? (int32_t)x : -1 - (int32_t)(UINT32_MAX-x); }
#endif
