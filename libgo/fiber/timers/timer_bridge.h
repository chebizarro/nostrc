#ifndef LIBGO_FIBER_TIMERS_TIMER_BRIDGE_H
#define LIBGO_FIBER_TIMERS_TIMER_BRIDGE_H
#include <stdint.h>

/* Internal: sleep helper in nanoseconds */
void __gof_sleep_ns(uint64_t ns);
#endif /* LIBGO_FIBER_TIMERS_TIMER_BRIDGE_H */
