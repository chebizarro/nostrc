#ifndef FIPS_BENCH_H
#define FIPS_BENCH_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define BENCH_SAMPLES 101u
#define REQUIRE(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)
uint64_t bench_now(void);
size_t bench_iterations(void);
void bench_report(const char *name, double samples[BENCH_SAMPLES]);
/* Compiler barrier, not a volatile per-byte copy; preserve the actual work. */
void bench_observe(const void *value);
/* Ten untimed batches warm caches; N iterations per sample, 101 samples.
 * p95 is of batch ns/op, not a claim about individual operation tails. */
#define BENCH(name, ...) do { \
    size_t bench_n = bench_iterations(); \
    double bench_samples[BENCH_SAMPLES]; \
    for (unsigned bench_s = 0; bench_s < BENCH_SAMPLES + 10; ++bench_s) { \
        uint64_t bench_start = bench_now(); \
        for (size_t bench_i = 0; bench_i < bench_n; ++bench_i) { __VA_ARGS__; } \
        uint64_t bench_elapsed = bench_now() - bench_start; \
        if (bench_s >= 10) bench_samples[bench_s - 10] = (double)bench_elapsed / (double)bench_n; \
    } \
    bench_report(name, bench_samples); \
} while (0)
#endif
