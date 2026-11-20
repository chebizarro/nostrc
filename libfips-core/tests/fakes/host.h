#ifndef FIPS_TEST_HOST_H
#define FIPS_TEST_HOST_H

#include <fips/host.h>

/* Test-only, caller-owned state; never link these predictable sources into hosts. */
typedef struct fips_test_clock { uint64_t millis; } fips_test_clock;
typedef struct fips_fixed_entropy { uint8_t byte; } fips_fixed_entropy;
typedef struct fips_seeded_entropy { uint64_t state; } fips_seeded_entropy;

fips_instant fips_test_instant(uint64_t millis);
fips_clock fips_test_clock_adapter(fips_test_clock *clock);
fips_status fips_test_clock_advance(fips_test_clock *clock, uint64_t millis);
fips_entropy fips_fixed_entropy_adapter(fips_fixed_entropy *entropy);
fips_entropy fips_seeded_entropy_adapter(fips_seeded_entropy *entropy);
uint64_t fips_splitmix64_next(fips_seeded_entropy *entropy);

#endif
