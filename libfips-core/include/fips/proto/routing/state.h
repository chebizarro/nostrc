#ifndef FIPS_ROUTING_STATE_H
#define FIPS_ROUTING_STATE_H
#include <fips/proto/routing/limits.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Caller-owned, bounded, no allocation; same clock-domain rules as rate_limit. */
typedef struct fips_router { fips_routing_error_rate_limiter error_limiter; } fips_router;
FIPS_CORE_API void fips_router_init(fips_router *r, fips_instant origin);
#ifdef __cplusplus
}
#endif
#endif
