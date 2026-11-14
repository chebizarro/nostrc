#include <fips/proto/fmp/limits.h>
#include <fips/proto/rate_limit.h>
uint64_t fips_fmp_backoff_ms(uint32_t count, uint64_t base, uint64_t cap)
{ return fips_backoff_ms(count, base, cap); }
