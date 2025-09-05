#include "timer_bridge.h"
#include "../sched/sched.h"
#include <time.h>

void __gof_sleep_ns(uint64_t ns) {
  struct timespec now;
#if defined(CLOCK_MONOTONIC)
  clock_gettime(CLOCK_MONOTONIC, &now);
#else
  clock_gettime(CLOCK_REALTIME, &now);
#endif
  uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
  uint64_t deadline = now_ns + ns;
  gof_sched_park_until(deadline);
}
