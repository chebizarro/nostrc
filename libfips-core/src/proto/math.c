#include <fips/proto/math.h>
#include <float.h>
#if defined(__FAST_MATH__)
#error "fips_powi requires strict IEEE arithmetic"
#endif
_Static_assert(FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024 &&
    sizeof(double) == 8, "fips_powi requires IEEE binary64");

double fips_powi(double base, uint32_t exp)
{
    /* Volatile stores force binary64 rounding even with excess evaluation
     * precision. Each multiply has the exact upstream sequencing. */
    volatile double result = 1.0;
    volatile double b = base;
    for (;;) {
        if ((exp & 1) != 0) result = result * b;
        exp >>= 1;
        if (exp == 0) break;
        b = b * b;
    }
    return result;
}
