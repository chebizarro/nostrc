#define _POSIX_C_SOURCE 200809L
#include "../include/libgo/fiber.h"
#include "../include/libgo/fiber_chan.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
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
  gof_chan_t *c;
  uint64_t rounds;
} args_t;

static void ping(void *arg) {
  args_t *a = (args_t*)arg;
  for (uint64_t i = 0; i < a->rounds; ++i) {
    (void)gof_chan_send(a->c, (void*)(uintptr_t)1);
    void *v = NULL;
    (void)gof_chan_recv(a->c, &v);
  }
}

static void pong(void *arg) {
  args_t *a = (args_t*)arg;
  for (uint64_t i = 0; i < a->rounds; ++i) {
    void *v = NULL;
    (void)gof_chan_recv(a->c, &v);
    (void)gof_chan_send(a->c, v);
  }
}

int main(int argc, char **argv) {
  uint64_t rounds = 100000;
  size_t capacity = 0; // rendezvous by default
  if (argc > 1) rounds = strtoull(argv[1], NULL, 10);
  if (argc > 2) capacity = (size_t)strtoull(argv[2], NULL, 10);

  gof_init(0);
  gof_chan_t *c = gof_chan_make(capacity);
  args_t a = { .c = c, .rounds = rounds };

  gof_spawn(pong, &a, 0);
  gof_spawn(ping, &a, 0);

  uint64_t t0 = now_ns();
  gof_run();
  uint64_t t1 = now_ns();
  double sec = (t1 - t0) / 1e9;
  double msg = rounds * 2.0; // send+recv per round
  double mps = msg / 1e6 / sec;
  printf("gof_bench_pingpong: rounds=%" PRIu64 " cap=%zu time=%.3fs msgs=%.0f (%.2f M/s)\n",
         rounds, capacity, sec, msg, mps);

  gof_chan_close(c);
  return 0;
}
