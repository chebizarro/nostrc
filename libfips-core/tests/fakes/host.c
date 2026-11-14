#include "host.h"
#include <string.h>

static int compare(fips_instant a, fips_instant b)
{
    return (a.opaque[0] > b.opaque[0]) - (a.opaque[0] < b.opaque[0]);
}

static fips_duration difference(fips_instant a, fips_instant b)
{
    return fips_duration_from_millis(a.opaque[0] > b.opaque[0]
                                   ? a.opaque[0] - b.opaque[0] : 0);
}

static fips_status millis_of(fips_duration d, uint64_t *out)
{
    uint64_t fraction = d.nanos / 1000000;
    if (d.secs > (UINT64_MAX - fraction) / 1000)
        return FIPS_ERR_RANGE;
    *out = d.secs * 1000 + fraction;
    return FIPS_OK;
}

static fips_status checked_add(fips_instant a, fips_duration d, fips_instant *out)
{
    uint64_t ms;
    if (millis_of(d, &ms) != FIPS_OK || ms > UINT64_MAX - a.opaque[0])
        return FIPS_ERR_RANGE;
    *out = fips_test_instant(a.opaque[0] + ms);
    return FIPS_OK;
}

static fips_status checked_sub(fips_instant a, fips_duration d, fips_instant *out)
{
    uint64_t ms;
    if (millis_of(d, &ms) != FIPS_OK || ms > a.opaque[0])
        return FIPS_ERR_RANGE;
    *out = fips_test_instant(a.opaque[0] - ms);
    return FIPS_OK;
}

static const fips_instant_ops instant_ops = {compare, difference, checked_add, checked_sub};

fips_instant fips_test_instant(uint64_t millis)
{
    return (fips_instant){{millis, 0}, &instant_ops};
}

static fips_instant now(void *ctx)
{
    return fips_test_instant(((fips_test_clock *)ctx)->millis);
}

fips_clock fips_test_clock_adapter(fips_test_clock *clock)
{
    return (fips_clock){now, clock};
}

fips_status fips_test_clock_advance(fips_test_clock *clock, uint64_t millis)
{
    if (millis > UINT64_MAX - clock->millis)
        return FIPS_ERR_RANGE;
    clock->millis += millis;
    return FIPS_OK;
}

static void fixed_fill(void *ctx, uint8_t *buf, size_t n)
{
    if (n != 0)
        memset(buf, ((fips_fixed_entropy *)ctx)->byte, n);
}

fips_entropy fips_fixed_entropy_adapter(fips_fixed_entropy *entropy)
{
    return (fips_entropy){fixed_fill, entropy};
}

uint64_t fips_splitmix64_next(fips_seeded_entropy *entropy)
{
    entropy->state += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t z = entropy->state;
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static void seeded_fill(void *ctx, uint8_t *buf, size_t n)
{
    while (n != 0) {
        uint64_t word = fips_splitmix64_next(ctx);
        size_t chunk = n < 8 ? n : 8;
        for (size_t i = 0; i < chunk; ++i)
            buf[i] = (uint8_t)(word >> (8 * i));
        buf += chunk;
        n -= chunk;
        /* Like Rust chunks_mut(8), discard unused bytes at each fill boundary. */
    }
}

fips_entropy fips_seeded_entropy_adapter(fips_seeded_entropy *entropy)
{
    return (fips_entropy){seeded_fill, entropy};
}
