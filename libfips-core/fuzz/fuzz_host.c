#include <fips/host.h>
#include "../tests/fakes/host.h"
#include <stdlib.h>

/* Fixed little-endian fields, zero-padded for short inputs; no invalid host
 * pointers or non-normalized durations enter the public contract. */
static uint64_t word(const uint8_t **data, size_t *size)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8 && *size != 0; ++i) {
        value |= (uint64_t)*(*data)++ << (8 * i);
        --*size;
    }
    return value;
}

static void require(int condition)
{
    if (!condition)
        abort();
}

static int le(fips_duration a, fips_duration b)
{
    return a.secs < b.secs || (a.secs == b.secs && a.nanos <= b.nanos);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint64_t ms = word(&data, &size);
    fips_duration converted = fips_duration_from_millis(ms);
    require(converted.secs == ms / 1000);
    require(converted.nanos == (ms % 1000) * 1000000);
    fips_instant now = fips_test_instant(word(&data, &size));
    fips_duration base = {word(&data, &size), 0};
    base.nanos = (uint32_t)(word(&data, &size) % 1000000000);
    fips_duration cap = {word(&data, &size), 0};
    cap.nanos = (uint32_t)(word(&data, &size) % 1000000000);
    uint32_t exponent = (uint32_t)word(&data, &size);
    fips_duration backoff = fips_backoff(exponent, base, cap);
    require(backoff.nanos < 1000000000 && le(backoff, cap));
    require(le(fips_backoff(0, base, cap), backoff));
    fips_instant deadline = fips_deadline(now, base);
    require(now.ops->compare(deadline, now) >= 0);
    fips_duration elapsed = now.ops->saturating_duration_since(deadline, now);
    require(le(elapsed, base));
    require(le(elapsed, (fips_duration){FIPS_DEADLINE_CEILING_SECS, 0}));
    fips_instant sum, restored;
    if (now.ops->checked_add(now, converted, &sum) == FIPS_OK) {
        require(now.ops->checked_sub(sum, converted, &restored) == FIPS_OK);
        require(now.ops->compare(restored, now) == 0);
    }
    require(now.ops->saturating_duration_since(now, deadline).secs == 0);
    return 0;
}
