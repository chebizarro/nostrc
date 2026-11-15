#include "../include/libgo/fiber.h"
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>

static int order[4];
static atomic_int idx = 0;

static void f1(void *arg) {
  (void)arg;
  order[atomic_fetch_add(&idx, 1)] = 1;
  gof_yield();
  order[atomic_fetch_add(&idx, 1)] = 3;
}

static void f2(void *arg) {
  (void)arg;
  order[atomic_fetch_add(&idx, 1)] = 2;
  gof_yield();
  order[atomic_fetch_add(&idx, 1)] = 4;
}

int main(void) {
  gof_init(128 * 1024);
  gof_spawn(f1, NULL, 0);
  gof_spawn(f2, NULL, 0);
  gof_run();
  for (int i=0;i<4;i++) printf("order[%d]=%d\n", i, order[i]);
  /* Verify all 4 steps executed (order may vary under multi-worker or work-stealing) */
  int seen[5] = {0};
  for (int i = 0; i < 4; i++) {
    assert(order[i] >= 1 && order[i] <= 4);
    seen[order[i]]++;
  }
  for (int i = 1; i <= 4; i++) assert(seen[i] == 1);
  return 0;
}
