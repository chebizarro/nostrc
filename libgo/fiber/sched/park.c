#include "park.h"
#include "sched.h"
#include <stdint.h>

void gof_park_current_until(uint64_t deadline_ns) {
  gof_sched_park_until(deadline_ns);
}

void gof_unpark_ready(void) {
  gof_sched_unpark_ready();
}
