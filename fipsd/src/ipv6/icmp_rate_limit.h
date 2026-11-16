#ifndef FIPSD_IPV6_ICMP_RATE_LIMIT_H
#define FIPSD_IPV6_ICMP_RATE_LIMIT_H
/* ICMPv6 Packet Too Big rate limiting (upstream icmp_rate_limit.rs): a token
 * bucket per source address, RFC 4443 Section 2.4 (f). The limiter counts the
 * instants it is given and never reads a clock itself. */
#include <fips/node/rate_limit.h>
#include <stdbool.h>
#include <stddef.h>
/* The burst a source may be sent after a quiet spell, and the time in which
 * its bucket regains one Packet Too Big once spent: ten a second. */
#define FIPSD_IPV6_PTB_BURST 10u
#define FIPSD_IPV6_PTB_INTERVAL_MS 100u
/* How long a source is kept after its last check, at the least. */
#define FIPSD_IPV6_PTB_MAX_AGE_SECS 10u
/* The host's bound on tracked sources (upstream grows without bound and
 * relies on the ten-second cleanup); a full table evicts the source checked
 * longest ago, which hands it a fresh burst early only under that pressure. */
#define FIPSD_IPV6_ICMP_LIMITER_SOURCES 256u
typedef struct fipsd_ipv6_icmp_source {
    bool used;
    uint8_t addr[16];
    fips_token_bucket bucket;
    fips_instant last;
} fipsd_ipv6_icmp_source;
typedef struct fipsd_ipv6_icmp_limiter {
    fipsd_ipv6_icmp_source sources[FIPSD_IPV6_ICMP_LIMITER_SOURCES];
    size_t count;
    double rate;             /* tokens each bucket regains a second */
    fips_duration max_age;   /* max(10 s, interval * burst) */
} fipsd_ipv6_icmp_limiter;
void fipsd_ipv6_icmp_limiter_init(fipsd_ipv6_icmp_limiter *limiter);
/* Buckets regain one token per interval; an interval of zero is one nanosecond. */
void fipsd_ipv6_icmp_limiter_init_interval(fipsd_ipv6_icmp_limiter *limiter, fips_duration interval);
/* True if the source's bucket, refilled up to now, holds a token, and spends
 * it; a source checked for the first time has a full bucket. When true,
 * sources not checked within the maximum age at now are dropped. */
bool fipsd_ipv6_icmp_limiter_check(fipsd_ipv6_icmp_limiter *limiter, const uint8_t src[16], fips_instant now);
void fipsd_ipv6_icmp_limiter_cleanup(fipsd_ipv6_icmp_limiter *limiter, fips_instant now);
size_t fipsd_ipv6_icmp_limiter_len(const fipsd_ipv6_icmp_limiter *limiter);
#endif
