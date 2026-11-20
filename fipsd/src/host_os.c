#define _POSIX_C_SOURCE 200809L
#include "host_os.h"
#include <sodium.h>
#include <stdlib.h>
#include <time.h>

/* This adapter's opaque words are seconds and normalized nanoseconds. */
static int compare(fips_instant a, fips_instant b)
{
    if (a.opaque[0] != b.opaque[0])
        return a.opaque[0] < b.opaque[0] ? -1 : 1;
    return (a.opaque[1] > b.opaque[1]) - (a.opaque[1] < b.opaque[1]);
}

static fips_duration difference(fips_instant a, fips_instant b)
{
    if (compare(a, b) <= 0)
        return (fips_duration){0, 0};
    uint64_t borrow = a.opaque[1] < b.opaque[1];
    return (fips_duration){a.opaque[0] - b.opaque[0] - borrow,
        (uint32_t)(a.opaque[1] + borrow * UINT64_C(1000000000) - b.opaque[1])};
}

static fips_status checked_add(fips_instant a, fips_duration d, fips_instant *out)
{
    uint64_t nanos = a.opaque[1] + d.nanos;
    uint64_t carry = nanos / UINT64_C(1000000000);
    if (d.secs > UINT64_MAX - a.opaque[0]
        || carry > UINT64_MAX - a.opaque[0] - d.secs)
        return FIPS_ERR_RANGE;
    a.opaque[0] += d.secs + carry;
    a.opaque[1] = nanos % UINT64_C(1000000000);
    *out = a;
    return FIPS_OK;
}

static fips_status checked_sub(fips_instant a, fips_duration d, fips_instant *out)
{
    uint64_t borrow = a.opaque[1] < d.nanos;
    if (d.secs > a.opaque[0] || borrow > a.opaque[0] - d.secs)
        return FIPS_ERR_RANGE;
    a.opaque[0] -= d.secs + borrow;
    a.opaque[1] = a.opaque[1] + borrow * UINT64_C(1000000000) - d.nanos;
    *out = a;
    return FIPS_OK;
}

static const fips_instant_ops instant_ops = {compare, difference, checked_add, checked_sub};

static fips_instant monotonic_now(void *ctx)
{
    struct timespec ts;
    (void)ctx;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0)
        abort(); /* Cannot fulfill the infallible monotonic-clock contract. */
    return (fips_instant){{(uint64_t)ts.tv_sec, (uint64_t)ts.tv_nsec}, &instant_ops};
}

static void secure_fill(void *ctx, uint8_t *buf, size_t n)
{
    (void)ctx;
    if (n == 0)
        return;
    if (sodium_init() < 0)
        abort(); /* Never hand predictable or partial entropy to the core. */
    randombytes_buf(buf, n);
}

fips_clock fipsd_host_os_clock(void)
{
    return (fips_clock){monotonic_now, NULL};
}

fips_entropy fipsd_host_os_entropy(void)
{
    return (fips_entropy){secure_fill, NULL};
}
