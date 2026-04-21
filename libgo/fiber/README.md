# go_fiber: Cooperative M:N Fiber Runtime

`go_fiber` is a cooperative, stackful M:N fiber runtime providing Go-like
lightweight threads, bounded channels, timers, and an integrated I/O netpoller.
Fibers are multiplexed across a pool of OS worker threads with per-worker run
queues, work-stealing, affinity routing, and partition-aware netpolling.

It integrates with the broader `libgo` library (channels, hook-based
primitives) but can be built and used standalone for fiber-based applications.

## Getting Started

**Header:** `libgo/fiber/include/libgo/fiber.h`
**Link target:** `go_fiber` (static library)

```c
#include "libgo/fiber/include/libgo/fiber.h"
#include <stdio.h>

static void hello(void *arg) {
  (void)arg;
  printf("hello from fiber!\n");
}

int main(void) {
  gof_init(0);               // initialize scheduler, default stack size
  gof_spawn(hello, NULL, 0); // spawn a fiber
  gof_run();                 // run until all fibers complete
  return 0;
}
```

## Architecture

```
┌───────────────────────────────────────────────────────┐
│                  Public API (fiber.h)                  │
├───────────────────────────────────────────────────────┤
│              Scheduler  (sched/sched.c)               │
│  ┌─────────┐ ┌─────────┐        ┌─────────┐          │
│  │Worker 0 │ │Worker 1 │  ...   │Worker N │          │
│  │ rq + µ  │ │ rq + µ  │        │ rq + µ  │          │
│  └────┬────┘ └────┬────┘        └────┬────┘          │
│       │  work-steal / inject queue   │               │
├───────┴──────────────────────────────┴───────────────┤
│  Context Switch    │  Sleep Heap    │  IO Layer       │
│  (asm / ucontext)  │  (min-heap)    │  (netpoll)      │
│  x86-64, ARM64,    │  O(log N)      │  epoll / kqueue │
│  portable fallback  │  insert+wake   │  / IOCP (stub)  │
├───────────────────────────────────────────────────────┤
│  Channels (chan.c)  │  Timers        │  Debug/Trace    │
│  buffered + unbuf   │  (timer_bridge)│  introspect +   │
│  MPMC, cooperative  │                │  atomic counters │
└───────────────────────────────────────────────────────┘
```

### Key internals

- **Per-worker run queues** with mutex-protected deques, atomic `rq_len`
  counters for lock-free idle scanning, and randomized work-stealing.
- **Global inject queue** for cross-thread fiber submission
  (pre-allocated nodes to avoid malloc under lock).
- **Binary min-heap sleep queue** (`sleepheap`) under its own mutex,
  decoupled from the global scheduler lock. O(log N) insert and wake.
- **Atomic fiber state machine** (`GOF_RUNNABLE → GOF_BLOCKED → GOF_FINISHED`)
  with CAS transitions to prevent double-enqueue races between timers and
  the I/O poller.
- **Context switching** via hand-rolled assembly for x86-64 and ARM64
  (SysV ABI / AAPCS64), with a `ucontext` portable fallback
  (`GOF_PORTABLE_CONTEXT=1`).
- **Guard-page stacks** allocated with `mmap` + `mprotect` (Unix) or
  `VirtualAlloc` (Windows), with error checking on protection calls.

## Public API

### Core lifecycle

```c
void         gof_init(size_t default_stack_bytes);
gof_fiber_t* gof_spawn(gof_fn fn, void *arg, size_t stack_bytes);
void         gof_run(void);
void         gof_yield(void);
void         gof_sleep_ms(uint64_t ms);
```

### Background mode

For applications with their own event loop (GTK, GLib, etc.), the scheduler
can run entirely on background threads:

```c
int  gof_start_background(size_t default_stack_bytes);
void gof_request_stop(void);
int  gof_join_background(void);
int  gof_in_fiber(void);
```

### Fiber-friendly I/O

Nonblocking wrappers that park the calling fiber on the netpoller and resume
it when the fd becomes ready (or a timeout expires):

```c
ssize_t gof_read(int fd, void *buf, size_t n);
ssize_t gof_write(int fd, const void *buf, size_t n);
int     gof_connect(int fd, const struct sockaddr *sa, socklen_t slen, int timeout_ms);
int     gof_accept(int fd, struct sockaddr *sa, socklen_t *slen, int timeout_ms);
```

Each fiber tracks its current `wait_fd` / `wait_events` for O(1) waiter
removal when a timeout fires before an I/O event.

### Channels

Header: `libgo/fiber/include/libgo/fiber_chan.h`

Bounded MPMC channels for pointer-sized messages. Unbuffered (capacity 0)
channels perform a synchronous rendezvous between sender and receiver.

```c
gof_chan_t* gof_chan_make(size_t capacity);
void        gof_chan_close(gof_chan_t *c);

int         gof_chan_send(gof_chan_t *c, void *value);    // blocking
int         gof_chan_recv(gof_chan_t *c, void **out);      // blocking
int         gof_chan_try_send(gof_chan_t *c, void *value); // non-blocking
int         gof_chan_try_recv(gof_chan_t *c, void **out);  // non-blocking
```

- Returns `0` on success, `-1` on closed channel.
- `gof_chan_close()` wakes all blocked waiters; they observe closure via a
  stack-local `done` flag (handoff sets `done=1`, close does not).

### Introspection

```c
gof_fiber_t* gof_current(void);
size_t       gof_list(gof_info *out, size_t max);
void         gof_dump_stacks(int fd);
void         gof_set_name(const char *name);
```

`gof_dump_stacks` buffers output under lock and writes after releasing it
to avoid interleaved output from concurrent callers.

### Scheduler statistics

```c
typedef struct gof_sched_stats {
  int      nworkers;
  int      enable_steal;
  int      affinity_enable;
  int      steal_min_live;
  int      steal_min_victim;
  uint64_t live_fibers;
  uint64_t steals_attempted;
  uint64_t steals_success;
  uint64_t inject_enqueues;
  uint64_t inject_drains;
  int      rebalance_enable;
  int      rebalance_threshold;
  int      rebalance_interval_ms;
  uint64_t rebalances_attempted;
  uint64_t rebalances_migrated;
} gof_sched_stats;

void gof_get_stats(gof_sched_stats *out);
```

### Runtime configuration

All parameters can also be set via environment variables (see below).

```c
// Work-stealing
typedef struct { int enable_steal; int steal_min_live; int steal_min_victim; } gof_steal_params;
void gof_set_steal_params(const gof_steal_params *p);
void gof_get_steal_params(gof_steal_params *out);

// Periodic rebalancer
typedef struct { int enable; int threshold; int interval_ms; } gof_rebalance_params;
void gof_set_rebalance_params(const gof_rebalance_params *p);
void gof_get_rebalance_params(gof_rebalance_params *out);

// Affinity routing
void gof_set_affinity_enabled(int enable);
int  gof_get_affinity_enabled(void);

// Netpollers (must be set before gof_init)
int  gof_set_npollers(int n);
int  gof_get_npollers(void);
```

### Trace hooks

Weak-symbol hooks called by the scheduler at context-switch and
park/unpark sites. Override them to wire up your own tracing:

```c
void gof_trace_on_switch(uint64_t old_id, uint64_t new_id);
void gof_trace_on_block(int fd, int ev);
void gof_trace_on_unblock(int fd, int ev);
```

Internal atomic counters (`gof_ctx_switches`, `gof_parks`, `gof_unparks`
in `debug/debug.h`) are incremented at the same sites for lightweight
observability without hooking.

## Environment Variables

Set before `gof_init()` to configure the runtime. All have sensible defaults.

| Variable | Default | Description |
|---|---|---|
| `GOF_NWORKERS` | auto | Worker threads (clamped 1–64) |
| `GOF_WORKSTEAL` | `1` | Enable work-stealing |
| `GOF_STEAL_MIN_LIVE` | `4` | Skip steal when victim has fewer runnables |
| `GOF_STEAL_MIN_VICTIM` | `3` | Victim must have ≥ N runnables (min 2) |
| `GOF_NPOLLERS` | `1` | Netpoll threads (capped by nworkers) |
| `GOF_POLL_PARTITION` | `1` | Partition-aware wakeups to poller-owned workers |
| `GOF_AFFINITY` | `1` | Prefer last-run worker for locality |
| `GOF_REBALANCE` | `0` | Enable periodic rebalancing |
| `GOF_REBALANCE_THRESHOLD` | `4` | Migrate when imbalance ≥ threshold (min 1) |
| `GOF_REBALANCE_INTERVAL_MS` | `10` | Rebalance check interval in ms (min 1) |

### CMake options

| Option | Default | Effect |
|---|---|---|
| `GOF_DEBUG` | `OFF` | Verbose scheduler logging |
| `GOF_ENABLE_TRACING` | `OFF` | Lightweight trace hook compilation |
| `GOF_PORTABLE_CONTEXT` | auto | Force `ucontext` fallback instead of asm |

## Directory Layout

```
fiber/
├── include/libgo/
│   ├── fiber.h              # Public API
│   └── fiber_chan.h          # Channel API
├── context/
│   ├── context.h             # Backend-neutral context types
│   ├── context_asm.c         # Bootstrap init (x86-64 + ARM64)
│   ├── context_x86_64.S      # x86-64 swap (macOS Mach-O)
│   ├── context_x86_64_linux.S# x86-64 swap (Linux ELF)
│   ├── context_arm64.S       # ARM64 swap (macOS Mach-O)
│   ├── context_arm64_linux.S # ARM64 swap (Linux ELF)
│   └── context_portable.c   # ucontext fallback
├── sched/
│   ├── sched.h               # Internal scheduler types (gof_fiber, gof_state)
│   ├── sched.c               # Core scheduler: workers, run queues, sleep heap,
│   │                         #   inject queue, work-stealing, idle loop
│   ├── api.c                 # Public API impl, pthread_once init, background mode
│   ├── park.h / park.c       # Park/unpark bridge for timers + netpoll
│   └── fiber_hooks_impl.c   # libgo hook integration (block/unblock/current)
├── io/
│   ├── netpoll.h             # Netpoll interface (arm, wait, close, callback)
│   ├── io.c                  # Fiber I/O wrappers, fd waiter table, timeout support
│   ├── netpoll_epoll.c       # Linux epoll backend (64-event batch)
│   ├── netpoll_kqueue.c      # macOS kqueue backend (64-event batch)
│   └── netpoll_iocp.c        # Windows IOCP stub
├── chan/
│   └── chan.c                # Bounded MPMC channel with done-flag handoff
├── stack/
│   ├── stack.h               # Stack allocation types
│   └── stack.c               # mmap/VirtualAlloc + guard page allocation
├── timers/
│   ├── timer_bridge.h        # Internal sleep-ns helper
│   └── timer_bridge.c        # Bridges gof_sleep_ms → scheduler park_until
├── debug/
│   ├── debug.h               # Atomic trace counters + introspect registry
│   ├── trace.c               # Weak trace hook defaults + counter definitions
│   └── introspect.c          # Fiber list, dump_stacks, set_name
├── tests/
│   ├── test_basic.c          # Spawn + yield correctness
│   ├── test_context.c        # Context switch ordering
│   ├── test_chan.c            # Channel producer/consumer
│   ├── test_io.c             # Pipe read/write through netpoller
│   ├── test_io_timeout.c     # Connect timeout via IO layer
│   └── test_starvation.c     # Fair scheduling under contention
└── bench/
    ├── bench_yield.c          # Cooperative yield overhead
    ├── bench_pingpong.c       # Fiber-to-fiber latency via channel
    ├── bench_sleep.c          # Timed sleep accuracy
    └── bench_poll_partition.c # IO throughput: partitioned vs non-partitioned
```

## Build

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Tests

Fiber tests use CTest with the `GoFiber*` label prefix:

```bash
ctest --test-dir build -R "GoFiber" --output-on-failure -j4
```

| Test | Timeout | What it covers |
|---|---|---|
| `GoFiberBasicTest` | 10 s | Spawn, yield, atomic counter |
| `GoFiberContextTest` | 10 s | Context switch sequencing |
| `GoFiberChanTest` | 15 s | Channel send/recv + close |
| `GoFiberIoTest` | 10 s | Pipe I/O through netpoller |
| `GoFiberIoTimeoutTest` | 2 s (10 s on CI) | Connect timeout path |
| `GoFiberStarvationTest` | 20 s | Fair scheduling under yield storms |

### Platform support

| Platform | Context backend | Netpoll backend | Status |
|---|---|---|---|
| Linux x86-64 | Assembly | epoll | ✅ Full |
| Linux ARM64 | Assembly | epoll | ✅ Full |
| macOS x86-64 | Assembly | kqueue | ✅ Full |
| macOS ARM64 | Assembly | kqueue | ✅ Full |
| Any POSIX | ucontext fallback | epoll/kqueue | ✅ Portable |
| Windows | — | IOCP | ⚠️ Stub only |

## Benchmarks

Microbenchmarks live under `fiber/bench/`. Build targets are created
automatically. Example usage:

```bash
# Compare partitioned vs non-partitioned wakeups
export GOF_NWORKERS=8
export GOF_NPOLLERS=4

# Partitioned on (default)
./build/libgo/gof_bench_poll_partition -conns 256 -duration_ms 2000 -msg_size 64

# Partitioned off
GOF_POLL_PARTITION=0 ./build/libgo/gof_bench_poll_partition -conns 256 -duration_ms 2000 -msg_size 64

# Disable affinity
GOF_AFFINITY=0 ./build/libgo/gof_bench_poll_partition -conns 256 -duration_ms 2000 -msg_size 64

# Enable periodic rebalancer
GOF_REBALANCE=1 GOF_REBALANCE_THRESHOLD=4 GOF_REBALANCE_INTERVAL_MS=10 \
  ./build/libgo/gof_bench_poll_partition -conns 256 -duration_ms 2000 -msg_size 64
```

Other benchmarks:

```bash
./build/libgo/gof_bench_yield      # raw yield latency
./build/libgo/gof_bench_pingpong   # channel round-trip
./build/libgo/gof_bench_sleep      # timer accuracy
```

## Concurrency Model

The scheduler uses a **cooperative** preemption model — fibers must
explicitly yield, sleep, or perform I/O to allow other fibers to run.
There is no forced preemption or signal-based interruption.

**Thread safety guarantees:**

- Initialization is protected by `pthread_once` (safe to call `gof_init`
  from multiple threads).
- Fiber state transitions use `atomic_compare_exchange_strong` to prevent
  double-enqueue when a timer and I/O event fire concurrently for the
  same fiber.
- The sleep heap uses a dedicated mutex (`sleepheap.mu`) separate from the
  global scheduler mutex, reducing contention.
- Trace counters are `_Atomic uint64_t` — safe to read from any thread
  without locking.
- `gof_dump_stacks` serializes output internally to avoid interleaved
  writes from concurrent callers.
