#include "host_os.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

int main(void)
{
    fips_clock clock = fipsd_host_os_clock();
    fips_instant a = clock.now(clock.ctx), b = clock.now(clock.ctx), out;
    CHECK(a.ops == b.ops && a.ops->compare(a, b) <= 0);
    fips_duration delta = a.ops->saturating_duration_since(b, a);
    CHECK(a.ops->checked_add(a, delta, &out) == FIPS_OK);
    CHECK(a.ops->compare(out, b) == 0);
    CHECK(a.ops->checked_sub(b, delta, &out) == FIPS_OK);
    CHECK(a.ops->compare(out, a) == 0);
    delta = a.ops->saturating_duration_since(a, b);
    CHECK(delta.secs == 0 && delta.nanos == 0);

    /* Same ops, synthetic edge values: exercise nanosecond carry/borrow and
     * representation limits without sleeps or assumptions about system uptime. */
    a.opaque[0] = 1;
    a.opaque[1] = 999999999;
    CHECK(a.ops->checked_add(a, (fips_duration){0, 1}, &out) == FIPS_OK);
    CHECK(out.opaque[0] == 2 && out.opaque[1] == 0);
    CHECK(a.ops->checked_sub(out, (fips_duration){0, 1}, &out) == FIPS_OK);
    CHECK(a.ops->compare(a, out) == 0);
    b = a;
    b.opaque[0] = 2;
    b.opaque[1] = 0;
    delta = a.ops->saturating_duration_since(b, a);
    CHECK(delta.secs == 0 && delta.nanos == 1);
    a.opaque[0] = UINT64_MAX;
    CHECK(a.ops->checked_add(a, (fips_duration){0, 1}, &out) == FIPS_ERR_RANGE);
    CHECK(out.opaque[0] == 1 && out.opaque[1] == 999999999);
    CHECK(a.ops->checked_add(a, (fips_duration){1, 0}, &out) == FIPS_ERR_RANGE);
    CHECK(a.ops->checked_add(a, (fips_duration){0, 0}, &out) == FIPS_OK);
    a.opaque[0] = a.opaque[1] = 0;
    CHECK(a.ops->checked_sub(a, (fips_duration){0, 1}, &out) == FIPS_ERR_RANGE);
    CHECK(a.ops->checked_sub(a, (fips_duration){1, 0}, &out) == FIPS_ERR_RANGE);
    CHECK(out.opaque[0] == UINT64_MAX && out.opaque[1] == 999999999);

    fips_entropy entropy = fipsd_host_os_entropy();
    uint8_t buf[34];
    memset(buf, 0xa5, sizeof(buf));
    entropy.fill(entropy.ctx, NULL, 0);
    entropy.fill(entropy.ctx, buf + 1, 32);
    CHECK(buf[0] == 0xa5 && buf[33] == 0xa5);
    /* No statistical/random-value assertion: this tests the real adapter's
     * execution and buffer contract, not the RNG's cryptographic quality. */
    return 0;
}
