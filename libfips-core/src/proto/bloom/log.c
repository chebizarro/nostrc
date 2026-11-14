/* Translated from rust-lang/libm 0.2.16 src/math/log.rs (FreeBSD e_log.c).
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 * Developed at SunSoft, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 * The caller's unsaturated bounded filter guarantees a normal x in (0,1].
 * Keep the Rust evaluation order; no FMA/contraction or platform log call.
 */
#include "log.h"
#include <stdint.h>
#include <string.h>
#include <float.h>
_Static_assert(FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024 && sizeof(double) == 8,
    "bloom logarithm requires IEEE binary64");
#if defined(__FAST_MATH__)
#error "bloom logarithm requires strict IEEE arithmetic"
#endif

double fips_bloom_log(double x)
{
    const double ln2_hi = 0x1.62e42fee00000p-1, ln2_lo = 0x1.a39ef35793c76p-33;
    const double lg1 = 0x1.5555555555593p-1, lg2 = 0x1.999999997fa04p-2;
    const double lg3 = 0x1.2492494229359p-2, lg4 = 0x1.c71c51d8e78afp-3;
    const double lg5 = 0x1.7466496cb03dep-3, lg6 = 0x1.39a09d078c69fp-3, lg7 = 0x1.2f112df3e5244p-3;
    uint64_t ui;
    memcpy(&ui, &x, sizeof(ui));
    if (ui == UINT64_C(0x3ff0000000000000)) return 0.0;
    uint32_t hx = (uint32_t)(ui >> 32);
    hx += 0x3ff00000u - 0x3fe6a09eu;
    int k = (int)(hx >> 20) - 0x3ff;
    hx = (hx & 0x000fffffu) + 0x3fe6a09eu;
    ui = ((uint64_t)hx << 32) | (ui & UINT64_C(0xffffffff));
    memcpy(&x, &ui, sizeof(x));
    double f = x - 1.0;
    double hfsq = 0.5 * f * f;
    double s = f / (2.0 + f);
    double z = s * s, w = z * z;
    double t1 = w * (lg2 + w * (lg4 + w * lg6));
    double t2 = z * (lg1 + w * (lg3 + w * (lg5 + w * lg7)));
    double r = t2 + t1, dk = (double)k;
    return s * (hfsq + r) + dk * ln2_lo - hfsq + f + dk * ln2_hi;
}
