#ifndef FIPS_ROUTING_LIMITS_H
#define FIPS_ROUTING_LIMITS_H
#include <fips/proto/rate_limit.h>
#include <fips/identity/node_addr.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_ROUTING_DEFAULT_MIN_INTERVAL_MS UINT64_C(100)
#define FIPS_ROUTING_MAX_AGE_MS UINT64_C(10000)
typedef enum fips_limit_verdict { FIPS_LIMIT_Admit, FIPS_LIMIT_AdmitAtCapacity, FIPS_LIMIT_Suppress } fips_limit_verdict;
typedef struct fips_routing_error_rate_limiter { fips_per_addr_rate_limiter inner; } fips_routing_error_rate_limiter;
FIPS_CORE_API void fips_routing_error_rate_limiter_init(fips_routing_error_rate_limiter *l, fips_instant origin);
FIPS_CORE_API void fips_routing_error_rate_limiter_with_interval_ms(fips_routing_error_rate_limiter *l, uint64_t interval, fips_instant origin);
FIPS_CORE_API fips_limit_verdict fips_routing_error_rate_limiter_check(fips_routing_error_rate_limiter *l, const fips_node_addr *dest, fips_instant now);
FIPS_CORE_API bool fips_routing_error_rate_limiter_should_send(fips_routing_error_rate_limiter *l, const fips_node_addr *dest, fips_instant now);
FIPS_CORE_API void fips_routing_error_rate_limiter_cleanup(fips_routing_error_rate_limiter *l, fips_instant now);
#ifdef __cplusplus
}
#endif
#endif
