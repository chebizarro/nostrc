#include <fips/proto/rate_limit.h>
#include <string.h>
#include <stdbool.h>

/* Quantize timestamps BEFORE subtraction, matching the proto u64-ms API. */
static uint64_t millis(fips_instant now, fips_instant origin)
{
    fips_duration d = now.ops->saturating_duration_since(now, origin);
    if (d.secs > UINT64_MAX / 1000) return UINT64_MAX;
    uint64_t whole = d.secs * 1000;
    uint64_t part = d.nanos / 1000000;
    return part > UINT64_MAX - whole ? UINT64_MAX : whole + part;
}

static bool younger(uint64_t now, uint64_t last, uint64_t age)
{
    return (now < last ? 0 : now - last) < age;
}

static size_t position(const fips_per_addr_rate_limiter *l, const uint8_t addr[16])
{
    size_t lo = 0, hi = l->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (memcmp(l->entries[mid].addr, addr, 16) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void fips_per_addr_init(fips_per_addr_rate_limiter *l, uint64_t interval,
                        uint64_t age, fips_instant origin)
{
    l->len = 0;
    l->min_interval_ms = interval;
    l->max_age_ms = age;
    l->sweeps = 0;
    l->origin = origin;
    l->last_sweep_ms = 0;
}

void fips_per_addr_cleanup(fips_per_addr_rate_limiter *l, fips_instant now)
{
    uint64_t now_ms = millis(now, l->origin);
    size_t n = 0;
    for (size_t i = 0; i < l->len; ++i)
        if (younger(now_ms, l->entries[i].last_ms, l->max_age_ms))
            l->entries[n++] = l->entries[i];
    l->len = n;
}

fips_record_outcome fips_per_addr_check_and_record(fips_per_addr_rate_limiter *l,
                                                  const uint8_t addr[16], fips_instant now)
{
    uint64_t now_ms = millis(now, l->origin);
    size_t i = position(l, addr);
    int known = i < l->len && memcmp(l->entries[i].addr, addr, 16) == 0;
    if (known && younger(now_ms, l->entries[i].last_ms, l->min_interval_ms))
        return FIPS_RECORD_SUPPRESS;
    uint64_t interval = l->max_age_ms / 8;
    if (interval == 0) interval = 1;
    if (!younger(now_ms, l->last_sweep_ms, interval) || l->last_sweep_ms == 0) {
        l->last_sweep_ms = now_ms;
        if (l->sweeps != UINT64_MAX) ++l->sweeps;
        fips_per_addr_cleanup(l, now);
    }
    if (!known && l->len == FIPS_PER_ADDR_MAX_ENTRIES)
        return FIPS_RECORD_ADMIT_AT_CAPACITY;
    i = position(l, addr);
    if (i == l->len || memcmp(l->entries[i].addr, addr, 16) != 0) {
        memmove(&l->entries[i + 1], &l->entries[i], (l->len - i) * sizeof(l->entries[0]));
        ++l->len;
    }
    memcpy(l->entries[i].addr, addr, 16);
    l->entries[i].last_ms = now_ms;
    return FIPS_RECORD_ADMIT;
}

uint64_t fips_backoff_ms(uint32_t exponent, uint64_t base, uint64_t cap)
{
    uint64_t factor = exponent < 64 ? UINT64_C(1) << exponent : UINT64_MAX;
    uint64_t value = base != 0 && factor > UINT64_MAX / base ? UINT64_MAX : base * factor;
    return value < cap ? value : cap;
}
