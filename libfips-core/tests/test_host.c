#include "fakes/host.h"
#include "seeded_vectors.h"
#include <stdio.h>
#include <string.h>

/* Unlike assert(), keep checks active in release builds too. */
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

static int duration_equal(fips_duration a, fips_duration b)
{
    return a.secs == b.secs && a.nanos == b.nanos;
}

static int test_clock(void)
{
    fips_test_clock state = {0};
    fips_clock clock = fips_test_clock_adapter(&state);
    fips_instant zero = clock.now(clock.ctx);
    CHECK(zero.opaque[0] == 0);
    CHECK(fips_test_clock_advance(&state, 1500) == FIPS_OK);
    CHECK(clock.now(clock.ctx).opaque[0] == 1500);
    CHECK(fips_test_clock_advance(&state, 500) == FIPS_OK);
    fips_instant now = clock.now(clock.ctx);
    CHECK(now.opaque[0] == 2000);
    CHECK(clock.now(clock.ctx).ops->compare(now, clock.now(clock.ctx)) == 0);
    CHECK(now.ops->compare(zero, now) < 0 && now.ops->compare(now, zero) > 0);
    CHECK(duration_equal(now.ops->saturating_duration_since(now, zero),
                         (fips_duration){2, 0}));
    CHECK(duration_equal(now.ops->saturating_duration_since(zero, now),
                         (fips_duration){0, 0}));
    CHECK(duration_equal(now.ops->saturating_duration_since(now, now),
                         (fips_duration){0, 0}));
    fips_instant out = zero;
    CHECK(now.ops->checked_add(now, (fips_duration){1, 999999}, &out) == FIPS_OK);
    CHECK(out.opaque[0] == 3000); /* Drop fractional milliseconds, like Rust. */
    CHECK(now.ops->checked_sub(out, (fips_duration){1, 0}, &out) == FIPS_OK);
    CHECK(now.ops->compare(out, now) == 0);
    CHECK(now.ops->checked_add(now, (fips_duration){0, 999999}, &out) == FIPS_OK);
    CHECK(now.ops->compare(out, now) == 0);
    CHECK(now.ops->checked_sub(zero, fips_duration_from_millis(1), &out) == FIPS_ERR_RANGE);
    CHECK(now.ops->compare(out, now) == 0); /* Failure leaves out unchanged. */
    CHECK(now.ops->checked_sub(now, (fips_duration){UINT64_MAX, 999999999}, &out)
          == FIPS_ERR_RANGE);
    CHECK(now.ops->checked_add(now, (fips_duration){UINT64_MAX, 999999999}, &out)
          == FIPS_ERR_RANGE);
    fips_instant end = fips_test_instant(UINT64_MAX);
    CHECK(end.ops->checked_add(end, fips_duration_from_millis(1), &out) == FIPS_ERR_RANGE);
    CHECK(end.ops->checked_add(end, (fips_duration){0, 0}, &out) == FIPS_OK);
    CHECK(out.ops->compare(out, end) == 0);
    CHECK(end.ops->checked_sub(end, fips_duration_from_millis(UINT64_MAX), &out) == FIPS_OK);
    CHECK(out.ops->compare(out, zero) == 0);
    CHECK(end.ops->checked_add(zero, fips_duration_from_millis(UINT64_MAX), &out) == FIPS_OK);
    CHECK(out.ops->compare(out, end) == 0);
    CHECK(fips_test_clock_advance(&state, UINT64_MAX) == FIPS_ERR_RANGE);
    CHECK(state.millis == 2000);
    return 0;
}

static int test_helpers(void)
{
    fips_instant now = fips_test_instant(5000);
    CHECK(fips_deadline(now, fips_duration_from_millis(2500)).opaque[0] == 7500);
    const fips_duration max = {UINT64_MAX, 999999999};
    const fips_duration ceiling = {FIPS_DEADLINE_CEILING_SECS, 0};
    CHECK(fips_deadline(now, ceiling).opaque[0] == 5000 + FIPS_DEADLINE_CEILING_SECS * 1000);
    CHECK(fips_deadline(now, max).opaque[0] == fips_deadline(now, ceiling).opaque[0]);
    CHECK(fips_deadline(now, (fips_duration){FIPS_DEADLINE_CEILING_SECS, 1}).opaque[0]
          == fips_deadline(now, ceiling).opaque[0]);
    CHECK(fips_deadline(now, (fips_duration){0, 0}).opaque[0] == 5000);
    CHECK(fips_deadline(fips_test_instant(UINT64_MAX - 10), (fips_duration){60, 0}).opaque[0]
          == UINT64_MAX - 3); /* 60s / 8192 -> 7 whole ms, not clamping to MAX. */
    CHECK(fips_deadline(fips_test_instant(UINT64_MAX), max).opaque[0] == UINT64_MAX);
    CHECK(duration_equal(fips_backoff(0, (fips_duration){1, 0}, max), (fips_duration){1, 0}));
    CHECK(duration_equal(fips_backoff(4, (fips_duration){1, 0}, max), (fips_duration){16, 0}));
    CHECK(duration_equal(fips_backoff(4, (fips_duration){30, 0}, (fips_duration){600, 0}),
                         (fips_duration){480, 0}));
    CHECK(duration_equal(fips_backoff(5, (fips_duration){30, 0}, (fips_duration){600, 0}),
                         (fips_duration){600, 0}));
    CHECK(duration_equal(fips_backoff(31, (fips_duration){1, 0}, max),
                         (fips_duration){UINT64_C(2147483648), 0}));
    CHECK(duration_equal(fips_backoff(32, (fips_duration){1, 0}, max),
                         (fips_duration){UINT32_MAX, 0}));
    CHECK(duration_equal(fips_backoff(UINT32_MAX, (fips_duration){1, 0}, max),
                         fips_backoff(32, (fips_duration){1, 0}, max)));
    CHECK(duration_equal(fips_backoff(3, max, max), max));
    CHECK(duration_equal(fips_backoff(0, max, max), max));
    CHECK(duration_equal(fips_backoff(1, (fips_duration){0, 750000001}, max),
                         (fips_duration){1, 500000002}));
    CHECK(duration_equal(fips_backoff(32, (fips_duration){UINT64_MAX / UINT32_MAX, 999999999},
                                     max), max)); /* Overflow caused by nanos carry. */
    CHECK(duration_equal(fips_backoff(0, (fips_duration){1, 0}, (fips_duration){0, 1}),
                         (fips_duration){0, 1}));
    CHECK(duration_equal(fips_backoff(UINT32_MAX, (fips_duration){0, 0}, max),
                         (fips_duration){0, 0}));
    CHECK(duration_equal(fips_backoff(1, max, (fips_duration){0, 0}),
                         (fips_duration){0, 0}));
    return 0;
}

static unsigned int hex_digit(char c)
{
    return (unsigned int)(c <= '9' ? c - '0' : c - 'a' + 10);
}

static int test_entropy(void)
{
    uint8_t buf[1026], expected[1024];
    fips_fixed_entropy fixed = {0xa5};
    fips_entropy entropy = fips_fixed_entropy_adapter(&fixed);
    memset(buf, 0xcc, sizeof(buf));
    entropy.fill(entropy.ctx, NULL, 0);
    entropy.fill(entropy.ctx, buf + 1, 1024);
    CHECK(buf[0] == 0xcc && buf[1025] == 0xcc);
    for (size_t i = 1; i <= 1024; ++i)
        CHECK(buf[i] == 0xa5);

    const uint64_t seeds[] = {0, UINT64_C(0x0123456789abcdef), UINT64_MAX};
    const size_t lengths[] = {0, 1, 7, 8, 9, 15, 16, 17, 31, 32, 63, 65, 255};
    for (size_t seed = 0; seed < 3; ++seed) {
        CHECK(strlen(expected_hex[seed]) == 2048);
        for (size_t i = 0; i < sizeof(expected); ++i)
            expected[i] = (uint8_t)(16 * hex_digit(expected_hex[seed][2 * i])
                                    + hex_digit(expected_hex[seed][2 * i + 1]));
        fips_seeded_entropy state = {seeds[seed]};
        entropy = fips_seeded_entropy_adapter(&state);
        entropy.fill(entropy.ctx, buf + 1, sizeof(expected));
        CHECK(memcmp(buf + 1, expected, sizeof(expected)) == 0);
        CHECK(buf[0] == 0xcc && buf[1025] == 0xcc);
        state.state = seeds[seed];
        size_t offset = 0;
        for (size_t j = 0; j < sizeof(lengths) / sizeof(lengths[0]); ++j) {
            size_t n = lengths[j];
            memset(buf, 0xcc, sizeof(buf));
            entropy.fill(entropy.ctx, NULL, 0); /* Must not advance the generator. */
            entropy.fill(entropy.ctx, buf + 1, n);
            CHECK(buf[0] == 0xcc && buf[n + 1] == 0xcc);
            CHECK(memcmp(buf + 1, expected + offset, n) == 0);
            offset += ((n + 7) / 8) * 8; /* Discard tail bytes after EVERY fill. */
        }
        state.state = seeds[seed];
        uint64_t first = fips_splitmix64_next(&state);
        for (size_t i = 0; i < 8; ++i)
            CHECK((uint8_t)(first >> (8 * i)) == expected[i]);
    }
    return 0;
}

int main(void)
{
    return test_clock() || test_helpers() || test_entropy();
}
