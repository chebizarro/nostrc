#include <fips/host.h>

static int duration_compare(fips_duration a, fips_duration b)
{
    if (a.secs != b.secs)
        return a.secs < b.secs ? -1 : 1;
    return (a.nanos > b.nanos) - (a.nanos < b.nanos);
}

fips_duration fips_duration_from_millis(uint64_t ms)
{
    return (fips_duration){ms / 1000, (uint32_t)(ms % 1000) * 1000000};
}

fips_instant fips_deadline(fips_instant now, fips_duration d)
{
    const fips_duration ceiling = {FIPS_DEADLINE_CEILING_SECS, 0};
    fips_duration step = duration_compare(d, ceiling) > 0 ? ceiling : d;
    for (;;) {
        fips_instant at;
        if (now.ops->checked_add(now, step, &at) == FIPS_OK)
            return at;
        if (step.secs == 0 && step.nanos == 0)
            return now;
        step.nanos = (uint32_t)(((step.secs % 2) * UINT64_C(1000000000)
                                + step.nanos) / 2);
        step.secs /= 2;
    }
}

fips_duration fips_backoff(uint32_t exponent, fips_duration base, fips_duration cap)
{
    uint32_t factor = exponent < 32 ? UINT32_C(1) << exponent : UINT32_MAX;
    uint64_t nanos = (uint64_t)base.nanos * factor;
    uint64_t carry = nanos / UINT64_C(1000000000);
    fips_duration product = {UINT64_MAX, 999999999};
    if (base.secs <= (UINT64_MAX - carry) / factor) {
        product.secs = base.secs * factor + carry;
        product.nanos = (uint32_t)(nanos % UINT64_C(1000000000));
    }
    return duration_compare(product, cap) > 0 ? cap : product;
}
