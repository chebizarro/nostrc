#include "bench.h"
#include <errno.h>
#include <math.h>
#include <time.h>
uint64_t bench_now(void)
{
    struct timespec ts;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
size_t bench_iterations(void)
{
    const char *s = getenv("FIPS_BENCH_ITERS");
    if (!s) return 1000;
    char *end; errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    REQUIRE(!errno && end != s && !*end && n >= 1 && n <= 10000000);
    return (size_t)n;
}
void bench_observe(const void *value)
{
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" : : "g"(value) : "memory");
#else
#error "Benchmark requires a compiler escape barrier"
#endif
}
static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
void bench_report(const char *name, double samples[BENCH_SAMPLES])
{
    qsort(samples, BENCH_SAMPLES, sizeof(*samples), compare);
    double median = samples[BENCH_SAMPLES / 2];
    REQUIRE(isfinite(median) && median > 0);
    printf("%s,%.3f,%.3f,%.3f\n", name, median, samples[95], 1e9 / median);
}
