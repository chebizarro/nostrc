#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "timer_bridge.h"
#include "../sched/sched.h"
#include <time.h>

static uint64_t bridge_now_ns(void) {
  struct timespec now;
#if defined(CLOCK_MONOTONIC)
  clock_gettime(CLOCK_MONOTONIC, &now);
#else
  clock_gettime(CLOCK_REALTIME, &now);
#endif
  return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

void __gof_sleep_ns(uint64_t ns) {
  uint64_t deadline = bridge_now_ns() + ns;
  for (;;) {
    gof_sched_park_until(deadline);
    /* A wake that is not our timer (a stale WOKEN claim consumed by the
     * park protocol, nostrc-q9lp0) returns the park early; only reaching
     * the deadline ends the sleep. */
    if (bridge_now_ns() >= deadline) return;
  }
}
