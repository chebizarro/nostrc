#include <fips/proto/routing/limits.h>
void fips_routing_error_rate_limiter_init(fips_routing_error_rate_limiter *l, fips_instant origin)
{ fips_routing_error_rate_limiter_with_interval_ms(l, FIPS_ROUTING_DEFAULT_MIN_INTERVAL_MS, origin); }
void fips_routing_error_rate_limiter_with_interval_ms(fips_routing_error_rate_limiter *l, uint64_t interval, fips_instant origin)
{ fips_per_addr_init(&l->inner, interval, FIPS_ROUTING_MAX_AGE_MS, origin); }
fips_limit_verdict fips_routing_error_rate_limiter_check(fips_routing_error_rate_limiter *l, const fips_node_addr *dest, fips_instant now)
{
    switch (fips_per_addr_check_and_record(&l->inner, dest->bytes, now)) {
    case FIPS_RECORD_ADMIT: return FIPS_LIMIT_Admit;
    case FIPS_RECORD_ADMIT_AT_CAPACITY: return FIPS_LIMIT_AdmitAtCapacity;
    case FIPS_RECORD_SUPPRESS: return FIPS_LIMIT_Suppress;
    }
    return FIPS_LIMIT_Suppress;
}
bool fips_routing_error_rate_limiter_should_send(fips_routing_error_rate_limiter *l, const fips_node_addr *dest, fips_instant now)
{ return fips_routing_error_rate_limiter_check(l, dest, now) != FIPS_LIMIT_Suppress; }
void fips_routing_error_rate_limiter_cleanup(fips_routing_error_rate_limiter *l, fips_instant now)
{ fips_per_addr_cleanup(&l->inner, now); }
