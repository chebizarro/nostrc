#ifndef FIPS_PROTO_FMP_LIMITS_H
#define FIPS_PROTO_FMP_LIMITS_H
#include <fips/host.h>
#ifdef __cplusplus
extern "C" {
#endif
/* limits.rs: base * 2^count, saturating u64 arithmetic then capped. */
FIPS_CORE_API uint64_t fips_fmp_backoff_ms(uint32_t count, uint64_t base_ms, uint64_t cap_ms);
#ifdef __cplusplus
}
#endif
#endif
