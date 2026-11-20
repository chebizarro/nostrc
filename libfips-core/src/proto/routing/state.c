#include <fips/proto/routing/state.h>
void fips_router_init(fips_router *r, fips_instant origin)
{ fips_routing_error_rate_limiter_init(&r->error_limiter, origin); }
