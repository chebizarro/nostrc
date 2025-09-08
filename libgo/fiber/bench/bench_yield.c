#define _POSIX_C_SOURCE 200809L
#include "../include/libgo/fiber.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

static uint64_t now_ns(void) {
  struct timespec ts;
#if defined(CLOCK_MONOTONIC)
  clock_gettime(CLOCK_MONOTONIC, &ts);
#else
  clock_gettime(CLOCK_REALTIME, &ts);
#endif
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef struct {
  uint64_t iters;
} worker_args;

static void worker(void *argp) {
  worker_args *wa = (worker_args*)argp;
  for (uint64_t i = 0; i < wa->iters; ++i) {
    gof_yield();
  }
}

int main(int argc, char **argv) {
  int64_t n_fibers = 2;
  uint64_t iters = 100000;
  if (argc > 1) n_fibers = strtoll(argv[1], NULL, 10);
  if (argc > 2) iters     = strtoull(argv[2], NULL, 10);
  if (n_fibers < 1) n_fibers = 1;

  gof_init(0);
  worker_args wa = { .iters = iters };
  for (int i = 0; i < n_fibers; ++i) {
    gof_spawn(worker, &wa, 0);
  }

  uint64_t t0 = now_ns();
  gof_run();
  uint64_t t1 = now_ns();
  double sec = (t1 - t0) / 1e9;
  uint64_t total_switches = (uint64_t)n_fibers * iters;
  double mps = total_switches / 1e6 / sec;
  printf("gof_bench_yield: fibers=%lld iters=%" PRIu64 " time=%.3fs switches=%" PRIu64 " (%.2f M/s)\n",
         (long long)n_fibers, iters, sec, total_switches, mps);
  return 0;
}
