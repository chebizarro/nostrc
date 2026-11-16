#include "ipv6/icmp_rate_limit.h"
#include <string.h>
static uint64_t nanos_of(fips_duration d)
{
    if (d.secs > UINT64_MAX / 1000000000u) return UINT64_MAX;
    uint64_t n = d.secs * 1000000000u;
    return n > UINT64_MAX - d.nanos ? UINT64_MAX : n + d.nanos;
}
static uint64_t nanos_since(fips_instant now, fips_instant earlier)
{
    if (!now.ops || now.ops->compare(now, earlier) <= 0) return 0;
    return nanos_of(now.ops->saturating_duration_since(now, earlier));
}
void fipsd_ipv6_icmp_limiter_init(fipsd_ipv6_icmp_limiter *l)
{
    fipsd_ipv6_icmp_limiter_init_interval(l, fips_duration_from_millis(FIPSD_IPV6_PTB_INTERVAL_MS));
}
void fipsd_ipv6_icmp_limiter_init_interval(fipsd_ipv6_icmp_limiter *l, fips_duration interval)
{
    memset(l, 0, sizeof *l);
    if (!interval.secs && !interval.nanos) interval.nanos = 1;
    l->rate = 1.0 / ((double)interval.secs + (double)interval.nanos / 1e9);
    uint64_t floor_ns = (uint64_t)FIPSD_IPV6_PTB_MAX_AGE_SECS * 1000000000u, ns = nanos_of(interval);
    uint64_t refill = ns > UINT64_MAX / FIPSD_IPV6_PTB_BURST ? UINT64_MAX : ns * FIPSD_IPV6_PTB_BURST;
    uint64_t age = refill > floor_ns ? refill : floor_ns;
    l->max_age = (fips_duration){age / 1000000000u, (uint32_t)(age % 1000000000u)};
}
static fipsd_ipv6_icmp_source *find(fipsd_ipv6_icmp_limiter *l, const uint8_t src[16])
{
    for (size_t i = 0; i < FIPSD_IPV6_ICMP_LIMITER_SOURCES; ++i)
        if (l->sources[i].used && !memcmp(l->sources[i].addr, src, 16)) return &l->sources[i];
    return NULL;
}
bool fipsd_ipv6_icmp_limiter_check(fipsd_ipv6_icmp_limiter *l, const uint8_t src[16], fips_instant now)
{
    fipsd_ipv6_icmp_source *e = find(l, src);
    if (!e) {
        fipsd_ipv6_icmp_source *oldest = NULL;
        for (size_t i = 0; i < FIPSD_IPV6_ICMP_LIMITER_SOURCES && !e; ++i) {
            if (!l->sources[i].used) e = &l->sources[i];
            else if (!oldest || l->sources[i].last.ops->compare(l->sources[i].last, oldest->last) < 0) oldest = &l->sources[i];
        }
        if (!e) e = oldest; else ++l->count;
        e->used = true; memcpy(e->addr, src, 16);
        fips_token_bucket_init(&e->bucket, (fips_bucket_params){FIPSD_IPV6_PTB_BURST, l->rate}, now);
    }
    e->last = now;
    if (!fips_token_bucket_acquire(&e->bucket, 1, now)) return false;
    fipsd_ipv6_icmp_limiter_cleanup(l, now);
    return true;
}
void fipsd_ipv6_icmp_limiter_cleanup(fipsd_ipv6_icmp_limiter *l, fips_instant now)
{
    uint64_t max_age = nanos_of(l->max_age);
    for (size_t i = 0; i < FIPSD_IPV6_ICMP_LIMITER_SOURCES; ++i)
        if (l->sources[i].used && nanos_since(now, l->sources[i].last) >= max_age) { l->sources[i].used = false; --l->count; }
}
size_t fipsd_ipv6_icmp_limiter_len(const fipsd_ipv6_icmp_limiter *l) { return l->count; }
