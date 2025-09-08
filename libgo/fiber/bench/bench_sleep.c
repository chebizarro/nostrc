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

typedef struct { uint64_t iters; uint64_t sleep_ns; } args_t;

static void worker(void *argp) {
  args_t *a = (args_t*)argp;
  for (uint64_t i = 0; i < a->iters; ++i) {
    uint64_t ms = a->sleep_ns / 1000000ull;
    if (ms == 0) ms = 1; /* floor to 1ms for millisecond sleep API */
    gof_sleep_ms(ms);
  }
}

int main(int argc, char **argv) {
  uint64_t iters = 10000;
  uint64_t sleep_ns = 1000000ull; /* 1ms */
  int fibers = 1;
  if (argc > 1) fibers = atoi(argv[1]);
  if (argc > 2) iters = strtoull(argv[2], NULL, 10);
  if (argc > 3) sleep_ns = strtoull(argv[3], NULL, 10);

  gof_init(0);
  args_t a = { .iters = iters, .sleep_ns = sleep_ns };
  for (int i = 0; i < fibers; ++i) {
    gof_spawn(worker, &a, 0);
  }
  uint64_t t0 = now_ns();
  gof_run();
  uint64_t t1 = now_ns();
  double sec = (t1 - t0) / 1e9;
  uint64_t total_sleeps = (uint64_t)fibers * iters;
  double per_sec = total_sleeps / sec;
  printf("gof_bench_sleep: fibers=%d iters=%" PRIu64 " sleep_ns=%" PRIu64 " time=%.3fs ops/s=%.2f\n",
         fibers, iters, sleep_ns, sec, per_sec);
  return 0;
}
