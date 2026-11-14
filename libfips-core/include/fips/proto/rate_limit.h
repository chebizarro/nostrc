#ifndef FIPS_PROTO_RATE_LIMIT_H
#define FIPS_PROTO_RATE_LIMIT_H
#include <fips/host.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_PER_ADDR_MAX_ENTRIES 4096

typedef enum fips_record_outcome {
    FIPS_RECORD_ADMIT, FIPS_RECORD_ADMIT_AT_CAPACITY, FIPS_RECORD_SUPPRESS
} fips_record_outcome;
typedef struct fips_per_addr_entry {
    uint8_t addr[16];
    uint64_t last_ms;
} fips_per_addr_entry;
/** Caller-owned, non-copyable while in use. Sorted storage avoids hashing
 * peer-controlled addresses. Exactly 4096 entries; no allocation. Time is
 * quantized to whole milliseconds relative to origin (the Rust clock's zero).
 * Elapsed milliseconds saturate at UINT64_MAX. All instants must
 * share its domain. Suppression does not refresh an address or run a sweep.
 * A full map deliberately admits unknown addresses without recording them. */
typedef struct fips_per_addr_rate_limiter {
    fips_per_addr_entry entries[FIPS_PER_ADDR_MAX_ENTRIES];
    size_t len;
    uint64_t min_interval_ms, max_age_ms, sweeps;
    fips_instant origin;
    uint64_t last_sweep_ms;
} fips_per_addr_rate_limiter;
FIPS_CORE_API void fips_per_addr_init(fips_per_addr_rate_limiter *limiter,
    uint64_t min_interval_ms, uint64_t max_age_ms, fips_instant origin);
FIPS_CORE_API fips_record_outcome fips_per_addr_check_and_record(
    fips_per_addr_rate_limiter *limiter, const uint8_t addr[16], fips_instant now);
FIPS_CORE_API void fips_per_addr_cleanup(fips_per_addr_rate_limiter *limiter,
                                       fips_instant now);
/** base_ms * 2^exponent; u64 shift/product saturation, then cap (NOT the
 * u32 multiplier used by the duration-valued host fips_backoff helper). */
FIPS_CORE_API uint64_t fips_backoff_ms(uint32_t exponent, uint64_t base_ms,
                                      uint64_t cap_ms);
#ifdef __cplusplus
}
#endif
#endif
