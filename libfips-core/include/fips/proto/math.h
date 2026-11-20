#ifndef FIPS_PROTO_MATH_H
#define FIPS_PROTO_MATH_H
#include <fips/host.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Multiply-then-square, matching upstream, not the platform pow/powi routine.
 * Requires IEEE binary64, round-to-nearest/ties-even and gradual underflow.
 * Host must not enable flush-to-zero or change rounding during core calls.
 * exp == 0 returns 1, including zero/NaN/infinite bases. No libm or IO. */
FIPS_CORE_API double fips_powi(double base, uint32_t exp);
#ifdef __cplusplus
}
#endif
#endif
