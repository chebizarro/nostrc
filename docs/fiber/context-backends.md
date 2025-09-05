# Context Backends

This page documents the fiber context switching backends under `libgo/fiber/context/`.

## Overview

- macOS arm64 and x86_64: Non-deprecated, assembly-based backends.
- Linux arm64 and x86_64: Assembly-based backends following AAPCS64 and SysV ABIs.
- Portable fallback: ucontext-based for other platforms (may be deprecated on some OSes; short-term strategy on Windows).
- Build toggle: `GOF_PORTABLE_CONTEXT=OFF` selects assembly on supported targets; `ON` forces portable.

## Files

- `libgo/fiber/context/context.h`
  - Declares `gof_context` layout with arch-specific fields.
- `libgo/fiber/context/context_arm64.S`
  - macOS arm64: `gof_ctx_swap`, `gof_trampoline` (Darwin/Mach-O symbols have leading underscore).
- `libgo/fiber/context/context_x86_64.S`
  - macOS x86_64: `gof_ctx_swap`, `gof_trampoline` (Darwin/Mach-O symbols have leading underscore).
- `libgo/fiber/context/context_arm64_linux.S`
  - Linux arm64: `gof_ctx_swap`, `gof_trampoline` (ELF symbols, no underscore).
- `libgo/fiber/context/context_x86_64_linux.S`
  - Linux x86_64: `gof_ctx_swap`, `gof_trampoline` (ELF symbols, no underscore).
- `libgo/fiber/context/context_asm.c`
  - `gof_ctx_init_bootstrap()` initializer for assembly contexts.
- `libgo/fiber/context/context_portable.c`
  - Portable `getcontext/makecontext/swapcontext` implementation.

## ABI Notes

- Entry prototype: `typedef void (*gof_fn)(void *arg);`
- Trampoline loads `entry` and `arg` from `gof_context` and calls `entry(arg)`.
- Callee-saved registers and stack pointer are saved/restored on swap.
- x86_64 uses `r12` to hold the `gof_context*` across the trampoline.
- arm64 uses `x19` to hold the `gof_context*` across the trampoline.

## Stack Alignment

- Stacks are 16-byte aligned on both arm64 and x86_64 to respect the system ABI.
- The initializer (`gof_ctx_init_bootstrap`) adjusts the initial SP and sets a return address to the trampoline.

## Build Configuration

- CMake option: `-DGOF_PORTABLE_CONTEXT=OFF` prefers assembly backends.
- Platform selection (from `libgo/CMakeLists.txt`):
  - macOS + arm64 → `context_arm64.S` + `context_asm.c`
  - macOS + x86_64 → `context_x86_64.S` + `context_asm.c`
  - Linux + arm64 → `context_arm64_linux.S` + `context_asm.c`
  - Linux + x86_64 → `context_x86_64_linux.S` + `context_asm.c`
  - Else → portable fallback `context_portable.c`
- Darwin guard: configuring with `GOF_PORTABLE_CONTEXT=ON` on macOS fails with a clear error to avoid deprecated ucontext on Apple.

## Testing

- Unit tests under `libgo/fiber/tests/` cover context init/swap basics.
- Additional starvation test (`gof_test_starvation`) stresses cooperative scheduling and yield fairness.

## CI Coverage

- GitHub Actions builds libgo with sanitizers on macOS and Linux.
- A dedicated Linux job runs only the GoFiber tests with `GOF_PORTABLE_CONTEXT=OFF` to validate the assembly backends.

## Windows strategy (short term → future)

- Short term: use the portable `context_portable.c` backend on Windows targets.
- Future direction (preferred):
  - Implement a Windows-specific backend using the Windows Fibers API (`ConvertThreadToFiber`, `CreateFiber`, `SwitchToFiber`), or
  - Provide a custom assembly backend that respects SEH/unwind metadata for correct stack unwinding and debugging.
- The API surface in `context.h` and the initializer in `context_asm.c` were designed to allow introducing a Windows-native backend without changing the public fiber API.
