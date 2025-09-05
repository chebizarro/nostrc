#include "../include/libgo/fiber.h"
#include "../include/libgo/fiber_chan.h"
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>

static gof_chan_t *ch;
static atomic_int recv_count = 0;

static void producer(void *arg) {
  int n = (int)(uintptr_t)arg;
  for (int i = 0; i < n; i++) {
    void *v = (void*)(uintptr_t)(i + 1);
    int r = gof_chan_send(ch, v);
    assert(r == 0);
    if ((i & 7) == 0) gof_yield();
  }
}

static void consumer(void *arg) {
  int n = (int)(uintptr_t)arg;
  for (int i = 0; i < n; i++) {
    void *v = NULL;
    int r = gof_chan_recv(ch, &v);
    assert(r == 0);
    int x = (int)(uintptr_t)v;
    assert(x >= 1);
    atomic_fetch_add(&recv_count, 1);
    if ((i & 3) == 0) gof_yield();
  }
}

int main(void) {
  gof_init(128 * 1024);
  const int N = 1000;
  ch = gof_chan_make(8);
  assert(ch);
  gof_spawn(producer, (void*)(uintptr_t)N, 0);
  gof_spawn(consumer, (void*)(uintptr_t)N, 0);
  gof_run();
  int rc = atomic_load(&recv_count);
  printf("received=%d expected=%d\n", rc, N);
  assert(rc == N);
  gof_chan_close(ch);
  return 0;
}
