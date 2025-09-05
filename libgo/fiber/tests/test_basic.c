#include "../include/libgo/fiber.h"
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>

static atomic_int counter = 0;

static void worker(void *arg) {
  int iters = (int)(uintptr_t)arg;
  for (int i = 0; i < iters; i++) {
    atomic_fetch_add(&counter, 1);
    gof_yield();
  }
}

int main(void) {
  gof_init(128 * 1024);
  const int N = 100;
  const int ITERS = 10;
  for (int i = 0; i < N; i++) {
    gof_spawn(worker, (void*)(uintptr_t)ITERS, 0);
  }
  gof_run();
  int expected = N * ITERS;
  printf("counter=%d expected=%d\n", counter, expected);
  assert(counter == expected);
  return 0;
}
