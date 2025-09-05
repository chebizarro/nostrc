# Scheduler

This page documents the cooperative fiber scheduler.

## Design

- Work-stealing across worker threads.
- Cooperative yield points via `gof_yield()` and blocking I/O shims.
- Optional periodic rebalancing to smooth queue length skew.
- Affinity routing for sticky I/O (opt-in).

## Code

- Core: `libgo/fiber/sched/`
  - `sched.c`: runnable queues, workers, steal/rebalance logic.
  - `park.c`/`park.h`: worker parking/waking.
  - `api.c`: public API glue and initialization.

## Cooperative parking

The scheduler supports cooperative parking to avoid busy-waiting when no fibers are runnable.

- APIs:
  - `gof_sched_park_until(uint64_t deadline_ns)`: parks the current worker until a deadline or an external wakeup.
  - `gof_sched_unpark_ready(void)`: wakes parked workers if new runnable work arrives.
- Implementation notes:
  - `park.c` implements the platform sleep primitive and delegates to the scheduler for coordination.
  - The timer bridge uses parking instead of `nanosleep()+gof_yield()`; when the next timer is due, the worker sleeps until the deadline.
  - Netpoll integration wakes workers on I/O readiness and enqueues the unblocked fibers before calling `gof_sched_unpark_ready()`.
  - Parking is cooperative: fibers still need yield points (e.g., `gof_yield()`, channel ops, or I/O waits) for fairness.

## Runtime knobs

- Stealing: `gof_set_steal_params()` / `gof_get_steal_params()`.
- Rebalance: `gof_set_rebalance_params()` / `gof_get_rebalance_params()`.
- Affinity: `gof_set_affinity_enabled()` / `gof_get_affinity_enabled()`.
- Netpollers: `gof_set_npollers()` / `gof_get_npollers()` (must be set before `gof_init()`).

## Tracing hooks

Weak hooks you may override in your application:

```c
void __attribute__((weak)) gof_trace_on_switch(uint64_t old_id, uint64_t new_id) {}
void __attribute__((weak)) gof_trace_on_block(int fd, int ev) {}
void __attribute__((weak)) gof_trace_on_unblock(int fd, int ev) {}
```

## Fairness and starvation

- Cooperative progress relies on periodic `gof_yield()` or I/O waits.
- See `libgo/fiber/tests/test_starvation.c` and `GoFiberStarvationTest` for a stress that detects yield fairness regressions.

## Future work

- Cooperative preemption/yield budgeting.
- Additional fairness and work-stealing stress.
- Graceful shutdown semantics and leak detection.
