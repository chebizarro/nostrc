#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include "../include/libgo/fiber.h"

static atomic_uint_least64_t g_counter = 0;

typedef struct {
  int yields;
} task_arg;

static void worker(void *argp) {
  task_arg *a = (task_arg*)argp;
  for (int i = 0; i < a->yields; ++i) {
    atomic_fetch_add_explicit(&g_counter, 1, memory_order_relaxed);
    gof_yield();
  }
  free(a);
}

int main(void) {
  const int nfib = 1000;
  const int nyield = 1000;

  gof_init(64 * 1024);

  for (int i = 0; i < nfib; ++i) {
    task_arg *a = (task_arg*)malloc(sizeof(task_arg));
    if (!a) { perror("malloc"); return 1; }
    a->yields = nyield;
    if (!gof_spawn(worker, a, 64 * 1024)) {
      fprintf(stderr, "spawn failed at %d\n", i);
      return 1;
    }
  }

  gof_run();

  uint64_t expect = (uint64_t)nfib * (uint64_t)nyield;
  uint64_t got = atomic_load_explicit(&g_counter, memory_order_relaxed);
  if (got != expect) {
    fprintf(stderr, "starvation test failed: got=%llu expect=%llu\n",
            (unsigned long long)got, (unsigned long long)expect);
    return 2;
  }

  printf("starvation test passed: fibers=%d yields=%d total=%llu\n",
         nfib, nyield, (unsigned long long)got);
  return 0;
}
