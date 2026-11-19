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

### Core channel integration and sanitizers

`gof_init()` explicitly registers the channel/select parking callbacks in
libgo, along with the existing fiber-spawn callback. The core `gof_hook_*`
symbols dispatch through that immutable, process-lifetime table, rather than
relying on weak-symbol overrides. Either archive order therefore selects the
same runtime behavior. The CMake target declares its core-libgo dependency,
including for spawn-only consumers; raw archive links must likewise put core
libgo after go_fiber (or repeat/group the archives) for dependency resolution.
Pthreads outside fibers continue to use OS-thread waits.

TSan builds (`-DCMAKE_BUILD_TYPE=Debug -DGO_ENABLE_TSAN=ON`) create one sanitizer
handle per fiber, capture each worker's original handle, annotate dispatch,
yield, blocking, timed parking and completion, and destroy finished handles on
the scheduler stack. Switches use `__tsan_switch_to_fiber_no_sync`: switching
alone is not application synchronization; the queue locks and wait protocols
supply ordering. The annotations cover assembly and portable context backends.
The portable swap wrapper must not add TSan function-entry/exit instrumentation
between selecting the destination handle and switching stacks.

`GoFiberHookLinkorderTest` links libgo first and verifies an empty core-channel
receive releases the only worker to a sending fiber.
`GoFiberSpawnLinkorderTest` covers a consumer without channel/select references.
`GoFiberTsanMigrationTest` runs 64 fibers on four workers for three seconds, checks actual migration, and
combines yield, sleep and core-channel ping-pong. Repeat teardown under TSan:

```sh
ctest --test-dir build-libgo-tsan -R '^GoFiberTsanMigrationTest$' --repeat until-fail:30 --output-on-failure
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

`gof_start_background()` keeps the worker pool alive even when there are no
fibers. OS threads can submit work later with `go_fiber()` or `gof_spawn()`;
no sentinel/keep-alive fiber or startup ordering workaround is needed.
`gof_run()` instead blocks until all fibers complete and then returns, including
when started empty. Never run the two scheduler modes concurrently.

Stop submitting new work, cancel application/channel work, call
`gof_request_stop()`, then call `gof_join_background()` from an OS thread.
Stop wakes pending I/O with `-1` / `ECANCELED`; it does not preempt a running
fiber or cancel arbitrary application waits. Join does not return just because
all fibers have completed: background mode requires an explicit stop. A new
start is rejected through the end of join; afterward the scheduler can restart
or run in foreground mode.

### Fiber-friendly I/O

Nonblocking wrappers that park the calling fiber on the netpoller and resume
it when the fd becomes ready (or a timeout expires). A wait without a deadline
parks indefinitely, not in 2 ms slices. Waiters are registered under the I/O
mutex **before** the poller is armed; readiness before the actual park is
retained by the scheduler's RUNNABLE/PARKING → WOKEN handshake. All matching
waiters receive a readiness edge, including peer EOF/hangup. Keep a waiting fd
open: raw `close(fd)` from another thread is not a supported cancellation path;
close the peer or stop/cancel the operation before closing its descriptor.

The dedicated poller already uses an indefinite kernel wait. Untimed I/O now
adds no sleep-heap entry, so idle workers also wait indefinitely. Injection,
readiness, new deadlines and stop signal the existing worker condition variable.
Timed operations retain one absolute monotonic deadline across retries, and
completed/cancelled I/O removes its deadline from the heap.

```c
/* gof_socket_t: int on POSIX, SOCKET on Windows (see "Windows" below). */
ssize_t      gof_read(gof_socket_t fd, void *buf, size_t n);
ssize_t      gof_write(gof_socket_t fd, const void *buf, size_t n);
int          gof_connect(gof_socket_t fd, const struct sockaddr *sa, socklen_t slen, int timeout_ms);
gof_socket_t gof_accept(gof_socket_t fd, struct sockaddr *sa, socklen_t *slen, int timeout_ms);
int          gof_io_reset_fd(gof_socket_t fd);
int          gof_io_cancel_fd(gof_socket_t fd);
int          gof_socket_errno(void); /* errno for a failed raw socket call */
ssize_t      gof_recvfrom(gof_socket_t fd, void *buf, size_t n, int flags,
                          struct sockaddr *sa, socklen_t *slen, int timeout_ms);
ssize_t      gof_sendto(gof_socket_t fd, const void *buf, size_t n, int flags,
                        const struct sockaddr *sa, socklen_t slen, int timeout_ms);
ssize_t      gof_recvmsg(gof_socket_t fd, struct msghdr *msg, int flags, int timeout_ms);
ssize_t      gof_sendmsg(gof_socket_t fd, const struct msghdr *msg, int flags, int timeout_ms);
```

The message wrappers must be called from a running fiber. They set `O_NONBLOCK`
on the fd (and its aliases) and pass flags, addresses, iovecs and ancillary data
through to the OS. On `EAGAIN`/`EWOULDBLOCK` they park and retry against one
monotonic deadline: `timeout_ms < 0` waits indefinitely, `0` attempts once without
waiting, and positive values bound readiness retries in milliseconds. Expiry
returns `-1` with `errno = ETIMEDOUT`; other errors follow the syscall. A ready
operation can still succeed with a zero timeout. `gof_sendto` and `gof_sendmsg`
return after **one successful syscall**, even for a short or zero-length send;
unlike `gof_write`, they never split a message or resend its control data.
`gof_recvmsg` exposes the OS's `msg_flags` (including truncation) and control data,
so callers retain the usual responsibility to inspect them and close received
`SCM_RIGHTS` fds. The Windows implementations return `-1`/`ENOTSUP`.

For owner-directed teardown, prepare a newly owned fd with `gof_io_reset_fd`
(which may fail allocation), then use `gof_io_cancel_fd` to latch `ECANCELED`
for existing and future readiness waits. This includes cancellation between
registration and park, without requiring socket shutdown, a live interface or
a periodic receive timeout. Successful immediate syscalls may still finish;
the caller checks its stop state after return. Detach the fd from new users,
join **every** existing user, then reset the latch before closing/reusing the
descriptor. Reset while a waiter remains queued returns `EBUSY`; readiness
already claimed is not proof that a user has joined. Descriptor aliases are
not tracked. The weak `gof_trace_on_io_armed` hook marks the registered/pre-park
boundary; like the other hooks it must not yield or call fiber APIs.

Two POSIX-only calls (not declared on Windows) serve a writer whose own syscall
cannot park, such as a macOS `AF_UNIX SOCK_DGRAM` sender refused with `ENOBUFS`
(nostrc-e3ps5):

```c
int gof_wait_writable(gof_socket_t fd, size_t lowat, int timeout_ms);
int gof_io_wake_writers(gof_socket_t fd);
```

`gof_wait_writable` parks until `fd` reports at least `lowat` bytes of send
space (0: any writability), a `gof_io_wake_writers`, the deadline (`timeout_ms`
< 0 waits indefinitely, > 0 bounds it, 0 is `EINVAL`), owner cancellation or
stop. `lowat > 0` needs a per-registration low-water mark: the kqueue backend
registers `EVFILT_WRITE` with `NOTE_LOWAT`, which XNU evaluates on a connected
`AF_UNIX` socket against the peer's receive space; epoll returns `ENOTSUP`. The
mark is kept per fd for the write queue (the lowest among parked writers) and
re-applied on every re-arm, so a read wake on the same fd never drops it.
`gof_io_wake_writers` wakes the parked writers as if the fd became writable, or
latches one wake for the next writability wait when none is parked, so a wake
issued just before the writer parks is not lost; `gof_io_reset_fd` clears the
latch. Both treat readiness as a hint: retry the write.

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
| `GoFiberIoIdleTestW1/W4` | 60 s | Zero resumes over 2 s idle, bounded write latency, 10k parking races, shared-fd EOF |
| `GoFiberBackgroundLifecycleTestW1/W4` | 60 s | 50 delayed submissions into empty pools, prompt stop/join, timed and untimed I/O cancellation |
| `GoFiberIoTimeoutTest` | 2 s (10 s on CI) | Connect timeout path |
| `GoFiberIoLoopbackTest{,W4}` | 60 s | Real UDP/TCP loopback: message exchange, truncation, deadline, accept/connect/stream/EOF, cancel mid-receive, 300-round cancel-vs-arrival race (every platform, incl. IOCP) |
| `GoFiberStarvationTest` | 20 s | Fair scheduling under yield storms |

### Platform support

| Platform | Context backend | Netpoll backend | Status |
|---|---|---|---|
| Linux x86-64 | Assembly | epoll | ✅ Full |
| Linux ARM64 | Assembly | epoll | ✅ Full |
| macOS x86-64 | Assembly | kqueue | ✅ Full |
| macOS ARM64 | Assembly | kqueue | ✅ Full |
| Any POSIX | ucontext fallback | epoll/kqueue | ✅ Portable |
| Windows ARM64 (MSYS2 CLANGARM64) | Fibers API | IOCP | ✅ Full (45/45 libgo tests) |
| Windows x86-64 | Fibers API | IOCP | Same code; not yet run |

### Windows

`go_fiber` builds on Windows with MSYS2 clang (CLANGARM64 verified; the code
has no architecture-specific parts) and links `ws2_32` and `mswsock`. Threads
and locks are winpthreads; nsync is the MSYS2 `nsync` package or the vendored
copy, as on the other platforms.

**Context switch.** `context/context_win32.c` uses the Windows Fibers API
(`CreateFiberEx`, `SwitchToFiber`, `ConvertThreadToFiberEx` for each worker's
own context, `DeleteFiber` when a finished fiber is reaped). A hand-written
ARM64 swap would have to keep x18 (the TEB pointer) untouched and swap the TEB's
`StackBase`/`StackLimit`/`DeallocationStack` and exception list with every fiber
stack, or guard-page stack growth, `__chkstk` probes and SEH unwinding fault on
the fiber stack; `SwitchToFiber` does exactly that bookkeeping. The fiber owns
its stack (reserved by `CreateFiberEx` in 64 KiB units, grown on demand through
its guard page), so `gof_stack_alloc` only records the size there. Fibers
migrate between workers as on POSIX; `errno` and other TLS belong to the
worker thread, so the noinline errno helpers apply unchanged.

**Submission.** There is no readiness poller: every `gof_*` call is an
overlapped Winsock operation on one process-wide I/O completion port
(`io/netpoll_iocp.c`, which also performs the process's `WSAStartup`).
`io/io_win32.c` registers an op record (the `OVERLAPPED` plus state, on the
calling fiber's stack) with the socket's registry entry under `io_mu`, issues
`WSARecv`/`WSASend` (stream reads, writes and stream `recvmsg`/`sendmsg`),
`WSARecvFrom`/`WSASendTo`, `WSARecvMsg`/`WSASendMsg` (datagram sockets),
`ConnectEx` (binding an unbound socket first; datagram `connect` stays plain)
or `AcceptEx` (the accepted socket is created with the listener's protocol
info), and parks until the completion packet is dequeued. Extension functions
are loaded once through `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER)`.

Every operation first probes the socket's port association with
`CreateIoCompletionPort`: Windows recycles a closed `SOCKET` value at once, so
cached state (socket type, completion mode) is trusted only while the port
reports the handle as already bound; a newly bound handle is prepared again
with `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE`.
With that mode an inline success returns without a packet or a park. Probed on
Windows 11 ARM64: a synchronous failure other than `WSA_IO_PENDING` queues no
packet (`OVERLAPPED.Internal` stays `STATUS_PENDING`, so only the return code
decides), and a truncated datagram always completes through the port.

**Completion.** A poller thread drains the port with
`GetQueuedCompletionStatusEx` and, under `io_mu`, unlinks the op, marks it done
and wakes its fiber through the scheduler's claim protocol (a packet dequeued
before the fiber parks claims RUNNABLE→WOKEN, so no wake is lost). The fiber
then reads status, byte count and flags with `WSAGetOverlappedResult`; for
`WSARecvMsg` the provider updates `namelen`, `Control.len` and `dwFlags` in
the `WSAMSG` at completion.

**Cancellation.** A deadline, `gof_io_cancel_fd()` and scheduler stop all call
`CancelIoEx` on the pending op. The kernel still delivers exactly one packet,
aborted or with the result when the operation won the race, and the fiber never
returns before that packet is dequeued, so no `OVERLAPPED`, `WSAMSG`, `WSABUF`
or data buffer outlives its call. A receive that completed despite the cancel
returns its data (the datagram is consumed and delivered); an aborted one
leaves the datagram queued for the next receive. `GoFiberIoLoopbackTest` races
cancel against arrival 300 times and checks that every datagram is delivered
exactly once. A cancel that lands before the op is listed is honoured when it
is listed. The latch semantics match POSIX: `cancel_fd` latches ECANCELED until
`reset_fd`; `reset_fd` returns EBUSY while an op is registered.

**Messages.** `libgo/net_compat.h` (included by `libgo/fiber.h` on Windows)
defines POSIX-shaped `struct msghdr` and `struct iovec` with the POSIX field
names, and `CMSG_FIRSTHDR/NXTHDR/DATA/SPACE/LEN/ALIGN` over `struct cmsghdr`
(`WSACMSGHDR`) with the `WSA_CMSG_*` alignment rules (header and data aligned
to `SIZE_T`), replacing Winsock's WSAMSG-only aliases. libgo translates to
`WSAMSG`/`WSABUF`; the kernel writes control messages straight into
`msg_control`. Windows offers `IP_PKTINFO`/`IPV6_PKTINFO` (receive and source
selection) and `IP_ECN`/`IPV6_ECN` (receive, after `IP_RECVECN`); anything else
(for example `SO_RXQ_OVFL` drop counters) is simply absent. Stream sockets
carry no ancillary data (`gof_sendmsg` with control data on a stream returns
EOPNOTSUPP) because `WSARecvMsg`/`WSASendMsg` are datagram-only.

**Errors.** Winsock errors map to the POSIX errno values the CRT defines;
`gof_socket_errno()` applies the same mapping to raw socket calls.

| Winsock / Win32 | errno |
|---|---|
| `WSAEWOULDBLOCK` | `EAGAIN` |
| `WSAECONNRESET`, `ERROR_NETNAME_DELETED` | `ECONNRESET` |
| `WSAECONNREFUSED`, `ERROR_CONNECTION_REFUSED`, `ERROR_PORT_UNREACHABLE` | `ECONNREFUSED` |
| `WSAEMSGSIZE`, `ERROR_MORE_DATA` | `EMSGSIZE` (send); truncation success with `MSG_TRUNC` (receive) |
| `WSA_OPERATION_ABORTED` | `ECANCELED` after `cancel_fd`/stop, `ETIMEDOUT` after this call's deadline, `EINVAL` when it pended outside a fiber |
| `WSAETIMEDOUT`, `ERROR_SEM_TIMEOUT` | `ETIMEDOUT` |
| `WSAENETUNREACH`, `ERROR_NETWORK_UNREACHABLE` | `ENETUNREACH` |
| `WSAEHOSTUNREACH`, `WSAEHOSTDOWN`, `ERROR_HOST_UNREACHABLE` | `EHOSTUNREACH` |
| `WSAECONNABORTED`, `ERROR_CONNECTION_ABORTED` | `ECONNABORTED` |
| `WSAENOTCONN` / `WSAEISCONN` / `WSAESHUTDOWN` | `ENOTCONN` / `EISCONN` / `EPIPE` |
| `WSAENOTSOCK` / `WSAEBADF`, `INVALID_SOCKET` | `ENOTSOCK` / `EBADF` |
| `WSAEADDRINUSE` / `WSAEADDRNOTAVAIL` / `WSAEAFNOSUPPORT` / `WSAEOPNOTSUPP` | `EADDRINUSE` / `EADDRNOTAVAIL` / `EAFNOSUPPORT` / `EOPNOTSUPP` |
| `WSAEINVAL` / `WSAEFAULT` / `WSAENOBUFS` / `WSAEINTR` / `WSAEACCES` / `WSAEMFILE` | `EINVAL` / `EFAULT` / `ENOBUFS` / `EINTR` / `EACCES` / `EMFILE` |
| anything else | `EIO` |

**Differences from POSIX.**
- `gof_read`/`gof_write` work on sockets only (`WSARecv`/`WSASend`); pipes and
  files return ENOTSOCK. Tests use loopback socket pairs (`fiber/tests/test_net.h`).
- Timeouts bound the overlapped operation: `timeout_ms == 0` issues it and
  cancels at once if it pends (ETIMEDOUT when the cancel wins; UDP sends
  complete inline and never pend in practice). A cancelled
  stream send may have sent part of its buffer, as Winsock documents.
- An unconnected UDP socket reports an ICMP port-unreachable as
  `WSAECONNRESET` on its next receive unless the owner disables
  `SIO_UDP_CONNRESET` (fipsd does).
- A call that would park outside a fiber cancels and returns EINVAL, as on
  POSIX; one that completes inline succeeds.
- Owners still follow the POSIX contract: join users before closing; do not
  close a socket with an operation pending (closing aborts it).
- TSan and the sanitizers are POSIX-only here.

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
