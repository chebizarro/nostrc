# libgo Fiber Runtime Documentation

This section documents the fiber runtime in `libgo`, including architecture, context-switch backends, stack management with guard pages, the scheduler, public APIs, debugging, and contribution guidelines.

If you are new to the project, start here and then follow the links to topic pages.

## Overview

- Non-preemptive (cooperative) fibers scheduled by a work-stealing runtime.
- Assembly context backends for macOS and Linux on arm64 and x86_64.
- Portable fallback context backend for unsupported platforms (short-term Windows strategy).
- Fiber stacks include dual guard pages to reliably catch over/underflow.
- Netpoll integration for I/O using kqueue/epoll/IOCP per platform.

## Key Components

- Context switching backends: `libgo/fiber/context/`
- Scheduler core: `libgo/fiber/sched/`
- Stacks and guards: `libgo/fiber/stack/`
- Public API headers: `libgo/fiber/include/libgo/`
- Tests: `libgo/fiber/tests/`
- Benchmarks: `libgo/fiber/bench/`

## Quick Start

```c
#include "libgo/fiber.h"

static void worker(void *arg) {
  for (int i = 0; i < 10; ++i) {
    gof_yield();
  }
}

int main(void) {
  gof_init(128 * 1024);
  for (int i = 0; i < 100; ++i) {
    gof_spawn(worker, NULL, 0); // 0 = default stack size
  }
  gof_run();
  return 0;
}
```

## Topics

- Context backends: context switching, trampolines, ABI and build flags
- Context switch lifecycle: bootstrap -> trampoline -> entry
- Stacks and guard pages: allocation, red zones, safety
- Scheduler: work-stealing design and runtime knobs
- API reference: fiber functions and configuration
- Debugging: sanitizers, trace hooks, and profiling
- Contributing: coding standards, tests, and CI

See the topic pages in this directory for details.

## CI & Sanitizers

- GitHub Actions runs sanitizer builds for libgo across macOS and Linux.
- Dedicated Linux GoFiber jobs ensure `GOF_PORTABLE_CONTEXT=OFF` and run only fiber tests under ASan/UBSan and TSan.
- macOS is configured to forbid the deprecated portable backend; assembly backends are always used there.

## Developer UX

### Build libgo with sanitizers

ASan + UBSan (recommended during development):

```bash
cmake -S libgo -B libgo/build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DGO_ENABLE_ASAN=ON -DGO_ENABLE_UBSAN=ON -DGO_WARNINGS_AS_ERRORS=ON \
  -DGOF_PORTABLE_CONTEXT=OFF
cmake --build libgo/build -j
```

ThreadSanitizer (for data races):

```bash
cmake -S libgo -B libgo/build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DGO_ENABLE_TSAN=ON -DGO_WARNINGS_AS_ERRORS=ON \
  -DGOF_PORTABLE_CONTEXT=OFF
cmake --build libgo/build -j
```

### Run tests

Run entire suite:

```bash
ctest --test-dir libgo/build --output-on-failure -j
```

Run only fiber tests:

```bash
ctest --test-dir libgo/build -R GoFiber --output-on-failure -j
```

### Debugging tips

- ASan/UBSan env (set before running tests):

```bash
export ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1:malloc_context_size=20
export UBSAN_OPTIONS=print_stacktrace=1
```

- TSan env:

```bash
export TSAN_OPTIONS=halt_on_error=1
```

- Leak checks: For long-running binaries that don’t exit cleanly, consider `ASAN_OPTIONS=detect_leaks=0` during CI runs.
- Symbols: Build Debug and avoid stripping to retain precise backtraces.
- CTest filters: Use `ctest -R GoFiber` to isolate fiber runtime tests.
