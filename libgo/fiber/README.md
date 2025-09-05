# go_fiber: Lightweight M:N Fiber Runtime

`go_fiber` is a lightweight cooperative scheduler and IO runtime that provides Go-like fibers, channels (via libgo), timers, and a pluggable netpoller. It integrates with the broader `libgo` library but can be used standalone for fiber-based applications.

This document describes the public API, runtime configuration (environment variables), build options, and benchmarks.

## Getting Started

- Include header: `libgo/fiber/include/libgo/fiber.h`
- Link target: `go_fiber` (static library)
- Minimal example:

```c
#include "libgo/fiber/include/libgo/fiber.h"
#include <stdio.h>

static void hello(void *arg) {
  (void)arg;
  printf("hello from fiber!\n");
}

int main(void) {
  gof_init(0);               // initialize scheduler, default stack size
  gof_spawn(hello, NULL, 0); // spawn a fiber with default stack
  gof_run();                 // run scheduler until all fibers finish
  return 0;
}
```

## Public API

Header: `libgo/fiber/include/libgo/fiber.h`

- Core lifecycle
  - `void gof_init(size_t default_stack_bytes);`
  - `gof_fiber_t* gof_spawn(gof_fn fn, void *arg, size_t stack_bytes);`
  - `void gof_run(void);`
  - `void gof_yield(void);`
  - `void gof_sleep_ms(uint64_t ms);`

- IO helpers (nonblocking + cooperative park/unpark)
  - `ssize_t gof_read(int fd, void *buf, size_t n);`
  - `ssize_t gof_write(int fd, const void *buf, size_t n);`
  - `int gof_connect(int fd, const struct sockaddr *sa, socklen_t slen, int timeout_ms);`
  - `int gof_accept(int fd, struct sockaddr *sa, socklen_t *slen, int timeout_ms);`

- Introspection
  - `size_t gof_list(gof_info *out, size_t max);`
  - `void gof_dump_stacks(int fd);`
  - `void gof_set_name(const char *name);`

- Scheduler stats
  - `typedef struct gof_sched_stats { ... } gof_sched_stats;`
  - `void gof_get_stats(gof_sched_stats *out);`
    - Includes: `nworkers`, `enable_steal`, `affinity_enable`, `steal_min_live`, `steal_min_victim`, `live_fibers`, `steals_attempted`, `steals_success`, `inject_enqueues`, `inject_drains`, and rebalancer stats (`rebalance_enable`, `rebalance_threshold`, `rebalance_interval_ms`, `rebalances_attempted`, `rebalances_migrated`).

- Runtime configuration
  - Work stealing
    - `typedef struct { int enable_steal; int steal_min_live; int steal_min_victim; } gof_steal_params;`
    - `void gof_set_steal_params(const gof_steal_params *p);`
    - `void gof_get_steal_params(gof_steal_params *out);`
  - Periodic rebalancer
    - `typedef struct { int enable; int threshold; int interval_ms; } gof_rebalance_params;`
    - `void gof_set_rebalance_params(const gof_rebalance_params *p);`
    - `void gof_get_rebalance_params(gof_rebalance_params *out);`
  - Affinity routing
    - `void gof_set_affinity_enabled(int enable);`
    - `int  gof_get_affinity_enabled(void);`
  - Netpollers
    - `int  gof_set_npollers(int n);`  // call before init
    - `int  gof_get_npollers(void);`

- Optional trace hooks (weak symbols)
  - `gof_trace_on_switch(uint64_t old_id, uint64_t new_id);`
  - `gof_trace_on_block(int fd, int ev);`
  - `gof_trace_on_unblock(int fd, int ev);`

## Environment Variables

Set before `gof_init()` to control the runtime. Defaults are chosen for general performance and can be overridden at runtime via APIs where applicable.

- Core workers
  - `GOF_NWORKERS` — number of worker threads (default chosen by runtime; clamped 1..64).

- Work stealing
  - `GOF_WORKSTEAL` — enable work stealing (default: 1)
  - `GOF_STEAL_MIN_LIVE` — do not steal when victim has fewer than this many live runnables (default: 4)
  - `GOF_STEAL_MIN_VICTIM` — victim must have at least this many runnables to steal (default: 3, min 2)

- Netpollers and partitioning
  - `GOF_NPOLLERS` — number of netpoll threads (default: 1; capped by `nworkers`)
  - `GOF_POLL_PARTITION` — partition-ready wakeups to workers owned by a poller (default: 1)

- Affinity routing
  - `GOF_AFFINITY` — prefer last-run worker for a fiber to improve locality (default: 1)

- Periodic rebalancer
  - `GOF_REBALANCE` — enable periodic rebalancing (default: 0)
  - `GOF_REBALANCE_THRESHOLD` — migrate when imbalance ≥ threshold (default: 4; min 1)
  - `GOF_REBALANCE_INTERVAL_MS` — check interval in ms (default: 10; min 1)

- Debug and tracing
  - `GOF_DEBUG` (CMake option) — compile with verbose scheduler logging.
  - `GOF_ENABLE_TRACING` (CMake option) — enable lightweight tracing hooks.

## Build

Using the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Fiber unit tests (CTest labels: `GoFiber*`) can be run with:

```bash
ctest --test-dir build -R "GoFiber" --output-on-failure -j 4
```

## Benchmarks

The fiber library includes several microbenchmarks under `libgo/fiber/bench/`:

- `gof_bench_yield` — cooperative yield overhead
- `gof_bench_pingpong` — fiber-to-fiber context switch latency via channel
- `gof_bench_sleep` — timed sleep accuracy and overhead
- `gof_bench_poll_partition` — IO throughput test to measure impact of `GOF_POLL_PARTITION` and `GOF_AFFINITY`

Build targets are added automatically when building the project. Example usage:

```bash
# Example: compare partitioned vs non-partitioned wakeups
export GOF_NWORKERS=8
export GOF_NPOLLERS=4

# Partitioned on (default)
./build/libgo/gof_bench_poll_partition -conns 256 -duration_ms 2000 -msg_size 64

# Partitioned off
export GOF_POLL_PARTITION=0
./build/libgo/gof_bench_poll_partition -conns 256 -duration_ms 2000 -msg_size 64

# Try disabling affinity
export GOF_AFFINITY=0
./build/libgo/gof_bench_poll_partition -conns 256 -duration_ms 2000 -msg_size 64

# Enable periodic rebalancer
export GOF_REBALANCE=1
export GOF_REBALANCE_THRESHOLD=4
export GOF_REBALANCE_INTERVAL_MS=10
./build/libgo/gof_bench_poll_partition -conns 256 -duration_ms 2000 -msg_size 64
```

The benchmark prints total messages transferred and aggregated throughput; 
use it to compare the effects of partitioning, affinity, and rebalancing under varying worker/poller counts.

## Notes

- The runtime is designed for aggressive refactors; keep internal tests passing to validate functional changes.
- The scheduler implements cooperative parking and integrates timers to avoid busy waits.
- Netpoll backends currently include kqueue (Darwin) and epoll (Linux); IOCP is a placeholder.
